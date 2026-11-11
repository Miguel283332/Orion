#!/usr/bin/env python3
"""Genera colab/Demostrador_Colab.ipynb con el código de demostrador.py y los problemas INCRUSTADOS (celdas %%writefile).

El cuaderno resultante no necesita `git clone`: se abre en Colab y funciona. Ejecuta este script después de tocar
demostrador.py o problemas/*.json (una prueba comprueba que el cuaderno está sincronizado).
    python tools/generar_notebook.py
"""
import json
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
PROBLEMAS = ["facil_par_mayor.json", "facil_velocidad_escape_schwarzschild.json"]


def md(t):
    return {"cell_type": "markdown", "metadata": {}, "source": t.strip("\n").splitlines(True)}


def code(t, **meta):
    return {"cell_type": "code", "metadata": meta, "execution_count": None, "outputs": [], "source": t.strip("\n").splitlines(True)}


def escribir(ruta, texto):
    return code(f"%%writefile {ruta}\n{texto.rstrip(chr(10))}")


def construir():
    fuente = (RAIZ / "demostrador.py").read_text(encoding="utf-8")
    celdas = [
        md("""
# Demostrador en Google Colab (cuaderno autocontenido)

Un modelo local de **Ollama** *propone* y un verificador externo *decide*: **Lean 4** para matemáticas y **SymPy** para física.
Guarda el progreso tras **cada intento**: si lo interrumpes (botón de parar, desconexión, límite de tiempo) o falla,
**volver a ejecutar la celda continúa donde se quedó**.

**El código está dentro de este cuaderno** (celda *Código del demostrador*): no hace falta `git clone`. Si lo cambias, vuelve a ejecutar esa celda.

Menú *Entorno de ejecución → Cambiar tipo de entorno → GPU (T4)*.
"""),
        md("## 1. Instalar Ollama, Lean 4 y el modelo (una vez por sesión)"),
        code("""
# Dependencias y Ollama
!apt-get -qq install -y zstd pciutils lshw
!curl -fsSL https://ollama.com/install.sh | sh
!pip -q install sympy

import subprocess, time
subprocess.Popen(["ollama", "serve"])
time.sleep(6)
"""),
        code("""
# Lean 4 (vía elan)
!curl https://raw.githubusercontent.com/leanprover/elan/master/elan-init.sh -sSf | sh -s -- -y --default-toolchain stable
!/root/.elan/bin/lean --version
"""),
        code("""
MODELO = 'qwen2.5:7b'
!ollama pull {MODELO}
"""),
        md("## 2. Dónde guardar el progreso\n\nCon Google Drive montado, el progreso **sobrevive** aunque se cierre la sesión."),
        code("""
from google.colab import drive
drive.mount('/content/drive')
DIR = '/content/drive/MyDrive/demostrador'      # estado y resultados
LEAN = '/root/.elan/bin/lean'
"""),
        md("## 3. Código del demostrador\n\nEjecuta esta celda (y la siguiente) antes de resolver. Contiene el programa completo."),
        code("%%writefile demostrador.py\n" + fuente.rstrip("\n"), cellView="form" if False else None),
        md("### Problemas de ejemplo"),
        code("!mkdir -p problemas   # %%writefile no crea carpetas"),
    ]
    for nombre in PROBLEMAS:
        celdas.append(escribir(f"problemas/{nombre}", (RAIZ / "problemas" / nombre).read_text(encoding="utf-8")))
    celdas += [
        code("!python demostrador.py --help | head -4"),
        md("""
## 4. Problema fácil (matemáticas, Lean 4)

*«Demuestra la existencia de un número natural m mayor que cualquier n dado, tal que m sea par.»*

- Si falla o lo interrumpes, **vuelve a ejecutar la celda**: reintenta con los contadores a cero y conserva lo ya demostrado.
- Para empezar de cero, añade `--reiniciar`.
"""),
        code("!python demostrador.py resolver --problema problemas/facil_par_mayor.json --modelo {MODELO} --lean {LEAN} --dir {DIR}/par_mayor"),
        md("## 5. Problema fácil de física (SymPy): velocidad de escape y radio de Schwarzschild"),
        code("!python demostrador.py resolver --problema problemas/facil_velocidad_escape_schwarzschild.json --modelo {MODELO} --dir {DIR}/escape"),
        md("""
> En física, los **criterios** de verificación los escribe el modelo antes de resolver (salen en el registro). **Revísalos**: la verificación es tan fiable como esos criterios.

## 6. Ver el progreso y el resultado
"""),
        code("""
!python demostrador.py estado --dir {DIR}/par_mayor
print()
!python demostrador.py estado --dir {DIR}/escape
"""),
        code("print(open(f'{DIR}/par_mayor/resultado.lean').read())"),
        md("""
## 7. Problemas largos o difíciles

- Dale **horas**: `--horas 3` (límite de la sesión; al llegar se pausa con el progreso guardado).
- Más intentos y descomposición más profunda: `--directos 10 --replanes 5 --profundidad 3`.
- Un modelo mayor ayuda mucho: `MODELO = 'qwen2.5:14b'` (si la GPU lo aguanta).
- **Sin GPU (solo CPU)** cada llamada de un 7B tarda minutos. Para acelerar: `--num-predict 600 --num-ctx 4096` (respuestas más cortas) y/o un modelo más pequeño (`qwen2.5:3b`). Lo ideal es esperar a que se renueve la cuota de GPU: el progreso queda guardado en Drive y se reanuda.
- En segundo plano, consultando el registro:
"""),
        code("!nohup python demostrador.py resolver --problema problemas/facil_par_mayor.json --modelo {MODELO} --lean {LEAN} --dir {DIR}/par_mayor --horas 3 > {DIR}/par_mayor.log 2>&1 &"),
        code("!tail -n 20 {DIR}/par_mayor.log"),
        md("""
Los problemas con **Mathlib** (números reales, análisis...) necesitan un proyecto *lake* con Mathlib (descarga grande); después: `--lake-dir RUTA --mathlib`.
Para añadir problemas nuevos (agujeros negros, espacio-tiempo), crea archivos `.json` en `problemas/` con `enunciado`, `backend` (`lean` o `sympy`) y `dominio`.
"""),
    ]
    return {"cells": celdas, "metadata": {"colab": {"provenance": []}, "kernelspec": {"name": "python3", "display_name": "Python 3"},
                                          "language_info": {"name": "python"}, "accelerator": "GPU"}, "nbformat": 4, "nbformat_minor": 5}


if __name__ == "__main__":
    destino = RAIZ / "colab" / "Demostrador_Colab.ipynb"
    destino.parent.mkdir(exist_ok=True)
    destino.write_text(json.dumps(construir(), ensure_ascii=False, indent=1), encoding="utf-8")
    print("escrito", destino, f"({destino.stat().st_size // 1024} KB)")
