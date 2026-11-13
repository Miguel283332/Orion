"""El bucle de vida: explorar -> aprender de lo verificado -> medirse -> quedarse con lo que mejora -> crecer si se estanca.

Cada generación:
1. EXPLORAR: elige problemas de su currículo, da varios intentos a cada uno y el juez marca los correctos.
2. ENTRENAR: ajusta sus pesos (LoRA) con las respuestas verificadas.
3. MEDIR: repite la validación (siempre los mismos problemas, que nunca entran en el entrenamiento).
4. ACEPTAR O DESHACER: si acierta claramente más, se queda con los pesos nuevos; si no, vuelve a los anteriores.
5. CRECER: tras varias generaciones sin mejorar, amplía su capacidad (más rango, más módulos, capas nuevas) y, si la
   GPU lo permite, se traslada a un modelo base mayor llevándose todo lo aprendido.
Además, en cada generación INVENTA ejercicios de programación nuevos; los que funcionan pasan a su currículo.
"""
from __future__ import annotations

import random
import shutil
import signal
import time
from pathlib import Path

from . import curriculo as cur
from .config import Config, Estado, anadir_linea, leer_lineas
from .juez import juzgar
from .modelo import MODULOS_MLP, Modelo, normalizar_modelo, siguiente_modelo
from .problemas import Problema, conjunto_fijo, texto_de_referencia
from .programas import SISTEMA_INVENTAR, invento_valido, peticion_de_invento


