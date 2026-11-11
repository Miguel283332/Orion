#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Demostrador - resuelve problemas matemáticos y físicos con un LLM local (Ollama) y un verificador externo.

Un solo archivo, solo biblioteca estándar. El modelo PROPONE; el verificador DECIDE:
  * backend "lean"  : demostraciones formales en Lean 4 (matemáticas). Lean es el juez.
  * backend "sympy" : derivaciones físicas/matemáticas con SymPy; se comprueban criterios fijados antes de resolver.

Pensado para problemas LARGOS y para poder INTERRUMPIRLO:
  * todo el progreso (árbol de demostración, intentos, errores) se guarda tras CADA intento (escritura atómica);
  * al volver a ejecutar el mismo comando continúa exactamente donde se quedó;
  * si un objetivo no sale, lo descompone en lemas (recursivamente) y conserva los que ya demostró;
  * presupuesto de tiempo/intentos (--horas, --max-intentos) y parada limpia con Ctrl-C / SIGTERM.

Uso:
  python demostrador.py resolver --problema problemas/par_mayor.json
  python demostrador.py resolver --enunciado "Demuestra que ..." --backend lean --dir mi_ejecucion
  python demostrador.py estado   --dir mi_ejecucion
  python demostrador.py selftest
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import traceback
import unicodedata
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

VERSION = "4.0.0"
ESTADO_VERSION = 6
MODELO_DEFECTO = "qwen2.5:7b"
AXIOMAS_PERMITIDOS = {"propext", "Classical.choice", "Quot.sound"}


# =============================================================================
# 1. CONFIGURACIÓN
# =============================================================================
class Config:
    def __init__(self, **kw: Any) -> None:
        self.modelo = MODELO_DEFECTO
        self.ollama_url = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434")
        self.num_ctx = 8192
        self.num_predict = 1600
        self.backend = "lean"
        self.lean = ""              # ruta a lean (vacío = buscar en PATH y ~/.elan/bin)
        self.lake_dir = ""          # proyecto lake con Mathlib: se usa `lake env lean` dentro de él
        self.mathlib = False
        self.timeout_lean = 90
        self.timeout_py = 120
        self.formalizar = 8         # intentos de formalizar el enunciado
        self.directos = 6           # intentos directos por objetivo
        self.finales = 6            # intentos del objetivo tras demostrar sus lemas
        self.planes = 4             # intentos de obtener un plan válido
        self.replanes = 3           # replanificaciones por objetivo
        self.max_lemas = 4
        self.profundidad = 2        # niveles de descomposición recursiva
        self.fidelidad = True       # comprobar que el enunciado formal dice lo que pide el problema
        self.horas = 0.0            # 0 = sin límite de tiempo
        self.max_intentos = 0       # 0 = sin límite de llamadas al modelo
        self.semilla = 1
        for k, v in kw.items():
            if not hasattr(self, k):
                raise TypeError(f"opción desconocida: {k}")
            setattr(self, k, v)


class Pausa(Exception):
    """Se alcanzó el presupuesto (tiempo/intentos) o se pidió parar: el estado ya está guardado."""


class ErrorModelo(Exception):
    pass


def ahora() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")


# =============================================================================
# 2. UTILIDADES
# =============================================================================
def ensamblar(*partes: Any) -> str:
    return "\n\n".join(p.strip() for p in partes if isinstance(p, str) and p.strip())


def normalizar(texto: str) -> str:
    # Sin NFKC a propósito: NFKC convertiría ℕ en "N" antes de reconocerlo, y la versión C++ no dispone de él;
    # así Python y C++ calculan la misma huella y pueden compartir el estado guardado.
    t = (texto or "").replace("ℕ", "Nat").replace("ℤ", "Int").replace("ℝ", "Real").replace("→", "->")
    t = t.replace("≤", "<=").replace("≥", ">=")
    return re.sub(r"\s+", " ", t).strip()


def huella(texto: str) -> str:
    return hashlib.sha256(normalizar(texto).encode("utf-8")).hexdigest()[:16]


def recortar(texto: str, n: int) -> str:
    texto = str(texto)
    return texto if len(texto) <= n else texto[: n - 15] + f"... [+{len(texto) - n + 15}]"


def quitar_comentarios_lean(codigo: str) -> str:
    codigo = re.sub(r"/-.*?-/", "", codigo, flags=re.S)
    return re.sub(r"--.*", "", codigo)


def quitar_comentarios_py(codigo: str) -> str:
    return re.sub(r"#.*", "", codigo)


