"""Orion: un modelo que mejora sus propios pesos y su propia capacidad, midiendo cada cambio.

    python -m orion vivir                 bucle continuo de automejora (Ctrl-C para parar; al volver, continúa)
    python -m orion estado                en qué generación va, qué sabe y cómo ha cambiado
    python -m orion examen                repite ahora la validación y el examen con sus pesos actuales
    python -m orion chat                  plantearle problemas a mano
    python -m orion autoprueba            comprueba el juez (no necesita modelo)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m orion", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("orden", choices=["vivir", "estado", "examen", "chat", "autoprueba"])
    ap.add_argument("--datos", default="datos", help="carpeta donde guarda sus pesos, ejemplos y registros (por defecto: datos)")
    ap.add_argument("--modelo", default=None, help="modelo base (solo la primera vez); por defecto lo elige según la GPU")
    ap.add_argument("--generaciones", type=int, default=0, help="parar tras N generaciones (0 = sin límite)")
    ap.add_argument("--minutos", type=float, default=0, help="parar tras N minutos (0 = sin límite)")
    a = ap.parse_args(argv)
    if a.orden == "autoprueba":
        from .juez import autoprueba
        fallos = autoprueba(450)
        print("juez correcto en 450 problemas" if fallos == 0 else f"el juez falla {fallos} de 450 problemas")
        return 1 if fallos else 0
    from .bucle import Orion
    o = Orion(Path(a.datos), a.modelo)
    if a.orden == "vivir":
        o.vivir(a.generaciones, a.minutos)
    elif a.orden == "estado":
        print(o.resumen())
    elif a.orden == "examen":
        print("validación:", o._pct(o.medir(o.validacion)))
        print("examen:    ", o._pct(o.medir(o.examen)))
    elif a.orden == "chat":
        print("Escribe un problema (línea vacía para salir).")
        while True:
            try:
                t = input("> ").strip()
            except EOFError:
                break
            if not t:
                break
            print(o.modelo.generar([t], max_tokens=o.cfg.max_tokens)[0][0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