class Orion:
    def __init__(self, datos: Path, modelo: str | None = None, decir=print):
        self.datos = Path(datos)
        self.datos.mkdir(parents=True, exist_ok=True)
        self.cfg = Config.cargar(self.datos)
        self.estado = Estado.cargar(self.datos)
        self.decir = decir
        self._parar = False
        if not self.estado.modelo_base:
            self.estado.modelo_base, aviso = normalizar_modelo(modelo or self.cfg.modelo)
            if aviso:
                self.decir(aviso)
            self.estado.modelo_original = self.estado.modelo_base
            self.estado.rango, self.estado.modulos, self.estado.lr = self.cfg.rango_lora, list(self.cfg.modulos), self.cfg.lr
        self.validacion = conjunto_fijo(self.cfg.n_validacion, self.cfg.semilla)
        self.examen = conjunto_fijo(self.cfg.n_examen, self.cfg.semilla + 1, excluir={p.huella for p in self.validacion})
        self.reservados = {p.huella for p in self.validacion + self.examen}
        self.inventados = [Problema(**d) for d in leer_lineas(self.datos / "inventados.jsonl")]
        self.modelo = self._cargar_modelo()

    # ------------------------------------------------------------ utilidades
    def _ruta(self, relativa: str) -> Path | None:
        return self.datos / relativa if relativa else None

    def _base(self) -> str:
        b = self.estado.modelo_base
        return str(self.datos / b) if b.startswith("bases/") else b

    def _cargar_modelo(self) -> Modelo:
        self.decir(f"cargo el modelo {self.estado.modelo_base}" + (f" con el adaptador {self.estado.adaptador}" if self.estado.adaptador else ""))
        return self._crear_modelo(self._base(), self._ruta(self.estado.adaptador), self.estado.rango, self.estado.modulos)

    def _crear_modelo(self, base: str, adaptador: Path | None, rango: int, modulos: list) -> Modelo:
        return Modelo(base, adaptador, rango, modulos)

    def parar(self, *_):
        self._parar = True

    def _hay_que_parar(self) -> bool:
        return self._parar

    def medir(self, problemas: list[Problema]) -> list[int]:
        """Acierto (0/1) en cada problema, con respuesta determinista."""
        r = self.modelo.generar([p.enunciado for p in problemas], max_tokens=self.cfg.max_tokens, lote=self.cfg.lote_generacion,
                                parar=self._hay_que_parar)
        return [int(juzgar(p, t[0])) for p, t in zip(problemas, r)]

    @staticmethod
    def _pct(v: list[int]) -> str:
        return f"{sum(v)}/{len(v)} ({100 * sum(v) / max(1, len(v)):.1f} %)"

    def _registrar(self, **datos) -> None:
        anadir_linea(self.datos / "generaciones.jsonl", {"generacion": self.estado.generacion, "fecha": time.strftime("%Y-%m-%d %H:%M:%S"), **datos})

    def _guardar_aceptado(self) -> None:
        """Guarda el adaptador actual como la versión aceptada y borra la anterior."""
        anterior = self.estado.adaptador
        self.estado.adaptador = f"adaptadores/g{self.estado.generacion}_{int(time.time())}"
        self.modelo.guardar_adaptador(self._ruta(self.estado.adaptador))
        self.estado.guardar(self.datos)
        if anterior and anterior != self.estado.adaptador:
            shutil.rmtree(self.datos / anterior, ignore_errors=True)

    # ------------------------------------------------------------ fases
    def inicio(self) -> bool:
        """Primera medida, con el modelo tal como viene. Es la referencia de todo lo demás."""
        if self.estado.validacion:
            return True
        self.decir("me mido por primera vez (validación y examen), antes de cambiar nada…")
        val = self.medir(self.validacion)
        exa = self.medir(self.examen)
        if self._parar:
            return False
        self.estado.validacion, self.estado.examen = val, exa
        self.estado.examen_inicial = sum(exa) / max(1, len(exa))
        self._guardar_aceptado()
        self._registrar(tipo="inicio", validacion=sum(val), examen=sum(exa), capas=self.modelo.num_capas(), rango=self.estado.rango)
        self.decir(f"punto de partida: validación {self._pct(val)}, examen {self._pct(exa)}")
        return True

    def inventar(self) -> None:
        """Se inventa ejercicios de programación. Solo entran en su currículo los que se ejecutan bien y no están repetidos."""
        n = self.cfg.inventos_por_generacion
        if n <= 0:
            return
        r = random.Random(f"inventar-{self.estado.generacion}-{time.time()}")
        textos = self.modelo.generar([peticion_de_invento(r) for _ in range(n)], n=1, temperatura=0.9, max_tokens=max(320, self.cfg.max_tokens),
                                     lote=self.cfg.lote_generacion, parar=self._hay_que_parar, sistema=SISTEMA_INVENTAR)
        conocidos = {p.huella for p in self.inventados} | {p.referencia for p in self.inventados}
        nuevos = 0
        for t in textos:
            p = invento_valido(t[0])
            if p is None or p.huella in conocidos or p.referencia in conocidos:
                continue
            conocidos |= {p.huella, p.referencia}
            self.inventados.append(p)
            anadir_linea(self.datos / "inventados.jsonl", p.a_dict())
            nuevos += 1
        if nuevos:
            self.decir(f"  ★ me he inventado {nuevos} {'ejercicio nuevo' if nuevos == 1 else 'ejercicios nuevos'} que funcionan"
                       f" (ya tengo {len(self.inventados)} propios)")

    def explorar(self) -> int:
        cfg = self.cfg
        r = random.Random(f"{cfg.semilla}-{self.estado.generacion}-{time.time()}")
        total = cfg.problemas_por_generacion * self.estado.multiplicador_exploracion
        propios = r.sample(self.inventados, min(len(self.inventados), int(total * cfg.parte_inventados)))
        problemas = cur.elegir(self.estado.curriculo, total - len(propios), r, self.reservados) + propios
        intentos = self.modelo.generar([p.enunciado for p in problemas], n=cfg.intentos_por_problema, temperatura=cfg.temperatura,
                                       max_tokens=cfg.max_tokens, lote=max(1, cfg.lote_generacion // 2), parar=self._hay_que_parar)
        resultados, propias, referencia = [], 0, 0
        for p, textos in zip(problemas, intentos):
            buenas = sorted({t for t in textos if juzgar(p, t)}, key=len)
            resultados.append((p, sum(juzgar(p, t) for t in textos) / max(1, len(textos))))
            if buenas:
                propias += 1
                anadir_linea(self.datos / "ejemplos.jsonl", {**p.a_dict(), "respuesta": buenas[0], "origen": "propia", "generacion": self.estado.generacion})
            elif cfg.usar_soluciones_de_referencia:
                referencia += 1
                anadir_linea(self.datos / "ejemplos.jsonl", {**p.a_dict(), "respuesta": texto_de_referencia(p), "origen": "referencia",
                                                             "generacion": self.estado.generacion})
        for aviso in cur.anotar(self.estado.curriculo, resultados):
            self.decir("  ★ " + aviso)
        self.decir(f"  exploré {len(resultados)} problemas: resolví {propias} por mi cuenta" +
                   (f" y estudié la solución de {referencia}" if referencia else ""))
        return len(resultados)

    def _ejemplos(self, cupo: int = 0) -> list[tuple[str, str]]:
        """Los de esta generación y, hasta completar el cupo, un repaso al azar de los anteriores (para no olvidar)."""
        ultimos: dict[str, dict] = {}
        for e in leer_lineas(self.datos / "ejemplos.jsonl"):
            ultimos[e["enunciado"]] = e
        todos = list(ultimos.values())
        nuevos = [e for e in todos if e.get("generacion") == self.estado.generacion]
        viejos = [e for e in todos if e.get("generacion") != self.estado.generacion]
        random.Random(self.estado.generacion).shuffle(viejos)
        elegidos = (nuevos + viejos)[: cupo or self.cfg.ejemplos_por_entrenamiento]
        return [(e["enunciado"], e["respuesta"]) for e in elegidos]

    def generacion(self) -> None:
        e = self.estado
        e.generacion += 1
        t0 = time.time()
        self.decir(f"\n— generación {e.generacion} —")
        self.inventar()
        if self._parar or self.explorar() == 0 or self._parar:
            e.generacion -= 1
            return
        ejemplos = self._ejemplos()
        copia = self.modelo.copia_adaptador()
        self.decir(f"  entreno mis pesos con {len(ejemplos)} ejemplos verificados (lr {e.lr:.1e})…")
        perdida = self.modelo.entrenar(ejemplos, self.cfg.epocas, self.cfg.lote_entrenamiento, e.lr, e.generacion, parar=self._hay_que_parar)
        val = self.medir(self.validacion) if perdida == perdida and not self._parar else []
        if self._parar:                                   # corte a medias: no se decide nada con datos incompletos
            self.modelo.restaurar_adaptador(copia)
            e.generacion -= 1
            e.guardar(self.datos)
            return
        ganados, perdidos, umbral = cur.comparar(e.validacion, val) if val else (0, 0, 1)
        aceptada = bool(val) and ganados - perdidos >= umbral
        registro = dict(tipo="generacion", ejemplos=len(ejemplos), perdida=perdida, lr=e.lr, validacion_antes=sum(e.validacion),
                        validacion_despues=sum(val), ganados=ganados, perdidos=perdidos, umbral=umbral, aceptada=aceptada,
                        capas=self.modelo.num_capas(), rango=e.rango, segundos=0)
        if aceptada:
            self.decir(f"  ✓ MEJORO: validación {self._pct(e.validacion)} → {self._pct(val)} (gano {ganados}, pierdo {perdidos}). Me quedo con estos pesos.")
            e.validacion, e.sin_mejorar = val, 0
            e.lr = min(self.cfg.lr, e.lr * 1.5)
            exa = self.medir(self.examen)
            if not self._parar:
                e.examen = exa
                self.decir(f"  examen (no decide nada, solo informa): {self._pct(exa)}; al empezar tenía {100 * e.examen_inicial:.1f} %")
            registro["examen"] = sum(e.examen)
            self._guardar_aceptado()
        else:
            motivo = "el entrenamiento divergió" if not val else f"gano {ganados}, pierdo {perdidos}; necesitaba una ventaja de {umbral}"
            self.decir(f"  ✗ no mejoro ({motivo}). Vuelvo a mis pesos anteriores.")
            self.modelo.restaurar_adaptador(copia)
            e.sin_mejorar += 1
            e.lr = max(self.cfg.lr / 8, e.lr / 2)
            e.guardar(self.datos)
        registro["segundos"] = round(time.time() - t0)
        self._registrar(**registro)
        if e.sin_mejorar >= self.cfg.paciencia and not self._parar:
            self.crecer()

    def crecer(self) -> None:
        """Estancada: elige la siguiente ampliación de capacidad, de la más barata a la más cara."""
        e, cfg = self.estado, self.cfg
        self.decir(f"  llevo {e.sin_mejorar} generaciones sin mejorar: necesito más capacidad.")
        accion = ""
        if e.multiplicador_exploracion < 2:
            e.multiplicador_exploracion = 2
            accion = "exploro el doble de problemas por generación"
        elif e.rango < cfg.rango_max:
            self.modelo.ampliar_adaptador(min(cfg.rango_max, e.rango * 2), e.modulos)
            accion = f"amplío mi adaptador de rango {e.rango} a {self.modelo.rango}"
        elif not set(MODULOS_MLP) <= set(e.modulos):
            self.modelo.ampliar_adaptador(e.rango, list(e.modulos) + [m for m in MODULOS_MLP if m not in e.modulos])
            accion = "extiendo mi adaptador a las capas de proyección (MLP)"
        elif e.capas_nuevas < cfg.max_capas_nuevas:
            antes, base_anterior = self.modelo.num_capas(), e.modelo_base
            nueva = f"bases/capas{antes + 1}_{int(time.time())}"
            if self.modelo.anadir_capa(self.datos / nueva):
                e.modelo_base, e.capas_nuevas = nueva, e.capas_nuevas + 1
                accion = f"me añado una capa nueva ({antes} → {self.modelo.num_capas()} capas)"
            else:
                e.capas_nuevas = cfg.max_capas_nuevas          # no se puede con esta arquitectura: no reintentar
                e.guardar(self.datos)
                self.decir("  no sé añadir capas a esta arquitectura; lo descarto.")
                return self.crecer()
        elif e.multiplicador_exploracion < 4:
            e.multiplicador_exploracion = 4
            accion = "exploro cuatro veces más problemas por generación"
        elif self._siguiente_modelo():
            return self.migrar(self._siguiente_modelo())
        else:
            e.sin_mejorar = 0
            e.guardar(self.datos)
            self._registrar(tipo="limite")
            self.decir("  ⚠ He agotado las ampliaciones que caben en este modelo y este hardware. Con una GPU con más memoria me "
                       "trasladaré sola a un modelo mayor; mientras tanto sigo explorando y entrenando.")
            return
        estructural = self.modelo.rango != e.rango or self.modelo.modulos != e.modulos or "capa nueva" in accion
        if estructural:                                    # comprobar que ampliar no ha cambiado lo que ya sabía
            val = self.medir(self.validacion)
            if self._parar:
                self.estado = Estado.cargar(self.datos)
                self.modelo = self._cargar_modelo()
                return
            g, p, u = cur.comparar(e.validacion, val)
            if p - g >= max(2, u):
                self.decir(f"  la ampliación empeoraba la validación ({self._pct(e.validacion)} → {self._pct(val)}): la deshago.")
                if "capa nueva" in accion:
                    shutil.rmtree(self.datos / e.modelo_base, ignore_errors=True)
                self.estado = Estado.cargar(self.datos)
                self.estado.sin_mejorar = 0
                self.estado.capas_nuevas = cfg.max_capas_nuevas if "capa nueva" in accion else self.estado.capas_nuevas
                self.estado.guardar(self.datos)
                self.modelo = self._cargar_modelo()
                return
            e.validacion, e.rango, e.modulos = val, self.modelo.rango, list(self.modelo.modulos)
            if "capa nueva" in accion and base_anterior.startswith("bases/"):
                shutil.rmtree(self.datos / base_anterior, ignore_errors=True)
        e.sin_mejorar, e.ampliaciones, e.lr = 0, e.ampliaciones + 1, cfg.lr
        self._guardar_aceptado()
        entrenables, total = self.modelo.parametros()
        self._registrar(tipo="ampliacion", accion=accion, capas=self.modelo.num_capas(), rango=e.rango, modulos=e.modulos,
                        parametros=total, entrenables=entrenables)
        self.decir(f"  ★ {accion}. Ahora tengo {total / 1e6:.1f} M de parámetros ({entrenables / 1e6:.2f} M entrenables).")

    def _siguiente_modelo(self) -> str | None:
        s = siguiente_modelo(self.estado.modelo_original)
        return s if s and s != self.estado.migracion_descartada else None

    def migrar(self, nuevo: str) -> None:
        """Se traslada a un modelo base mayor: lo entrena con todo lo que ha verificado hasta ahora y solo se queda en él
        si no acierta menos que el actual."""
        e, cfg = self.estado, self.cfg
        self.decir(f"  he agotado este modelo: pruebo a trasladarme a uno mayor ({nuevo}) llevándome lo aprendido…")
        self.modelo = None                                 # libera la memoria del modelo actual antes de cargar el mayor
        try:
            import torch
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
        except ImportError:
            pass
        try:
            self.modelo = self._crear_modelo(nuevo, None, cfg.rango_lora, list(cfg.modulos))
            ejemplos = self._ejemplos(cupo=4 * cfg.ejemplos_por_entrenamiento)
            perdida = self.modelo.entrenar(ejemplos, cfg.epocas, cfg.lote_entrenamiento, cfg.lr, e.generacion, parar=self._hay_que_parar) if ejemplos else 0.0
            val = self.medir(self.validacion) if perdida == perdida and not self._parar else []
        except Exception as error:                         # no cabe en memoria, no se pudo descargar…
            self.decir(f"  no pude usar {nuevo}: {str(error)[:200]}")
            val = []
        ganados, perdidos, _ = cur.comparar(e.validacion, val) if val else (0, 1, 1)
        if self._parar or not val or ganados < perdidos:
            if not self._parar:
                self.decir(f"  el modelo mayor no me mejora ({self._pct(e.validacion)} → {self._pct(val) if val else 'sin medida'}): sigo en el actual.")
                e.migracion_descartada, e.sin_mejorar = nuevo, 0
                e.guardar(self.datos)
                self._registrar(tipo="migracion", modelo=nuevo, aceptada=False)
            self.modelo = self._cargar_modelo()
            return
        base_anterior = e.modelo_base
        self.decir(f"  ★ ME TRASLADO a {nuevo}: validación {self._pct(e.validacion)} → {self._pct(val)}.")
        e.modelo_base = e.modelo_original = nuevo
        e.rango, e.modulos, e.capas_nuevas, e.multiplicador_exploracion = cfg.rango_lora, list(cfg.modulos), 0, 1
        e.validacion, e.sin_mejorar, e.lr, e.migraciones = val, 0, cfg.lr, e.migraciones + 1
        exa = self.medir(self.examen)
        if not self._parar:
            e.examen = exa
        self._guardar_aceptado()
        if base_anterior.startswith("bases/"):
            shutil.rmtree(self.datos / base_anterior, ignore_errors=True)
        entrenables, total = self.modelo.parametros()
        self._registrar(tipo="migracion", modelo=nuevo, aceptada=True, validacion=sum(val), examen=sum(e.examen), parametros=total)
        self.decir(f"  ahora tengo {total / 1e6:.0f} M de parámetros; examen {self._pct(e.examen)}")

    # ------------------------------------------------------------ vida
    def vivir(self, generaciones: int = 0, minutos: float = 0) -> None:
        """Bucle continuo. Para con Ctrl-C, al cumplir el límite, o nunca. Se puede cortar en cualquier momento:
        al volver a arrancar continúa desde la última versión aceptada."""
        try:
            signal.signal(signal.SIGINT, self.parar)
            signal.signal(signal.SIGTERM, self.parar)
        except ValueError:
            pass                                           # no es el hilo principal (cuaderno): se para con el límite
        t0, hechas = time.time(), 0
        if not self.inicio():
            return
        while not self._parar and (generaciones <= 0 or hechas < generaciones) and (minutos <= 0 or time.time() - t0 < minutos * 60):
            self.generacion()
            hechas += 1
        self.decir("\n" + self.resumen())

    def resumen(self) -> str:
        e = self.estado
        if not e.validacion:
            return "Todavía no me he medido. Arráncame con: python -m orion vivir"
        entrenables, total = self.modelo.parametros()
        lineas = [f"Orion · generación {e.generacion} · modelo {e.modelo_base}",
                  f"Capas: {self.modelo.num_capas()} ({e.capas_nuevas} añadidas por mí) · rango LoRA {e.rango} en {', '.join(e.modulos)} · "
                  f"{total / 1e6:.1f} M de parámetros, {entrenables / 1e6:.2f} M entrenables",
                  f"Validación: {self._pct(e.validacion)} · Examen: {self._pct(e.examen)} (al empezar: {100 * e.examen_inicial:.1f} %)",
                  "Currículo (nivel desbloqueado y acierto en él):"]
        for d in sorted(e.curriculo):
            t, n = cur.tasa(e.curriculo, d, e.curriculo[d]["nivel"])
            lineas.append(f"  {d:<12} nivel {e.curriculo[d]['nivel']}  {100 * t:3.0f} %" + ("" if n else "  (sin datos)"))
        if self.inventados:
            lineas.append(f"Ejercicios inventados por mí que funcionan: {len(self.inventados)}")
        hist = [g for g in leer_lineas(self.datos / "generaciones.jsonl") if g.get("tipo") in ("generacion", "ampliacion", "limite", "migracion")][-8:]
        if hist:
            lineas.append("Últimos pasos:")
        for g in hist:
            if g["tipo"] == "generacion":
                lineas.append(f"  g{g['generacion']}: validación {g['validacion_antes']} → {g['validacion_despues']} "
                              f"{'✓ aceptada' if g['aceptada'] else '✗ deshecha'}")
            elif g["tipo"] == "migracion":
                lineas.append(f"  g{g['generacion']}: traslado a {g['modelo']} " + ("✓ hecho" if g["aceptada"] else "✗ descartado"))
            else:
                lineas.append(f"  g{g['generacion']}: " + (g.get("accion") or "límite del modelo y del hardware alcanzado"))
        return "\n".join(lineas)