def escribir_atomico(ruta: Path, texto: str) -> None:
    ruta.parent.mkdir(parents=True, exist_ok=True)
    tmp = ruta.with_name(ruta.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(texto)
        f.flush()
        try:
            os.fsync(f.fileno())
        except OSError:
            pass
    os.replace(tmp, ruta)


def extraer_json(texto: str) -> Dict[str, Any]:
    t = (texto or "").strip()
    t = re.sub(r"^```(?:json)?\s*|\s*```$", "", t, flags=re.S).strip()
    try:
        obj = json.loads(t)
        if isinstance(obj, dict):
            return obj
    except json.JSONDecodeError:
        pass
    i = t.find("{")
    if i < 0:
        raise ValueError("el modelo no devolvió JSON")
    prof, instr, esc = 0, False, False
    for k in range(i, len(t)):
        c = t[k]
        if instr:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == "{":
            prof += 1
        elif c == "}":
            prof -= 1
            if prof == 0:
                obj = json.loads(t[i:k + 1])
                if isinstance(obj, dict):
                    return obj
                break
    raise ValueError("el modelo no devolvió un objeto JSON válido")


def bloques_codigo(texto: str, lenguajes: Tuple[str, ...]) -> List[str]:
    pat = r"```(?:" + "|".join(lenguajes) + r")?[ \t]*\n(.*?)```"
    return [b.strip("\n") for b in re.findall(pat, texto or "", flags=re.S | re.I)]


def elegir_bloque(texto: str, lenguajes: Tuple[str, ...], pista: Callable[[str], bool]) -> str:
    """De varias vallas de código, la que cumple `pista` (la última que la cumple); si no, la más larga."""
    bs = bloques_codigo(texto, lenguajes)
    if not bs:
        return (texto or "").replace("```", "").strip("\n")
    buenos = [b for b in bs if pista(b)]
    return buenos[-1] if buenos else max(bs, key=len)


def sangrar(lineas: List[str], n: int = 2) -> str:
    return "\n".join((" " * n + l) if l.strip() else l for l in lineas)


# =============================================================================
# 3. CLIENTE DE OLLAMA
# =============================================================================
class LLM:
    """Cliente mínimo de la API de Ollama con reintentos. Nunca usa proxy (el servidor es local)."""

    def __init__(self, cfg: Config) -> None:
        raw = (cfg.ollama_url or "http://127.0.0.1:11434").strip()
        if "://" not in raw:
            raw = "http://" + raw
        self.base = raw.replace("://0.0.0.0", "://127.0.0.1").rstrip("/")
        self.modelo = cfg.modelo
        self.num_ctx = cfg.num_ctx
        self._op = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def _post(self, ruta: str, payload: Dict[str, Any], timeout: float = 1800.0, reintentos: int = 3) -> Dict[str, Any]:
        req = urllib.request.Request(self.base + ruta, data=json.dumps(payload).encode("utf-8"), method="POST",
                                     headers={"Content-Type": "application/json"})
        ultimo: Optional[BaseException] = None
        for i in range(reintentos + 1):
            try:
                with self._op.open(req, timeout=timeout) as r:
                    return json.loads(r.read().decode("utf-8"))
            except urllib.error.HTTPError as e:
                raise ErrorModelo(f"Ollama respondió HTTP {e.code}: {e.read().decode('utf-8', 'ignore')[:300]}")
            except (urllib.error.URLError, ConnectionError, socket.timeout, TimeoutError) as e:
                ultimo = e
                time.sleep(min(30, 2 * (i + 1)))
        raise ErrorModelo(f"No se puede hablar con Ollama en {self.base} ({ultimo}). ¿Está en marcha? (ollama serve)")

    def chat(self, sistema: str, usuario: str, *, temperatura: float = 0.2, num_predict: int = 1600, seed: int = 0,
             json_mode: bool = False) -> str:
        payload: Dict[str, Any] = {
            "model": self.modelo, "stream": False,
            "messages": [{"role": "system", "content": sistema}, {"role": "user", "content": usuario}],
            "options": {"temperature": temperatura, "num_predict": num_predict, "num_ctx": self.num_ctx, "seed": seed},
        }
        if json_mode:
            payload["format"] = "json"
        out = self._post("/api/chat", payload)
        return str((out.get("message") or {}).get("content", ""))

    def modelos(self) -> List[str]:
        req = urllib.request.Request(self.base + "/api/tags")
        with self._op.open(req, timeout=10) as r:
            return [m.get("name", "") for m in json.loads(r.read().decode("utf-8")).get("models", [])]


# =============================================================================
# 4. VERIFICADORES
# =============================================================================
class Verificador:
    """Interfaz de un verificador. El motor no sabe si por debajo hay Lean o SymPy."""
    nombre = ""
    extension = ""
    admite_plan = False
    sistema = ""

    def entorno(self) -> str:
        raise NotImplementedError

    def comprobar(self, codigo: str, nombre_final: str = "") -> Tuple[bool, str]:
        raise NotImplementedError

    def prohibido(self, texto: str) -> Optional[str]:
        return None

    def titulo_error(self, resumen: str) -> str:
        """Línea más informativa de un error, para el registro de progreso."""
        lineas = [l for l in (resumen or "").splitlines() if l.strip()]
        return lineas[0] if lineas else ""


# ------------------------------------------------------------------ Lean 4
LEAN_PROHIBIDO = re.compile(
    r"\b(sorry|admit|axiom|native_decide|unsafe|implemented_by|extern|opaque|macro|macro_rules|syntax|elab|"
    r"notation|infix|infixl|infixr|prefix|postfix|set_option|run_cmd|initialize|builtin_initialize)\b|#eval|#exit|#reduce|^\s*import\b",
    re.M)
LEAN_ORDENES = re.compile(r"^(theorem|lemma|example|def|abbrev|instance|structure|inductive|class|namespace|end|open|section|"
                          r"variable|universe|attribute|noncomputable|private|protected|#)\b")

PISTA_SIN_MATHLIB = ("PISTA: ese identificador no existe en Lean 4 sin Mathlib. Usa solo Nat/Int y operaciones básicas: "
                     "«m es par» se escribe `m % 2 = 0` (o `∃ k, m = 2 * k`) y «m es impar» `m % 2 = 1`. "
                     "NO existen Even, Odd, Nat.Prime, Finset, Real ni la notación ℕ (usa Nat).")
PISTA_MATHLIB = ("PISTA: revisa el nombre del identificador (p. ej. Even, Odd, Nat.Prime) o exprésalo con operaciones básicas "
                 "(`m % 2 = 0`).")
PISTA_SINTAXIS = "PISTA: error de sintaxis; revisa paréntesis, ⟨ ⟩, la sangría (todas las tácticas con la misma) y que cada táctica esté en su línea."

GUIA_LEAN = """REGLAS DE LEAN 4:
- Demuestra EXACTAMENTE el enunciado dado: no lo cambies, no añadas hipótesis, no inventes resultados.
- Prohibido: sorry, admit, axiom, native_decide, set_option, macros. Solo tácticas (:= by ...).
- Una táctica por línea, todas con la misma sangría.
TÁCTICAS ÚTILES:
- omega: aritmética lineal sobre Nat/Int (+, -, * por constantes, /, %, <, <=, =, y, o, no).
- simp, simp_all, decide, rfl, exact h, assumption, constructor (para y / si-y-solo-si).
- intro n / intros; obtain ⟨a, b⟩ := h; rcases h with ⟨a, b⟩ | c.
- Existenciales: `exact ⟨testigo, by omega⟩` (las tuplas planas SÍ valen: ∃ x, P x ∧ Q x se prueba con
  `exact ⟨t, by omega, by omega⟩` o con `refine ⟨t, ?_, ?_⟩`).
- Inducción: `induction n with | zero => ... | succ k ih => ...`.
- exact? busca un lema que cierre el objetivo (úsalo si no sabes cuál aplicar).
- SIN Mathlib no existen Even, Odd, Nat.Prime, Finset, Real: «par» es `m % 2 = 0`, «impar» `m % 2 = 1`."""


class LeanVerificador(Verificador):
    nombre = "lean"
    extension = "lean"
    admite_plan = True
    sistema = ("Eres un experto en matemáticas y en Lean 4. No inventes hipótesis ni resultados. "
               "No afirmes que algo funciona sin que el compilador lo haya comprobado.")

    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.mathlib = bool(cfg.mathlib or cfg.lake_dir)
        self.lean_bin = cfg.lean or shutil.which("lean") or str(Path.home() / ".elan" / "bin" / "lean")

    def entorno(self) -> str:
        cmd = self._cmd(Path("x.lean"))[:-1] + ["--version"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, cwd=self.cfg.lake_dir or None)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise RuntimeError(f"No se puede ejecutar Lean 4 ({self.lean_bin}): {e}")
        if r.returncode != 0:
            raise RuntimeError("Lean 4 no responde: " + (r.stdout + r.stderr)[:200])
        return (r.stdout or r.stderr).strip().splitlines()[0]

    def _cmd(self, archivo: Path) -> List[str]:
        if self.cfg.lake_dir:
            lake = shutil.which("lake") or str(Path.home() / ".elan" / "bin" / "lake")
            return [lake, "env", self.lean_bin, str(archivo)]
        return [self.lean_bin, str(archivo)]

    @property
    def encabezado(self) -> str:
        # autoImplicit desactivado: un identificador desconocido da "unknown identifier 'even'" (claro) en lugar de
        # convertirse en una variable implícita y producir "Function expected" (confuso para el modelo).
        return ("import Mathlib" if self.mathlib else "import Lean") + "\nset_option autoImplicit false"

    def tacticas_auto(self) -> List[str]:
        base = ["omega", "(simp; done)", "(simp_all; done)", "decide", "rfl", "(intros; omega)", "(intros; simp_all; done)",
                "(constructor <;> omega)", "(constructor <;> simp)"]
        if self.mathlib:
            base += ["(norm_num; done)", "linarith", "nlinarith [sq_nonneg _]", "positivity", "(ring; done)", "aesop"]
        return base + ["exact?"]

    def comprobar(self, codigo: str, nombre_final: str = "", permitir_sorry: bool = False) -> Tuple[bool, str]:
        if nombre_final:
            codigo = codigo.rstrip() + f"\n\n#print axioms {nombre_final}\n"
        with tempfile.TemporaryDirectory(prefix="demo_lean_") as td:
            f = Path(td) / "Problema.lean"
            f.write_text(codigo, encoding="utf-8")
            try:
                r = subprocess.run(self._cmd(f), capture_output=True, text=True, timeout=self.cfg.timeout_lean,
                                   cwd=self.cfg.lake_dir or td)
            except subprocess.TimeoutExpired:
                return False, f"Lean superó el límite de {self.cfg.timeout_lean}s."
            except OSError as e:
                return False, f"no se pudo ejecutar Lean: {e}"
        salida = ((r.stdout or "") + "\n" + (r.stderr or "")).strip()
        hay_error = r.returncode != 0 or re.search(r":\d+:\d+: error", salida) is not None
        if hay_error:
            return False, salida or "Lean falló sin mensaje."
        if not permitir_sorry and re.search(r"declaration uses [`'’]sorry[`'’]", salida):
            return False, "El código usa sorry (no es una demostración)."
        if nombre_final:
            m = re.search(r"depends on axioms: \[(.*?)\]", salida, flags=re.S)
            if m:
                usados = {a.strip() for a in m.group(1).split(",") if a.strip()}
                raros = usados - AXIOMAS_PERMITIDOS
                if raros:
                    return False, f"La demostración depende de axiomas no permitidos: {sorted(raros)}"
            elif "does not depend on any axioms" not in salida:
                return False, "No se pudo comprobar los axiomas de la demostración:\n" + recortar(salida, 400)
        return True, salida

    def prohibido(self, texto: str) -> Optional[str]:
        m = LEAN_PROHIBIDO.search(quitar_comentarios_lean(texto))
        return m.group(0).strip() if m else None

    # --- extracción
    def _codigo(self, texto: str) -> str:
        return elegir_bloque(texto, ("lean4", "lean"), lambda b: re.search(r"\b(theorem|lemma)\b|:=\s*by", b) is not None)

    def extraer_firma(self, texto: str) -> str:
        c = quitar_comentarios_lean(self._codigo(texto))
        m = re.search(r"\b(?:theorem|lemma)\s+\S", c)
        if not m:
            return ""
        c = c[m.start():]
        c = re.sub(r"^lemma\b", "theorem", c, count=1)
        i = c.find(":=")
        if i < 0:
            return ""
        firma = " ".join(c[:i].split())
        return (firma + " := by") if firma.startswith("theorem ") else ""

    def extraer_cuerpo(self, texto: str) -> str:
        c = self._codigo(texto)
        m = re.search(r"\b(?:theorem|lemma)\b.*?:=\s*by\b", c, flags=re.S)
        if m:
            resto = c[m.end():]
            lineas = resto.split("\n")
            primera = lineas[0].strip()
            cola: List[str] = []
            for l in lineas[1:]:
                if l.strip() and not l.startswith((" ", "\t")):   # primera línea sin sangría = otro comando
                    break
                cola.append(l)
            cola = textwrap.dedent("\n".join(cola)).split("\n") if cola else []
            cuerpo = ([primera] if primera else []) + [l for l in cola]
        else:
            m2 = re.search(r":=\s*by\b", c)
            resto = c[m2.end():] if m2 else c
            cuerpo = textwrap.dedent(resto).split("\n")
        while cuerpo and not cuerpo[-1].strip():
            cuerpo.pop()
        while cuerpo and not cuerpo[0].strip():
            cuerpo.pop(0)
        cuerpo = [l.rstrip() for l in cuerpo]
        return sangrar(cuerpo) if cuerpo else ""

    def nombre(self, firma: str) -> str:
        m = re.match(r"\s*theorem\s+([^\s:({\[]+)", firma)
        return m.group(1) if m else ""

    def renombrar(self, firma: str, nuevo: str) -> str:
        return re.sub(r"^\s*theorem\s+[^\s:({\[]+", f"theorem {nuevo}", firma, count=1)

    def clave(self, firma: str) -> str:
        t = re.sub(r"^\s*(?:theorem|lemma)\s+\S+", "", normalizar(firma))
        return re.sub(r"\s+", "", re.sub(r":=\s*by$", "", t))

    def enunciado_trivial(self, firma: str) -> Optional[str]:
        t = normalizar(firma)
        t = re.sub(r"^theorem\s+\S+", "", t)
        t = re.sub(r":=\s*by$", "", t).strip()
        if re.search(r":\s*True\s*$", t) or re.search(r"\(\s*\w+\s*:\s*False\s*\)", t) or t.endswith("= True"):
            return "el enunciado es trivial (True) o tiene una hipótesis imposible (False)"
        return None

    def bloque(self, firma: str, cuerpo: str) -> str:
        return f"{firma}\n{cuerpo}"

    def codigo(self, estado: Dict[str, Any], bloques: List[str], firma: str = "", cuerpo: str = "") -> str:
        return ensamblar(self.encabezado, estado.get("definiciones", ""), *bloques, f"{firma}\n{cuerpo}" if firma else "")

    def auto(self, estado: Dict[str, Any], firma: str, bloques: List[str]) -> Optional[str]:
        alts = self.tacticas_auto()
        cuerpo = "  first\n" + "\n".join(f"  | {a}" for a in alts)
        ok, _ = self.comprobar(self.codigo(estado, bloques, firma, cuerpo))
        return cuerpo if ok else None

    def resumir_error(self, salida: str) -> str:
        if not salida:
            return "error desconocido"
        trozos = re.split(r"(?m)^(?=\S+:\d+:\d+: (?:error|warning))", salida)
        errores = [t.strip() for t in trozos if re.match(r"\S+:\d+:\d+: error", t.strip())]
        base = "\n".join(errores[:3]) if errores else salida
        base = re.sub(r"\S*Problema\.lean:", "línea ", base)
        base = recortar(base, 1400)
        if re.search(r"unknown (identifier|constant|namespace)|Function expected", salida):
            base += "\n" + (PISTA_MATHLIB if self.mathlib else PISTA_SIN_MATHLIB)
        elif re.search(r"unexpected token|expected term|expected '", salida):
            base += "\n" + PISTA_SINTAXIS
        return base

    def titulo_error(self, resumen: str) -> str:
        ls = [l.strip() for l in (resumen or "").splitlines() if l.strip() and not l.strip().startswith("PISTA")]
        return " ".join(ls[:3])

    # --- prompts
    def prompt_formalizar(self, problema: str, error: str) -> str:
        libs = ("Puedes usar Mathlib (Real, Finset, Even, Nat.Prime...)." if self.mathlib else
                "Solo hay Lean 4 SIN Mathlib: usa Nat e Int (no Real) y operaciones básicas. «m es par» se escribe `m % 2 = 0` "
                "(NO existen Even, Odd, Nat.Prime, Finset ni la notación ℕ: usa Nat).")
        return f"""Traduce el siguiente problema matemático a un ENUNCIADO de Lean 4 (sin demostrarlo).

PROBLEMA:
{problema}

REGLAS:
- Conserva exactamente el significado matemático. No lo conviertas en True ni añadas hipótesis.
- {libs}
- Incluye solo definiciones realmente necesarias. No uses axiomas ni sorry.
- El teorema debe terminar en `:= by`.

ERROR DEL INTENTO ANTERIOR (si lo hay):
{error or '(ninguno)'}

Devuelve SOLO este JSON:
{{"definiciones": "", "teorema": "theorem nombre (...) : proposicion := by"}}"""

    def prompt_fidelidad(self, problema: str, defs: str, firma: str) -> str:
        return f"""Comprueba si este enunciado de Lean 4 expresa EXACTAMENTE el problema (ni más fuerte, ni más débil, ni trivial).

PROBLEMA:
{problema}

DEFINICIONES:
{defs or '(ninguna)'}

ENUNCIADO LEAN:
{firma}

Devuelve SOLO JSON: {{"equivalente": true|false, "motivo": "una frase"}}"""

    def prompt_prueba(self, firma: str, bloques: List[str], estado: Dict[str, Any], nodo: Dict[str, Any], tipo: str) -> str:
        ctx = "\n\n".join(bloques) or "(ninguno)"
        prev = ""
        if nodo.get("ultimo_codigo"):
            prev = f"""
TU INTENTO ANTERIOR (falló):
```lean
{nodo['ultimo_codigo']}
```
ERROR DE LEAN:
{nodo.get('ultimo_error', '')}
Corrige ese intento: analiza el error y cambia lo necesario (no repitas lo mismo)."""
        otros = ""
        if len(nodo.get("errores", [])) > 1:
            otros = "\nERRORES DE INTENTOS ANTERIORES (para no repetirlos):\n" + "\n---\n".join(nodo["errores"][-4:-1])
        return f"""Demuestra este teorema en Lean 4.

TEOREMA:
```lean
{firma}
```
DEFINICIONES:
{estado.get('definiciones') or '(ninguna)'}

LEMAS YA DEMOSTRADOS (puedes usarlos por su nombre):
{ctx}
{prev}{otros}

{GUIA_LEAN}

Devuelve el teorema completo en UN solo bloque ```lean ... ``` (con `:= by` y la demostración)."""

    def prompt_plan(self, firma: str, bloques: List[str], estado: Dict[str, Any], nodo: Dict[str, Any], max_lemas: int,
                    fallidos: List[str]) -> str:
        probados = "\n\n".join(bloques) or "(ninguno)"
        f = "\n".join(f"- {x}" for x in fallidos[-6:]) or "(ninguno)"
        return f"""Diseña una estrategia para demostrar este teorema en Lean 4 descomponiéndolo en lemas.

TEOREMA PRINCIPAL:
{firma}

DEFINICIONES:
{estado.get('definiciones') or '(ninguna)'}

LEMAS YA DEMOSTRADOS (no los repitas; úsalos):
{probados}

LEMAS QUE NO SE PUDIERON DEMOSTRAR (propón otros, más sencillos o distintos):
{f}

ÚLTIMO ERROR DEL TEOREMA PRINCIPAL:
{nodo.get('ultimo_error') or '(ninguno)'}

Propón como máximo {max_lemas} lemas NUEVOS, en el orden en que deben demostrarse.
REGLAS: cada lema aporta una propiedad intermedia necesaria; no repitas el teorema principal; sin lemas tautológicos;
sin hipótesis injustificadas; sin sorry/admit/axiom; cada uno termina en `:= by`.

Devuelve SOLO JSON: {{"lemas": ["theorem nombre (...) : proposicion := by"]}}"""


# ------------------------------------------------------------------ SymPy (física y cálculo simbólico)
PRELUDIO_PY = '''import sympy as sp
from sympy import *

def verificar(i, cond):
    if not bool(cond):
        raise AssertionError("CRITERIO_FALLA %s" % i)
    print("CRITERIO_OK %s" % i)
'''
PY_PROHIBIDO = re.compile(
    r"\b(verificar|subprocess|socket|urllib|requests|http|ftplib|ctypes|shutil|pathlib|importlib|__import__|exec|eval|"
    r"compile|globals|locals|breakpoint|input|open|os\.system|os\.popen|os\.remove|sys\.exit|exit|quit)\b")


class SympyVerificador(Verificador):
    nombre = "sympy"
    extension = "py"
    admite_plan = False
    sistema = ("Eres un físico teórico y matemático riguroso que programa en Python con SymPy. DERIVAS los resultados "
               "paso a paso a partir de principios; nunca los das por supuestos ni los escribes directamente.")

    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg

    def entorno(self) -> str:
        r = subprocess.run([sys.executable, "-c", "import sympy; print(sympy.__version__)"], capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError("Falta SymPy: pip install sympy")
        return "sympy " + r.stdout.strip()

    def comprobar(self, codigo: str, nombre_final: str = "") -> Tuple[bool, str]:
        limpio = {k: v for k, v in os.environ.items() if not re.search(r"(KEY|TOKEN|SECRET|PASS|CRED|AUTH)", k, re.I)}
        kw: Dict[str, Any] = {}
        if os.name == "posix":
            def limites() -> None:   # noqa: ANN202
                import resource
                resource.setrlimit(resource.RLIMIT_CPU, (self.cfg.timeout_py + 5, self.cfg.timeout_py + 10))
                resource.setrlimit(resource.RLIMIT_AS, (6 * 2**30, 6 * 2**30))
            kw["preexec_fn"] = limites
        with tempfile.TemporaryDirectory(prefix="demo_py_") as td:
            f = Path(td) / "derivacion.py"
            f.write_text(codigo, encoding="utf-8")
            try:
                r = subprocess.run([sys.executable, "-B", str(f)], capture_output=True, text=True, timeout=self.cfg.timeout_py,
                                   cwd=td, env=limpio, **kw)
            except subprocess.TimeoutExpired:
                return False, f"El cálculo superó el límite de {self.cfg.timeout_py}s."
        salida = ((r.stdout or "") + "\n" + (r.stderr or "")).strip()
        if r.returncode < 0:
            return False, (f"El proceso fue terminado por el sistema (señal {-r.returncode}): probablemente superó el límite de "
                           f"CPU o de memoria. Usa un cálculo más ligero (menos simplificaciones pesadas, series cortas).")
        if r.returncode != 0:
            return False, salida or "El script falló sin mensaje."
        if nombre_final and "TODO_VERIFICADO" not in salida:
            return False, "El script terminó pero no llegó a comprobar todos los criterios."
        return True, salida

    def prohibido(self, texto: str) -> Optional[str]:
        m = PY_PROHIBIDO.search(quitar_comentarios_py(texto))
        return m.group(0) if m else None

    def _codigo(self, texto: str) -> str:
        return elegir_bloque(texto, ("python", "py"), lambda b: "import" in b or "=" in b)

    def extraer_cuerpo(self, texto: str) -> str:
        c = self._codigo(texto)
        c = "\n".join(l for l in c.split("\n") if not re.match(r"\s*(import sympy|from sympy import \*)\s*$", l))
        return textwrap.dedent(c).strip("\n")

    def bloque(self, firma: str, cuerpo: str) -> str:
        return cuerpo

    def codigo(self, estado: Dict[str, Any], bloques: List[str], firma: str = "", cuerpo: str = "") -> str:
        crit = "\n".join(f"verificar({i + 1}, {c})" for i, c in enumerate(estado.get("criterios", [])))
        return ensamblar(PRELUDIO_PY, estado.get("preambulo", ""), *bloques, cuerpo, "# --- criterios fijados antes de resolver ---\n" + crit,
                         'print("TODO_VERIFICADO")')

    def auto(self, estado: Dict[str, Any], firma: str, bloques: List[str]) -> Optional[str]:
        return None

    def resumir_error(self, salida: str) -> str:
        lineas = [l for l in (salida or "").splitlines() if l.strip()]
        return recortar("\n".join(lineas[-12:]) or "error desconocido", 1400)

    def titulo_error(self, resumen: str) -> str:
        lineas = [l for l in (resumen or "").splitlines() if l.strip()]
        return lineas[-1] if lineas else ""     # la excepción es la última línea del traceback

    def enunciado_trivial(self, firma: str) -> Optional[str]:
        return None

    def prompt_formalizar(self, problema: str, error: str) -> str:
        return f"""Convierte este problema de física/matemáticas en una ESPECIFICACIÓN verificable con SymPy.

PROBLEMA:
{problema}

Debes definir:
1. "preambulo": código Python (SymPy) que declara símbolos, constantes y supuestos (p. ej. `G, M, c, r = symbols('G M c r', positive=True)`).
2. "variables": nombres de las variables que la solución debe DEFINIR (los resultados).
3. "criterios": expresiones Python que valen True solo si el resultado es correcto (p. ej. `simplify(v_esc.subs(r, 2*G*M/c**2) - c) == 0`).
   Deben usar las variables de resultado, ser estrictas (no triviales) y comprobar el resultado pedido, no uno parecido.
4. "descripcion": el objetivo en una frase.

ERROR DEL INTENTO ANTERIOR (si lo hay):
{error or '(ninguno)'}

Devuelve SOLO JSON: {{"descripcion": "", "preambulo": "", "variables": ["..."], "criterios": ["..."]}}"""

    def prompt_fidelidad(self, problema: str, defs: str, firma: str) -> str:
        return f"""Comprueba si estos criterios verifican EXACTAMENTE lo que pide el problema (ni más débiles, ni triviales, ni de otro resultado).

PROBLEMA:
{problema}

{defs}

Devuelve SOLO JSON: {{"equivalente": true|false, "motivo": "una frase"}}"""

    def prompt_prueba(self, firma: str, bloques: List[str], estado: Dict[str, Any], nodo: Dict[str, Any], tipo: str) -> str:
        prev = ""
        if nodo.get("ultimo_codigo"):
            prev = f"""
TU INTENTO ANTERIOR (falló):
```python
{nodo['ultimo_codigo']}
```
SALIDA / ERROR:
{nodo.get('ultimo_error', '')}
Corrige el fallo concreto; no repitas lo mismo."""
        crit = "\n".join(f"  {i + 1}. {c}" for i, c in enumerate(estado.get("criterios", [])))
        return f"""Resuelve este problema DERIVANDO el resultado con SymPy.

PROBLEMA:
{estado['problema']['enunciado']}

OBJETIVO: {firma}

CONTEXTO ya ejecutado antes de tu código (no lo repitas):
```python
{estado.get('preambulo', '')}
```
Tu código debe DEFINIR estas variables: {', '.join(estado.get('variables', []))}
Después de tu código se comprobarán automáticamente estos criterios (no los escribas tú):
{crit}

REGLAS: deriva el resultado (planteando ecuaciones y resolviendo con solve/dsolve/integrate/series/simplify...), no lo escribas ya hecho;
sin entrada/salida de ficheros ni red; sin `verificar`; el código debe ejecutarse sin errores.
{prev}

Devuelve SOLO un bloque ```python ... ```."""

    def prompt_plan(self, *a: Any, **k: Any) -> str:
        return ""


# =============================================================================
# 5. MOTOR: árbol de demostración persistente y reanudable
# =============================================================================
TEMPERATURAS = [0.1, 0.25, 0.4, 0.55, 0.7, 0.85]


class Motor:
    def __init__(self, cfg: Config, llm: Any, ver: Verificador, directorio: Path, log: Optional[Callable[[str], None]] = None) -> None:
        self.cfg, self.llm, self.ver = cfg, llm, ver
        self.dir = Path(directorio)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.log = log or (lambda s: print(s, flush=True))
        self.estado: Dict[str, Any] = {}
        self.t_inicio = time.time()
        self.t_base = 0.0
        self.llamadas_sesion = 0
        self.parar = False

    # ------------------------------------------------------------ persistencia
    @property
    def ruta_estado(self) -> Path:
        return self.dir / "estado.json"

    def nuevo_estado(self, problema: Dict[str, Any]) -> Dict[str, Any]:
        return {"version": ESTADO_VERSION, "problema": problema, "huella": self.huella_problema(problema), "creado": ahora(),
                "estado": "formalizando", "formalizado": False, "definiciones": "", "preambulo": "", "variables": [], "criterios": [],
                "raiz": None, "contador": 0, "llamadas": 0, "tiempo": 0.0, "intentos_formalizar": 0, "ultimo_error": "",
                "resultado": {"codigo": "", "archivo": ""}}

    def huella_problema(self, problema: Dict[str, Any]) -> str:
        return huella(problema["enunciado"] + "|" + problema.get("backend", "lean") + "|" + str(bool(self.cfg.mathlib or self.cfg.lake_dir)))

    def cargar(self, problema: Dict[str, Any], reiniciar: bool = False) -> None:
        if reiniciar and self.ruta_estado.exists():
            self.ruta_estado.rename(self.dir / f"estado.{int(time.time())}.bak.json")
        if self.ruta_estado.exists():
            try:
                datos = json.loads(self.ruta_estado.read_text(encoding="utf-8"))
                if datos.get("version") == ESTADO_VERSION and datos.get("huella") == self.huella_problema(problema):
                    self.estado = datos
                    self.t_base = float(datos.get("tiempo", 0.0))
                    self.log(f"Reanudando: {datos['estado']}, {datos['llamadas']} llamadas y {self.t_base / 60:.1f} min de trabajo previos.")
                    if datos["estado"] == "fallido":
                        self.reabrir_fallidos()
                    return
                self.log("El estado guardado es de otro problema o versión: se conserva como copia y se empieza de nuevo.")
                self.ruta_estado.rename(self.dir / f"estado.{int(time.time())}.bak.json")
            except (OSError, json.JSONDecodeError) as e:
                self.log(f"Estado ilegible ({e}); se empieza de nuevo.")
                self.ruta_estado.rename(self.dir / f"estado.{int(time.time())}.corrupto.json")
        self.estado = self.nuevo_estado(problema)
        self.guardar()

    def reabrir_fallidos(self) -> None:
        """Un estado "fallido" significa que se agotaron los límites. Al volver a ejecutar se reintenta: los contadores
        vuelven a cero, pero se conservan los lemas demostrados, el historial de errores y las demostraciones ya
        descartadas (huellas), para no repetir lo que ya falló."""
        e = self.estado
        e["estado"] = "resolviendo" if e.get("formalizado") else "formalizando"
        e["intentos_formalizar"] = 0
        e.pop("error_fatal", None)

        def abrir(n: Dict[str, Any]) -> None:
            if n["estado"] == "fallido":
                n.update(estado="pendiente", directos=0, finales=0, replanes=0, necesita_plan=True)
            for h in n["hijos"]:
                abrir(h)
        if e.get("raiz"):
            abrir(e["raiz"])
        self.log("La ejecución anterior terminó sin resolverse: se reintenta con los contadores a cero "
                 "(se conservan los lemas demostrados y las demostraciones ya descartadas).")
        self.guardar()

    def guardar(self) -> None:
        self.estado["tiempo"] = self.t_base + (time.time() - self.t_inicio)
        escribir_atomico(self.ruta_estado, json.dumps(self.estado, ensure_ascii=False, indent=1))

    def evento(self, nombre_evento: str, /, **datos: Any) -> None:
        rec = {"ts": ahora(), "t": round(self.estado.get("tiempo", 0.0), 1), "evento": nombre_evento, **datos}
        with open(self.dir / "registro.jsonl", "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    def say(self, n: Optional[Dict[str, Any]], msg: str) -> None:
        t = self.t_base + (time.time() - self.t_inicio)
        h, m, s = int(t // 3600), int(t % 3600 // 60), int(t % 60)
        self.log(f"[{h:02d}:{m:02d}:{s:02d} | llamadas {self.estado.get('llamadas', 0)}{' | ' + n['id'] if n else ''}] {msg}")

    # ------------------------------------------------------------ presupuesto y modelo
    def presupuesto(self) -> None:
        if self.parar:
            raise Pausa("parada solicitada")
        if self.cfg.horas and (time.time() - self.t_inicio) >= self.cfg.horas * 3600:
            raise Pausa(f"límite de {self.cfg.horas} h de esta sesión")
        if self.cfg.max_intentos and self.llamadas_sesion >= self.cfg.max_intentos:
            raise Pausa(f"límite de {self.cfg.max_intentos} llamadas al modelo en esta sesión")

    def _llm(self, prompt: str, temperatura: float, seed: int, json_mode: bool = False, num_predict: Optional[int] = None) -> str:
        self.presupuesto()
        self.estado["llamadas"] += 1
        self.llamadas_sesion += 1
        try:
            return self.llm.chat(self.ver.sistema, prompt, temperatura=temperatura, num_predict=num_predict or self.cfg.num_predict,
                                 seed=seed, json_mode=json_mode)
        finally:
            self.guardar()

    # ------------------------------------------------------------ FASE 0: formalización
    def formalizar(self) -> None:
        if self.estado["formalizado"]:
            return
        self.say(None, "FASE 0: formalización del enunciado")
        problema = self.estado["problema"]["enunciado"]
        error = self.estado.get("ultimo_error", "")
        while self.estado["intentos_formalizar"] < self.cfg.formalizar:
            self.estado["intentos_formalizar"] += 1
            i = self.estado["intentos_formalizar"]
            try:
                resp = self._llm(self.ver.prompt_formalizar(problema, error), 0.1 + 0.03 * i, self.cfg.semilla * 7919 + i, json_mode=True)
                datos = extraer_json(resp)
                firma, descripcion = self._validar_formalizacion(datos)
                if self.cfg.fidelidad:
                    self._fidelidad(problema, firma)
                self.estado["formalizado"] = True
                self.estado["estado"] = "resolviendo"
                self.estado["raiz"] = self.nodo(firma, 0)
                self.estado["ultimo_error"] = ""
                self.guardar()
                self.evento("formalizado", firma=firma)
                self.say(None, "Formalización aceptada:\n" + firma)
                return
            except (ValueError, KeyError, TypeError) as e:
                error = str(e)
                self.estado["ultimo_error"] = recortar(error, 800)
                self.guardar()
                self.evento("formalizacion_fallo", error=recortar(error, 400))
                self.say(None, f"formalización {i}/{self.cfg.formalizar} rechazada: {recortar(error.splitlines()[0] if error else '', 140)}")
        raise RuntimeError("No se pudo formalizar el problema: " + recortar(error, 300))

    def _validar_formalizacion(self, datos: Dict[str, Any]) -> Tuple[str, str]:
        ver = self.ver
        if isinstance(ver, LeanVerificador):
            defs = datos.get("definiciones", "") or ""
            defs = "\n".join(l for l in str(defs).splitlines() if not l.strip().startswith("import "))
            firma = ver.extraer_firma(str(datos.get("teorema", "")))
            if not firma:
                raise ValueError("No se obtuvo una firma `theorem ... := by` válida.")
            malo = ver.prohibido(defs + "\n" + firma)
            if malo:
                raise ValueError(f"Construcción prohibida en la formalización: {malo}")
            triv = ver.enunciado_trivial(firma)
            if triv:
                raise ValueError(triv)
            self.estado["definiciones"] = defs.strip()
            ok, salida = ver.comprobar(ver.codigo(self.estado, [], firma, "  sorry"), permitir_sorry=True)
            if not ok:
                raise ValueError(ver.resumir_error(salida))
            return firma, firma
        # SymPy
        pre = str(datos.get("preambulo", "") or "")
        crit = [str(c) for c in (datos.get("criterios") or []) if str(c).strip()]
        vars_ = [str(v) for v in (datos.get("variables") or [])]
        desc = str(datos.get("descripcion", "") or "").strip()
        if not crit or not vars_ or not desc:
            raise ValueError("Faltan 'descripcion', 'variables' o 'criterios'.")
        malo = ver.prohibido(pre) or next((m for m in (ver.prohibido(c) for c in crit) if m), None)
        if malo:
            raise ValueError(f"Construcción prohibida en la especificación: {malo}")
        try:
            compile(pre, "<preambulo>", "exec")
            for c in crit:
                compile(c, "<criterio>", "eval")
        except SyntaxError as e:
            raise ValueError(f"Error de sintaxis en la especificación: {e}")
        ok, salida = ver.comprobar(ensamblar(PRELUDIO_PY, pre, 'print("TODO_VERIFICADO")'), nombre_final="x")
        if not ok:
            raise ValueError("El preámbulo no se ejecuta: " + ver.resumir_error(salida))
        self.estado.update({"preambulo": pre.strip(), "variables": vars_, "criterios": crit})
        return desc, desc

    def _fidelidad(self, problema: str, firma: str) -> None:
        """Segunda opinión del modelo: ¿el enunciado formal dice lo que pide el problema? (señal débil, pero útil)."""
        ver = self.ver
        if isinstance(ver, LeanVerificador):
            prompt = ver.prompt_fidelidad(problema, self.estado.get("definiciones", ""), firma)
        else:
            prompt = ver.prompt_fidelidad(problema, "PREÁMBULO:\n" + self.estado.get("preambulo", "") + "\nCRITERIOS:\n" +
                                          "\n".join(self.estado.get("criterios", [])), firma)
        try:
            obj = extraer_json(self._llm(prompt, 0.0, self.cfg.semilla, json_mode=True, num_predict=300))
        except (ValueError, TypeError):
            return
        if obj.get("equivalente") is False:
            raise ValueError("Fidelidad: el enunciado formal no coincide con el problema (" + str(obj.get("motivo", ""))[:200] + ")")

    # ------------------------------------------------------------ árbol
    def nodo(self, firma: str, prof: int) -> Dict[str, Any]:
        self.estado["contador"] += 1
        return {"id": f"n{self.estado['contador']}", "firma": firma, "prof": prof, "estado": "pendiente", "cuerpo": "", "auto": False,
                "directos": 0, "finales": 0, "replanes": 0, "necesita_plan": True, "hijos": [], "fallidos": [], "errores": [],
                "ultimo_codigo": "", "ultimo_error": "", "huellas": []}

    def bloque_nodo(self, n: Dict[str, Any]) -> str:
        hijos = [self.bloque_nodo(h) for h in n["hijos"] if h["estado"] == "probado"]
        return ensamblar(*hijos, self.ver.bloque(n["firma"], n["cuerpo"]))

    def _fallo(self, n: Dict[str, Any], cuerpo: str, salida: str, tipo: str) -> bool:
        resumen = self.ver.resumir_error(salida)
        n["ultimo_codigo"], n["ultimo_error"] = cuerpo, resumen
        n["errores"] = (n["errores"] + [resumen])[-6:]
        self.guardar()
        self.evento("intento_fallido", nodo=n["id"], tipo=tipo, error=recortar(resumen, 400))
        self.say(n, f"{tipo} {n['directos'] if tipo == 'directo' else n['finales']}: FALLA — {recortar(self.ver.titulo_error(resumen), 130)}")
        return False

    def _aceptar(self, n: Dict[str, Any], cuerpo: str, tipo: str) -> bool:
        n["cuerpo"], n["estado"] = cuerpo, "probado"
        n["ultimo_error"] = ""
        self.guardar()
        self.evento("probado", nodo=n["id"], tipo=tipo, firma=recortar(n["firma"], 200))
        self.say(n, f"{tipo}: VERIFICADO — {recortar(n['firma'].splitlines()[0], 110)}")
        return True

    def _intento(self, n: Dict[str, Any], ctx: List[str], tipo: str) -> bool:
        self.presupuesto()
        clave = "directos" if tipo == "directo" else "finales"
        n[clave] += 1
        idx = n["directos"] + n["finales"] + 6 * n["replanes"]
        temp = TEMPERATURAS[min(idx - 1, len(TEMPERATURAS) - 1)]
        seed = self.cfg.semilla * 1000003 + int(n["id"][1:]) * 1009 + n["directos"] * 31 + n["finales"] * 17 + n["replanes"] * 5
        resp = self._llm(self.ver.prompt_prueba(n["firma"], ctx, self.estado, n, tipo), temp, seed)
        cuerpo = self.ver.extraer_cuerpo(resp)
        if not cuerpo:
            return self._fallo(n, "", "No se pudo extraer una demostración de la respuesta.", tipo)
        malo = self.ver.prohibido(cuerpo)
        if malo:
            return self._fallo(n, cuerpo, f"Construcción prohibida: {malo}", tipo)
        h = huella(cuerpo)
        if h in n["huellas"]:
            return self._fallo(n, cuerpo, "Demostración repetida: ya se probó y falló.", tipo)
        n["huellas"] = (n["huellas"] + [h])[-200:]
        ok, salida = self.ver.comprobar(self.ver.codigo(self.estado, ctx, n["firma"], cuerpo), nombre_final="x" if self.ver.nombre == "sympy" else "")
        return self._aceptar(n, cuerpo, tipo) if ok else self._fallo(n, cuerpo, salida, tipo)

    def _planificar(self, n: Dict[str, Any], ctx: List[str]) -> List[Dict[str, Any]]:
        ver = self.ver
        assert isinstance(ver, LeanVerificador)
        existentes = {ver.clave(h["firma"]) for h in n["hijos"]} | {ver.clave(f) for f in n["fallidos"]} | {ver.clave(n["firma"])}
        probados = ctx + [self.bloque_nodo(h) for h in n["hijos"] if h["estado"] == "probado"]
        error = ""
        for k in range(1, self.cfg.planes + 1):
            seed = self.cfg.semilla * 2000003 + int(n["id"][1:]) * 1013 + n["replanes"] * 101 + k
            try:
                obj = extraer_json(self._llm(ver.prompt_plan(n["firma"], probados, self.estado, n, self.cfg.max_lemas, n["fallidos"]),
                                             0.25 + 0.05 * (k + n["replanes"]), seed, json_mode=True, num_predict=2000))
                lista = obj.get("lemas")
                if not isinstance(lista, list):
                    raise ValueError("'lemas' debe ser una lista")
                firmas, claves = [], set(existentes)
                for raw in lista[: self.cfg.max_lemas]:
                    f = ver.extraer_firma(str(raw))
                    if not f or ver.prohibido(f) or ver.enunciado_trivial(f) or ver.clave(f) in claves:
                        continue
                    claves.add(ver.clave(f))
                    self.estado["contador"] += 1
                    slug = re.sub(r"[^A-Za-z0-9_]", "_", ver.nombre(f))[:30] or "lema"
                    firmas.append(ver.renombrar(f, f"l{self.estado['contador']}_{slug}"))
                if not firmas:
                    raise ValueError("no se obtuvo ningún lema útil y nuevo")
                codigo = ensamblar(ver.codigo(self.estado, probados), *[f"{f}\n  sorry" for f in firmas], f"{n['firma']}\n  sorry")
                ok, salida = ver.comprobar(codigo, permitir_sorry=True)
                if not ok:
                    raise ValueError(ver.resumir_error(salida))
                self.evento("plan", nodo=n["id"], lemas=firmas)
                self.say(n, f"plan con {len(firmas)} lemas: " + "; ".join(recortar(f.splitlines()[0], 60) for f in firmas))
                return [self.nodo(f, n["prof"] + 1) for f in firmas]
            except (ValueError, KeyError, TypeError) as e:
                error = str(e)
                self.evento("plan_fallo", nodo=n["id"], error=recortar(error, 300))
                self.say(n, f"plan {k}/{self.cfg.planes} rechazado: {recortar(error.splitlines()[0] if error else '', 120)}")
        return []

    def resolver_nodo(self, n: Dict[str, Any], ctx: List[str]) -> bool:
        if n["estado"] == "probado":
            return True
        ver = self.ver
        if not n["auto"]:
            self.presupuesto()
            cuerpo = ver.auto(self.estado, n["firma"], ctx)
            n["auto"] = True
            self.guardar()
            if cuerpo:
                return self._aceptar(n, cuerpo, "auto")
        while n["directos"] < self.cfg.directos:
            if self._intento(n, ctx, "directo"):
                return True
        if not ver.admite_plan or n["prof"] >= self.cfg.profundidad:
            return self._fallar(n)
        while n["replanes"] <= self.cfg.replanes:
            if n["necesita_plan"]:
                if n["replanes"] >= self.cfg.replanes:
                    break
                n["replanes"] += 1
                nuevos = self._planificar(n, ctx)
                n["hijos"].extend(nuevos)
                n["necesita_plan"] = not nuevos
                n["finales"] = 0
                self.guardar()
                if not nuevos:
                    continue
            hubo_fallo = False
            for i, h in enumerate(n["hijos"]):
                if h["estado"] == "probado":
                    continue
                ctx_h = ctx + [self.bloque_nodo(x) for x in n["hijos"][:i] if x["estado"] == "probado"]
                if not self.resolver_nodo(h, ctx_h):
                    n["fallidos"].append(h["firma"])
                    n["hijos"] = [x for x in n["hijos"] if x["estado"] == "probado"]   # se conservan los lemas ya demostrados
                    n["necesita_plan"] = True
                    hubo_fallo = True
                    self.guardar()
                    break
            if hubo_fallo:
                continue
            ctx_f = ctx + [self.bloque_nodo(x) for x in n["hijos"]]
            while n["finales"] < self.cfg.finales:
                if self._intento(n, ctx_f, "final"):
                    return True
            n["necesita_plan"] = True    # los lemas no bastaron: pedir más
            self.guardar()
        return self._fallar(n)

    def _fallar(self, n: Dict[str, Any]) -> bool:
        n["estado"] = "fallido"
        self.guardar()
        self.evento("fallido", nodo=n["id"], firma=recortar(n["firma"], 200))
        self.say(n, "no se pudo demostrar: " + recortar(n["firma"].splitlines()[0], 100))
        return False

    # ------------------------------------------------------------ cierre
    def finalizar(self) -> bool:
        ver, raiz = self.ver, self.estado["raiz"]
        codigo = ver.codigo(self.estado, [self.bloque_nodo(raiz)])
        nombre = ver.nombre(raiz["firma"]) if isinstance(ver, LeanVerificador) else "x"
        ok, salida = ver.comprobar(codigo, nombre_final=nombre)
        if not ok:
            self.estado["ultimo_error"] = recortar(salida, 800)
            self.guardar()
            self.say(None, "La verificación final FALLÓ: " + recortar(salida, 300))
            raiz["estado"] = "pendiente"   # se volverá a intentar en la próxima ejecución
            raiz["directos"] = raiz["finales"] = 0
            self.guardar()
            return False
        archivo = self.dir / f"resultado.{ver.extension}"
        escribir_atomico(archivo, codigo + "\n")
        self.estado["resultado"] = {"codigo": codigo, "archivo": str(archivo)}
        self.estado["estado"] = "verificado"
        self.guardar()
        self.evento("verificado", archivo=str(archivo))
        return True

    def ejecutar(self, problema: Dict[str, Any], reiniciar: bool = False) -> int:
        self.say(None, f"Verificador: {self.ver.entorno()} | modelo {self.cfg.modelo}")
        self.cargar(problema, reiniciar)
        self.t_inicio = time.time()
        previo = signal.getsignal(signal.SIGTERM) if hasattr(signal, "SIGTERM") else None

        def al_terminar(signum: int, frame: Any) -> None:
            raise KeyboardInterrupt()
        try:
            if hasattr(signal, "SIGTERM"):
                signal.signal(signal.SIGTERM, al_terminar)
            if self.estado["estado"] == "verificado":
                self.say(None, "Este problema ya estaba verificado: " + self.estado["resultado"]["archivo"])
                return 0
            self.formalizar()
            if not self.resolver_nodo(self.estado["raiz"], []):
                self.estado["estado"] = "fallido"
                self.guardar()
                self.say(None, "No se encontró una demostración con los límites actuales. Sube --directos/--replanes/--profundidad y vuelve a ejecutar (con --reiniciar si quieres empezar de cero).")
                return 1
            if not self.finalizar():
                return 1
            self.say(None, "PROBLEMA RESUELTO Y VERIFICADO: " + self.estado["resultado"]["archivo"])
            return 0
        except Pausa as p:
            self.estado["estado"] = "pausado"
            self.guardar()
            self.say(None, f"PAUSADO ({p}). El progreso está guardado: ejecuta el mismo comando para continuar.")
            return 3
        except KeyboardInterrupt:
            self.estado["estado"] = "pausado"
            self.guardar()
            self.say(None, "INTERRUMPIDO. El progreso está guardado: ejecuta el mismo comando para continuar.")
            return 130
        except ErrorModelo as e:
            self.estado["estado"] = "pausado"
            self.guardar()
            self.say(None, f"ERROR DEL MODELO: {e}\nEl progreso está guardado: arranca Ollama y vuelve a ejecutar el mismo comando.")
            return 4
        except RuntimeError as e:
            self.estado["estado"] = "fallido"
            self.estado["error_fatal"] = str(e)      # ultimo_error conserva el error bruto del verificador
            self.guardar()
            self.say(None, f"ERROR: {e}")
            return 1
        finally:
            if previo is not None and hasattr(signal, "SIGTERM"):
                signal.signal(signal.SIGTERM, previo)


# =============================================================================
# 6. INFORME Y LÍNEA DE COMANDOS
# =============================================================================
def arbol(n: Dict[str, Any], nivel: int = 0) -> List[str]:
    marca = {"probado": "✔", "fallido": "✘", "pendiente": "…"}.get(n["estado"], "?")
    linea = f"{'    ' * nivel}{marca} {n['id']} [{n['estado']}] directos={n['directos']} finales={n['finales']} replanes={n['replanes']}  {recortar(n['firma'].splitlines()[0], 90)}"
    return [linea] + [l for h in n["hijos"] for l in arbol(h, nivel + 1)]


def informe(directorio: Path) -> str:
    ruta = Path(directorio) / "estado.json"
    if not ruta.exists():
        return f"No hay estado en {directorio}"
    e = json.loads(ruta.read_text(encoding="utf-8"))
    t = float(e.get("tiempo", 0))
    out = [f"Problema: {recortar(e['problema']['enunciado'].strip(), 150)}", f"Estado: {e['estado']} | backend {e['problema'].get('backend')} | "
           f"llamadas {e['llamadas']} | tiempo {t / 3600:.2f} h"]
    if e.get("raiz"):
        out += ["", *arbol(e["raiz"])]
    if e.get("error_fatal"):
        out += ["", "Error: " + recortar(e["error_fatal"], 300)]
    elif e.get("ultimo_error"):
        out += ["", "Último error: " + recortar(e["ultimo_error"], 300)]
    if e["estado"] == "verificado":
        out += ["", "Resultado: " + e["resultado"]["archivo"]]
    return "\n".join(out)


def cargar_problema(a: argparse.Namespace) -> Dict[str, Any]:
    if a.problema:
        p = json.loads(Path(a.problema).read_text(encoding="utf-8"))
        if "enunciado" not in p:
            raise SystemExit("El archivo del problema necesita el campo 'enunciado'.")
        p.setdefault("id", Path(a.problema).stem)
    elif a.enunciado:
        p = {"id": "", "enunciado": a.enunciado}
    else:
        raise SystemExit("Indica --problema ARCHIVO.json o --enunciado \"texto\".")
    p["enunciado"] = textwrap.dedent(p["enunciado"]).strip()
    p["backend"] = a.backend or p.get("backend", "lean")
    p.setdefault("dominio", "matematicas")
    if not p["id"]:
        p["id"] = re.sub(r"\W+", "_", p["enunciado"][:30]).strip("_").lower() or "problema"
    return p


def construir_config(a: argparse.Namespace, problema: Dict[str, Any]) -> Config:
    cfg = Config(modelo=a.modelo, ollama_url=a.ollama_url or os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434"), backend=problema["backend"],
                 lean=a.lean or "", lake_dir=a.lake_dir or "", mathlib=a.mathlib, horas=a.horas, max_intentos=a.max_intentos,
                 directos=a.directos, finales=a.finales, replanes=a.replanes, profundidad=a.profundidad, num_ctx=a.num_ctx,
                 num_predict=a.num_predict, fidelidad=not a.sin_fidelidad, semilla=a.semilla, timeout_lean=a.timeout_lean)
    return cfg


def crear_verificador(cfg: Config) -> Verificador:
    if cfg.backend == "lean":
        return LeanVerificador(cfg)
    if cfg.backend == "sympy":
        return SympyVerificador(cfg)
    raise SystemExit(f"backend desconocido: {cfg.backend} (usa lean o sympy)")


def cmd_resolver(a: argparse.Namespace) -> int:
    problema = cargar_problema(a)
    cfg = construir_config(a, problema)
    ver = crear_verificador(cfg)
    base = Path(a.dir) if a.dir else Path("demostrador_runs") / f"{problema['id']}_{huella(problema['enunciado'] + problema['backend'])[:8]}"
    llm = LLM(cfg)
    motor = Motor(cfg, llm, ver, base)
    print(f"Directorio de trabajo (estado y resultado): {base}", flush=True)
    try:
        return motor.ejecutar(problema, a.reiniciar)
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 2


def construir_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="demostrador.py", description="Demostrador de problemas matemáticos y físicos con LLM local y verificador externo.")
    sub = p.add_subparsers(dest="cmd")
    r = sub.add_parser("resolver", help="resolver (o reanudar) un problema")
    r.add_argument("--problema", help="archivo JSON con enunciado, backend y dominio")
    r.add_argument("--enunciado", help="enunciado en texto")
    r.add_argument("--backend", choices=["lean", "sympy"], default=None)
    r.add_argument("--dir", help="carpeta de estado/resultado (por defecto demostrador_runs/<id>_<hash>)")
    r.add_argument("--modelo", default=MODELO_DEFECTO)
    r.add_argument("--ollama-url", default=None)
    r.add_argument("--lean", default="", help="ruta al ejecutable lean")
    r.add_argument("--lake-dir", default="", help="proyecto lake con Mathlib (usa `lake env lean`)")
    r.add_argument("--mathlib", action="store_true", help="permitir tácticas y tipos de Mathlib (requiere --lake-dir)")
    r.add_argument("--horas", type=float, default=0.0, help="límite de tiempo de ESTA sesión (0 = sin límite)")
    r.add_argument("--max-intentos", type=int, default=0, help="límite de llamadas al modelo en esta sesión (0 = sin límite)")
    r.add_argument("--directos", type=int, default=6)
    r.add_argument("--finales", type=int, default=6)
    r.add_argument("--replanes", type=int, default=3)
    r.add_argument("--profundidad", type=int, default=2)
    r.add_argument("--num-ctx", type=int, default=8192)
    r.add_argument("--num-predict", type=int, default=1600)
    r.add_argument("--timeout-lean", type=int, default=90)
    r.add_argument("--semilla", type=int, default=1)
    r.add_argument("--sin-fidelidad", action="store_true", help="no comprobar con el modelo que el enunciado formal es fiel")
    r.add_argument("--reiniciar", action="store_true", help="descartar el estado guardado y empezar de cero")
    e = sub.add_parser("estado", help="mostrar el progreso de una ejecución")
    e.add_argument("--dir", required=True)
    sub.add_parser("selftest", help="ejecutar las pruebas (tests/test_demostrador.py)")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except Exception:  # noqa: BLE001
            pass
    a = construir_parser().parse_args(argv)
    if a.cmd == "resolver":
        return cmd_resolver(a)
    if a.cmd == "estado":
        print(informe(Path(a.dir)))
        return 0
    if a.cmd == "selftest":
        import unittest
        suite = unittest.defaultTestLoader.discover(str(Path(__file__).resolve().parent / "tests"), pattern="test_demostrador.py")
        return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1
    construir_parser().print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
