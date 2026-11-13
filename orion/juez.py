"""El juez: decide si la respuesta del modelo es correcta comparándola con la verdad calculada por SymPy.

No ejecuta nada de lo que escribe el modelo: solo acepta expresiones matemáticas con un vocabulario cerrado y las
compara numéricamente en varios puntos (rápido y sin bloqueos, a diferencia de `simplify`).
"""
from __future__ import annotations

import math
import random
import re

import sympy as sp
from sympy.parsing.sympy_parser import (convert_xor, implicit_multiplication_application, parse_expr,
                                        standard_transformations)

from .problemas import Problema

MARCA = "RESPUESTA:"
_TRANSFORMACIONES = standard_transformations + (implicit_multiplication_application, convert_xor)
_x, _y = sp.Symbol("x"), sp.Symbol("y")
_NOMBRES = {"x": _x, "y": _y, "sin": sp.sin, "cos": sp.cos, "tan": sp.tan, "exp": sp.exp, "log": sp.log, "ln": sp.log,
            "sqrt": sp.sqrt, "pi": sp.pi, "e": sp.E, "E": sp.E, "sen": sp.sin}
_PERMITIDO = re.compile(r"^[0-9a-zA-Z+\-*/^()., ]*$")
_PALABRA = re.compile(r"[a-zA-Z_]+")
_PUNTOS = [0.37, -1.21, 2.63, 0.81, -0.53]


def extraer_respuesta(texto: str) -> str | None:
    """Lo que hay tras la última marca RESPUESTA: (una sola línea)."""
    p = texto.rfind(MARCA)
    if p < 0:
        return None
    lineas = texto[p + len(MARCA):].strip().splitlines()
    return lineas[0].strip() if lineas else None


def _limpiar(s: str) -> str:
    s = s.replace("−", "-").replace("·", "*").replace("×", "*").replace("÷", "/").replace("²", "^2").replace("³", "^3")
    s = s.replace("$", "").replace("\\", "").replace("`", "").strip().rstrip(".").strip()
    return s


def _expr(s: str):
    """Convierte texto en una expresión de SymPy, o None si no es una expresión matemática válida y segura."""
    s = _limpiar(s)
    s = re.sub(r"^\s*[a-zA-Z]\w*'?(\([a-z]\))?\s*=\s*", "", s)      # "x = 3", "f'(x) = ..."
    if not s or len(s) > 300 or not _PERMITIDO.match(s) or "__" in s:
        return None
    if any(w not in _NOMBRES for w in _PALABRA.findall(s)):
        return None
    try:
        e = parse_expr(s, local_dict=dict(_NOMBRES), global_dict={"Integer": sp.Integer, "Float": sp.Float, "Rational": sp.Rational,
                                                                  "Symbol": sp.Symbol}, transformations=_TRANSFORMACIONES)
        return e if isinstance(e, sp.Expr) else None
    except Exception:
        return None


def _valor(e, punto: float):
    try:
        v = complex(e.subs({_x: punto, _y: punto * 0.7 + 0.3}).evalf(15))
        return v if math.isfinite(v.real) and math.isfinite(v.imag) else None
    except Exception:
        return None


def _equivalentes(a, b, tolerancia: float = 1e-6) -> bool:
    comparados = 0
    for p in _PUNTOS:
        va, vb = _valor(a, p), _valor(b, p)
        if va is None and vb is None:
            continue
        if va is None or vb is None or abs(va - vb) > tolerancia * max(1.0, abs(vb)):
            return False
        comparados += 1
    return comparados >= 2


def _partes(s: str) -> list[str]:
    s = re.sub(r"\b(y|and)\b", ",", _limpiar(s))
    return [t for t in (p.strip() for p in s.replace(";", ",").split(",")) if t]


def correcta(p: Problema, respuesta: str | None) -> bool:
    if not respuesta:
        return False
    try:
        if p.tipo == "num":
            e = _expr(respuesta)
            if e is None:                                   # "45 m", "45 metros": se toma el primer número
                m = re.search(r"[-+]?\d+(?:[.,]\d+)?", _limpiar(respuesta))
                e = _expr(m.group(0).replace(",", ".")) if m else None
            return e is not None and not e.free_symbols and _equivalentes(e, sp.sympify(p.verdad), 5e-3)
        if p.tipo == "expr":
            e = _expr(respuesta)
            return e is not None and _equivalentes(e, sp.sympify(p.verdad))
        if p.tipo == "primitiva":
            e = _expr(respuesta)
            return e is not None and _equivalentes(sp.diff(e, _x), sp.sympify(p.verdad))
        if p.tipo in ("lista", "conjunto"):
            dadas = [_expr(t) for t in _partes(respuesta)]
            verdad = [sp.sympify(t) for t in p.verdad.split(",")]
            if any(d is None for d in dadas) or len(dadas) != len(verdad):
                return False
            if p.tipo == "lista":
                return all(_equivalentes(d, v) for d, v in zip(dadas, verdad))
            libres = list(verdad)
            for d in dadas:
                i = next((i for i, v in enumerate(libres) if _equivalentes(d, v)), None)
                if i is None:
                    return False
                libres.pop(i)
            return True
    except Exception:
        return False
    return False


def juzgar(p: Problema, texto: str) -> bool:
    """¿Es correcta la respuesta completa del modelo (razonamiento + línea RESPUESTA:)?"""
    return correcta(p, extraer_respuesta(texto))


def autoprueba(n: int = 200, semilla: int = 0) -> int:
    """Comprueba que el juez acepta la verdad de n problemas al azar. Devuelve cuántos falla (debe ser 0)."""
    from .problemas import DOMINIOS, generar
    r, fallos = random.Random(semilla), 0
    for i in range(n):
        p = generar(DOMINIOS[i % len(DOMINIOS)], i % 5 + 1, r)
        verdad = p.verdad
        if p.tipo == "primitiva":
            verdad = sp.sstr(sp.integrate(sp.sympify(p.verdad), _x))
        fallos += not correcta(p, verdad)
    return fallos
