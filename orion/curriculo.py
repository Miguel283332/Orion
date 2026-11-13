"""Qué practicar y cuándo aceptar un cambio. Sin modelo: son reglas puras, fáciles de probar."""
from __future__ import annotations

import math
import random

from .problemas import DOMINIOS, NIVEL_MAX, Problema, generar

OLVIDO = 0.7            # peso de lo anterior al anotar resultados nuevos
UMBRAL_SUBIR = 0.6      # tasa de acierto para desbloquear el nivel siguiente
MIN_INTENTOS = 8.0


def _dom(curriculo: dict, dominio: str) -> dict:
    return curriculo.setdefault(dominio, {"nivel": 1, "tasas": {}})


def tasa(curriculo: dict, dominio: str, nivel: int) -> tuple[float, float]:
    """(tasa de acierto, intentos acumulados) en ese dominio y nivel."""
    a, n = _dom(curriculo, dominio)["tasas"].get(str(nivel), [0.0, 0.0])
    return (a / n if n > 0 else 0.5), n


def elegir(curriculo: dict, n: int, r: random.Random, excluir: set[str]) -> list[Problema]:
    """n problemas de los niveles desbloqueados, con más peso donde más falla."""
    opciones, pesos = [], []
    for d in DOMINIOS:
        for nivel in range(1, _dom(curriculo, d)["nivel"] + 1):
            t, _ = tasa(curriculo, d, nivel)
            opciones.append((d, nivel))
            pesos.append(0.2 + (1.0 - t))
    salida, vistos = [], set(excluir)
    for _ in range(n * 20):
        if len(salida) >= n:
            break
        d, nivel = r.choices(opciones, pesos)[0]
        p = generar(d, nivel, r)
        if p.huella not in vistos:
            vistos.add(p.huella)
            salida.append(p)
    return salida


def anotar(curriculo: dict, resultados: list[tuple[Problema, float]]) -> list[str]:
    """Anota la fracción de intentos acertados de cada problema y desbloquea niveles. Devuelve los avisos de subida."""
    nuevos: dict[tuple[str, int], list[float]] = {}
    for p, fraccion in resultados:
        nuevos.setdefault((p.dominio, p.nivel), []).append(fraccion)
    for (d, nivel), v in nuevos.items():
        a, n = _dom(curriculo, d)["tasas"].get(str(nivel), [0.0, 0.0])
        _dom(curriculo, d)["tasas"][str(nivel)] = [a * OLVIDO + sum(v), n * OLVIDO + len(v)]
    avisos = []
    for d in DOMINIOS:
        actual = _dom(curriculo, d)["nivel"]
        t, n = tasa(curriculo, d, actual)
        if actual < NIVEL_MAX and n >= MIN_INTENTOS and t >= UMBRAL_SUBIR:
            _dom(curriculo, d)["nivel"] = actual + 1
            avisos.append(f"{d}: paso al nivel {actual + 1} (acertaba el {100 * t:.0f} % del nivel {actual})")
    return avisos


def comparar(antes: list[int], despues: list[int]) -> tuple[int, int, int]:
    """(ganados, perdidos, umbral): problemas que antes fallaba y ahora acierta, al revés, y la ventaja mínima para
    aceptar. El umbral crece con el número de cambios para no confundir el azar con una mejora."""
    ganados = sum(1 for a, d in zip(antes, despues) if d and not a)
    perdidos = sum(1 for a, d in zip(antes, despues) if a and not d)
    return ganados, perdidos, max(1, math.ceil(math.sqrt(ganados + perdidos)))


def mejora(antes: list[int], despues: list[int]) -> bool:
    g, p, u = comparar(antes, despues)
    return g - p >= u
