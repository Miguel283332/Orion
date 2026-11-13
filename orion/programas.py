"""Programación: Orion escribe funciones de Python y un juez las ejecuta contra casos de prueba.

Tres piezas:
- un ejecutor aislado (otro proceso, con límite de tiempo y memoria, y solo código que pasa un filtro estricto);
- problemas de programación generados por familias, con 5 niveles;
- ejercicios INVENTADOS por el propio modelo: propone un enunciado, una solución y unas entradas; si la solución
  se ejecuta bien y es determinista, el ejercicio entra en su currículo y más adelante tendrá que resolverlo solo.
"""
from __future__ import annotations

import ast
import json
import random
import re
import subprocess
import sys
import tempfile

from .problemas import Problema

IMPORTS_PERMITIDOS = {"math", "itertools", "collections", "functools", "re", "string", "heapq", "bisect", "fractions", "operator"}
NOMBRES_PROHIBIDOS = {"open", "exec", "eval", "compile", "__import__", "input", "globals", "locals", "getattr", "setattr", "delattr",
                      "vars", "breakpoint", "exit", "quit", "help", "memoryview", "print"}
MARCA_SALIDA = "@@ORION@@"

_NORMALIZAR = '''
def _norm(v):
    if isinstance(v, float):
        return round(v, 6)
    if v is None or isinstance(v, (bool, int, str)):
        return v
    if isinstance(v, (list, tuple)):
        return [_norm(x) for x in v]
    if isinstance(v, (set, frozenset)):
        return sorted((_norm(x) for x in v), key=repr)
    if isinstance(v, dict):
        return {str(k): _norm(x) for k, x in sorted(v.items(), key=lambda p: repr(p[0]))}
    return repr(v)
'''
exec(_NORMALIZAR)   # define _norm también aquí: las salidas de referencia se normalizan igual que las del modelo

_ARNES = _NORMALIZAR + f'''
import json, sys
d = json.load(sys.stdin)
ns = {{}}
exec(compile(d["codigo"], "<orion>", "exec"), ns)
f = ns[d["funcion"]]
print("{MARCA_SALIDA}" + json.dumps([_norm(f(*a)) for a in d["entradas"]]))
'''


def codigo_seguro(codigo: str) -> bool:
    """Solo se ejecuta código que no importa nada fuera de una lista corta ni toca archivos, red o el intérprete."""
    if len(codigo) > 4000:
        return False
    try:
        arbol = ast.parse(codigo)
    except (SyntaxError, ValueError, RecursionError):
        return False
    for n in ast.walk(arbol):
        if isinstance(n, ast.Import) and any(a.name.split(".")[0] not in IMPORTS_PERMITIDOS for a in n.names):
            return False
        if isinstance(n, ast.ImportFrom) and (n.module or "").split(".")[0] not in IMPORTS_PERMITIDOS:
            return False
        if isinstance(n, ast.Name) and (n.id in NOMBRES_PROHIBIDOS or n.id.startswith("__")):
            return False
        if isinstance(n, ast.Attribute) and n.attr.startswith("_"):
            return False
        if isinstance(n, (ast.Global, ast.Nonlocal, ast.AsyncFunctionDef, ast.Await)):
            return False
    return True


def _limites():
    import resource
    resource.setrlimit(resource.RLIMIT_CPU, (5, 5))
    resource.setrlimit(resource.RLIMIT_AS, (1 << 30, 1 << 30))
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))


def ejecutar(codigo: str, funcion: str, entradas: list, segundos: float = 6.0) -> list | None:
    """Ejecuta funcion(*entrada) para cada entrada en un proceso aparte. Devuelve las salidas, o None si algo falla."""
    if not codigo_seguro(codigo):
        return None
    try:
        with tempfile.TemporaryDirectory() as carpeta:
            p = subprocess.run([sys.executable, "-I", "-c", _ARNES], input=json.dumps({"codigo": codigo, "funcion": funcion, "entradas": entradas}),
                               capture_output=True, text=True, timeout=segundos, cwd=carpeta, env={},
                               preexec_fn=_limites if sys.platform != "win32" else None)
    except (subprocess.TimeoutExpired, OSError, ValueError):
        return None
    if p.returncode != 0 or MARCA_SALIDA not in p.stdout:
        return None
    try:
        return json.loads(p.stdout.rsplit(MARCA_SALIDA, 1)[1])
    except json.JSONDecodeError:
        return None


def extraer_codigo(texto: str, funcion: str) -> str | None:
    """El último bloque de código de la respuesta que define la función pedida."""
    bloques = re.findall(r"```[a-zA-Z]*[ \t]*\n(.*?)```", texto, re.S)
    abierto = re.search(r"```[a-zA-Z]*[ \t]*\n((?:(?!```).)*)$", texto, re.S)      # bloque sin cerrar (respuesta cortada)
    if abierto:
        bloques.append(abierto.group(1))
    if not bloques and "def " in texto:
        bloques = [texto[texto.index("def "):]]
    for b in reversed(bloques):
        if re.search(rf"def\s+{re.escape(funcion)}\s*\(", b):
            return b.strip()
    return None


def correcto(p: Problema, texto: str) -> bool:
    d = json.loads(p.verdad)
    codigo = extraer_codigo(texto, d["funcion"])
    return codigo is not None and ejecutar(codigo, d["funcion"], d["entradas"]) == d["salidas"]


# ---------------------------------------------------------------- problemas generados
def _listas(r, lo=-9, hi=20):
    return [[[r.randint(lo, hi) for _ in range(n)]] for n in (0, 1, r.randint(3, 6), r.randint(5, 9), r.randint(6, 12))]


def _enteros(r):
    return [[r.randint(-60, 60)] for _ in range(5)] + [[0]]


def _naturales(r):
    return [[0], [1]] + [[r.randint(2, 400)] for _ in range(4)]


_PALABRAS = ["sol", "luna", "orion", "estrella", "camino", "red", "pensar", "azul", "tiempo", "materia"]


def _palabras(r):
    return [[""]] + [[r.choice(_PALABRAS)] for _ in range(4)]


def _frases(r):
    return [[""]] + [[" ".join(r.choice(_PALABRAS) for _ in range(r.randint(1, 4))) + r.choice(["", "!", " 42"])] for _ in range(4)]


# (nivel, nombre, argumentos, qué debe hacer, cuerpo de referencia, entradas, rango de k)
FAMILIAS = [
    (1, "doble_mas", "n", "devuelva el doble de n más {k}", "return 2 * n + {k}", _enteros, (1, 30)),
    (1, "es_multiplo", "n", "devuelva True si n es múltiplo de {k} y False si no", "return n % {k} == 0", _enteros, (2, 12)),
    (1, "cuantos_mayores", "lista", "devuelva cuántos elementos de la lista son mayores que {k}", "return sum(1 for v in lista if v > {k})", _listas, (-5, 15)),
    (1, "repetir", "s", "devuelva la cadena s repetida {k} veces", "return s * {k}", _palabras, (2, 6)),
    (2, "suma_multiplos", "lista", "devuelva la suma de los elementos de la lista que son múltiplos de {k}",
     "return sum(v for v in lista if v % {k} == 0)", _listas, (2, 7)),
    (2, "filtrar_menores", "lista", "devuelva una lista con los elementos menores que {k}, en el mismo orden",
     "return [v for v in lista if v < {k}]", _listas, (-3, 15)),
    (2, "cabecera", "s", "devuelva los primeros {k} caracteres de s en mayúsculas (o todos, si tiene menos)", "return s[:{k}].upper()", _palabras, (1, 5)),
    (2, "uno_de_cada", "lista", "devuelva una lista con un elemento de cada {k}, empezando por el primero", "return lista[::{k}]", _listas, (2, 4)),
    (3, "rotar", "lista", "devuelva la lista rotada {k} posiciones a la izquierda (si está vacía, devuelve una lista vacía)",
     "if not lista:\n    return []\nd = {k} % len(lista)\nreturn lista[d:] + lista[:d]", _listas, (1, 7)),
    (3, "mayores", "lista", "devuelva los {k} elementos mayores de la lista, de mayor a menor (o todos, si hay menos)",
     "return sorted(lista, reverse=True)[:{k}]", _listas, (1, 4)),
    (3, "cifrar", "s", "cifre s con el cifrado César: cada letra minúscula de la a a la z avanza {k} posiciones (tras la z vuelve a la a) y "
     "los demás caracteres no cambian", "return ''.join(chr((ord(c) - 97 + {k}) % 26 + 97) if 'a' <= c <= 'z' else c for c in s)", _frases, (1, 25)),
    (3, "suma_cifras", "n", "devuelva la suma de las cifras de n (entero no negativo) escrito en base {k}",
     "t = 0\nwhile n > 0:\n    t += n % {k}\n    n //= {k}\nreturn t", _naturales, (2, 10)),
    (3, "trozos", "lista", "divida la lista en trozos consecutivos de {k} elementos (el último puede ser más corto) y devuelva la lista de trozos",
     "return [lista[i:i + {k}] for i in range(0, len(lista), {k})]", _listas, (2, 4)),
    (4, "primos_mayores", "n", "devuelva una lista con los primeros n números primos mayores que {k}",
     "r, c = [], {k} + 1\nwhile len(r) < n:\n    if c > 1 and all(c % d for d in range(2, int(c ** 0.5) + 1)):\n        r.append(c)\n    c += 1\nreturn r",
     lambda r: [[n] for n in (0, 1, 2, 4, 7)], (0, 200)),
    (4, "ventana", "lista", "devuelva la mayor suma de {k} elementos consecutivos de la lista (0 si la lista tiene menos de {k} elementos)",
     "if len(lista) < {k}:\n    return 0\nreturn max(sum(lista[i:i + {k}]) for i in range(len(lista) - {k} + 1))", _listas, (2, 4)),
    (4, "a_base", "n", "devuelva n (entero no negativo) escrito en base {k}, como cadena de cifras",
     "if n == 0:\n    return '0'\ns = ''\nwhile n > 0:\n    s = str(n % {k}) + s\n    n //= {k}\nreturn s", _naturales, (2, 9)),
    (4, "pares_suma", "lista", "devuelva cuántas parejas de posiciones distintas (i < j) cumplen lista[i] + lista[j] == {k}",
     "return sum(1 for i in range(len(lista)) for j in range(i + 1, len(lista)) if lista[i] + lista[j] == {k})", lambda r: _listas(r, 0, 9), (5, 14)),
    (4, "frecuentes", "lista", "devuelva los {k} valores más frecuentes de la lista, del más al menos frecuente; si empatan, primero el menor",
     "c = {}\nfor v in lista:\n    c[v] = c.get(v, 0) + 1\nreturn [v for v, _ in sorted(c.items(), key=lambda p: (-p[1], p[0]))[:{k}]]",
     lambda r: _listas(r, 0, 4), (1, 3)),
    (5, "monedas", "n", "devuelva el mínimo número de monedas de valores 1, {k} y {k2} necesarias para sumar exactamente n (n no negativo)",
     "m = [0] + [n + 1] * n\nfor i in range(1, n + 1):\n    for c in (1, {k}, {k2}):\n        if c <= i:\n            m[i] = min(m[i], m[i - c] + 1)\nreturn m[n]",
     lambda r: [[0], [1]] + [[r.randint(2, 90)] for _ in range(4)], (3, 9)),
    (5, "particiones", "n", "devuelva de cuántas formas se puede escribir n como suma de enteros positivos menores o iguales que {k}, "
     "sin importar el orden (para n = 0 la respuesta es 1)",
     "f = [1] + [0] * n\nfor p in range(1, {k} + 1):\n    for i in range(p, n + 1):\n        f[i] += f[i - p]\nreturn f[n]",
     lambda r: [[0], [1]] + [[r.randint(2, 30)] for _ in range(4)], (2, 6)),
    (5, "creciente", "lista", "devuelva la longitud de la subsecuencia más larga (no necesariamente contigua) en la que cada elemento supera al "
     "anterior en al menos {k} (0 si la lista está vacía)",
     "m = [1] * len(lista)\nfor i in range(len(lista)):\n    for j in range(i):\n        if lista[i] - lista[j] >= {k}:\n            m[i] = max(m[i], m[j] + 1)\nreturn max(m, default=0)",
     _listas, (1, 5)),
]


def _fuente(nombre: str, args: str, cuerpo: str) -> str:
    return f"def {nombre}({args}):\n" + "\n".join("    " + l for l in cuerpo.split("\n"))


def _salidas_de_referencia(fuente: str, nombre: str, entradas: list) -> list:
    ns: dict = {}
    exec(fuente, ns)                                   # código de este archivo, no del modelo
    return [_norm(ns[nombre](*json.loads(json.dumps(a)))) for a in entradas]   # noqa: F821


def codigo(r: random.Random, nivel: int) -> Problema:
    _, nombre, args, desc, cuerpo, entradas, (lo, hi) = r.choice([f for f in FAMILIAS if f[0] == nivel])
    k = r.randint(lo, hi)
    k2 = k + r.randint(2, 6)

    def cambia(t: str) -> str:
        return t.replace("{k2}", str(k2)).replace("{k}", str(k))

    fuente = _fuente(nombre, args, cambia(cuerpo))
    ent = entradas(r)
    verdad = {"funcion": nombre, "entradas": ent, "salidas": _salidas_de_referencia(fuente, nombre, ent)}
    return Problema("codigo", nivel, f"Escribe una función de Python `{nombre}({args})` que {cambia(desc)}.", "codigo",
                    json.dumps(verdad, ensure_ascii=False), fuente)


# ---------------------------------------------------------------- ejercicios inventados por el modelo
TEMAS = ["listas de números", "cadenas de texto", "números enteros", "diccionarios", "matrices (listas de listas)", "conjuntos", "recursión",
         "ordenación", "búsqueda", "fechas y horas representadas con números", "geometría con coordenadas", "juegos de tablero sencillos",
         "contar y agrupar", "transformar texto", "secuencias numéricas", "fracciones", "números binarios", "intervalos"]
DIFICULTADES = ["fácil", "de dificultad media", "difícil"]

SISTEMA_INVENTAR = ("Eres Orion. Inventas ejercicios de programación para entrenarte a ti misma. Sigue EXACTAMENTE el formato pedido.")


def peticion_de_invento(r: random.Random) -> str:
    return (f"Inventa un ejercicio {r.choice(DIFICULTADES)} de programación en Python sobre {r.choice(TEMAS)}. Que sea original.\n"
            "Formato exacto de tu respuesta:\n"
            "ENUNCIADO: una frase que diga con precisión qué debe devolver la función.\n"
            "```python\ndef nombre_de_la_funcion(argumentos):\n    ...\n```\n"
            "ENTRADAS: una lista de Python con 5 listas de argumentos, por ejemplo [[1, 2], [3, 4], [0, 0], [5, 1], [9, 9]]\n"
            "La función debe ser pura: sin print, sin leer nada, sin números aleatorios.")


def invento_valido(texto: str) -> Problema | None:
    """Convierte la propuesta del modelo en un problema, solo si su solución se ejecuta bien y dice algo."""
    m = re.search(r"ENUNCIADO:\s*(.+)", texto)
    e = re.search(r"ENTRADAS:\s*(\[.*)", texto, re.S)
    f = re.search(r"def\s+([a-zA-Z_]\w*)\s*\(([^)]*)\)", texto)
    if not (m and e and f):
        return None
    nombre, args, desc = f.group(1), re.sub(r"\s+", " ", f.group(2)).strip(), m.group(1).strip().rstrip(".")
    fuente = extraer_codigo(texto[: e.start()], nombre)
    if not fuente or not 15 <= len(desc) <= 400 or nombre.startswith("_"):
        return None
    entradas = None
    literal = e.group(1).strip().split("```")[0]
    for fin in range(len(literal), 0, -1):              # la lista puede ir seguida de más texto
        if literal[fin - 1] != "]":
            continue
        try:
            entradas = ast.literal_eval(literal[:fin])
            break
        except (ValueError, SyntaxError, MemoryError, RecursionError):
            continue
    if not isinstance(entradas, list) or not 3 <= len(entradas) <= 12:
        return None
    entradas = [list(a) if isinstance(a, (list, tuple)) else [a] for a in entradas]
    try:
        entradas = json.loads(json.dumps(entradas))
    except (TypeError, ValueError):
        return None
    salidas = ejecutar(fuente, nombre, entradas)
    if salidas is None or salidas != ejecutar(fuente, nombre, entradas):          # debe funcionar y ser determinista
        return None
    if len({json.dumps(s, sort_keys=True) for s in salidas}) < 2 or len(json.dumps(salidas)) > 2000:   # no trivial
        return None
    verdad = {"funcion": nombre, "entradas": entradas, "salidas": salidas}
    return Problema("inventados", 1, f"Escribe una función de Python `{nombre}({args})` que haga lo siguiente: {desc}.", "codigo",
                    json.dumps(verdad, ensure_ascii=False), fuente)
