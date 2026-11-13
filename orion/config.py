"""Configuración y estado persistente. Todo vive en la carpeta de datos, en JSON legible."""
from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path


@dataclass
class Config:
    modelo: str = "auto"                 # "auto" elige según la VRAM; o un nombre de Hugging Face o una carpeta local
    # exploración (cada generación)
    problemas_por_generacion: int = 96
    intentos_por_problema: int = 4       # respuestas distintas por problema; el juez se queda con las buenas
    temperatura: float = 0.8
    max_tokens: int = 256
    lote_generacion: int = 16
    usar_soluciones_de_referencia: bool = True   # si falla todos los intentos, aprende de la respuesta correcta conocida
    inventos_por_generacion: int = 8     # ejercicios de programación que intenta inventarse en cada generación (0 = ninguno)
    parte_inventados: float = 0.2        # fracción de la exploración dedicada a resolver sus propios ejercicios
    # entrenamiento
    ejemplos_por_entrenamiento: int = 512
    epocas: int = 2
    lote_entrenamiento: int = 8
    lr: float = 2e-4
    # medida
    n_validacion: int = 180              # decide si una generación se acepta
    n_examen: int = 180                  # nunca decide nada: solo informa del progreso real
    # capacidad
    rango_lora: int = 16
    rango_max: int = 128
    modulos: list = field(default_factory=lambda: ["q_proj", "k_proj", "v_proj", "o_proj"])
    max_capas_nuevas: int = 4
    paciencia: int = 3                   # generaciones seguidas sin mejorar antes de ampliar la capacidad
    semilla: int = 1234

    @staticmethod
    def cargar(carpeta: Path) -> "Config":
        f = carpeta / "config.json"
        c = Config()
        if f.exists():
            datos = json.loads(f.read_text(encoding="utf-8"))
            for campo in fields(Config):
                if campo.name in datos:
                    setattr(c, campo.name, datos[campo.name])
        else:
            carpeta.mkdir(parents=True, exist_ok=True)
            escribir_json(f, asdict(c))
        return c


@dataclass
class Estado:
    generacion: int = 0
    modelo_base: str = ""                # nombre o carpeta del modelo sobre el que va el adaptador
    modelo_original: str = ""            # el modelo de Hugging Face del que desciende el actual
    migracion_descartada: str = ""       # modelo mayor que ya probó y no le mejoró
    migraciones: int = 0
    adaptador: str = ""                  # carpeta (relativa a datos) del adaptador aceptado; "" = ninguno todavía
    rango: int = 0
    modulos: list = field(default_factory=list)
    capas_nuevas: int = 0
    validacion: list = field(default_factory=list)   # acierto (0/1) en cada problema de validación, versión aceptada
    examen: list = field(default_factory=list)
    examen_inicial: float = -1.0
    sin_mejorar: int = 0
    ampliaciones: int = 0
    lr: float = 0.0
    multiplicador_exploracion: int = 1
    curriculo: dict = field(default_factory=dict)    # dominio -> {"nivel": n, "tasas": {nivel: [aciertos, intentos]}}

    @staticmethod
    def cargar(carpeta: Path) -> "Estado":
        f = carpeta / "estado.json"
        e = Estado()
        if f.exists():
            datos = json.loads(f.read_text(encoding="utf-8"))
            for campo in fields(Estado):
                if campo.name in datos:
                    setattr(e, campo.name, datos[campo.name])
        return e

    def guardar(self, carpeta: Path) -> None:
        escribir_json(carpeta / "estado.json", asdict(self))


def escribir_json(archivo: Path, datos) -> None:
    """Escritura atómica: un corte de luz o de Colab nunca deja el archivo a medias."""
    archivo.parent.mkdir(parents=True, exist_ok=True)
    tmp = archivo.with_suffix(archivo.suffix + ".part")
    tmp.write_text(json.dumps(datos, ensure_ascii=False, indent=1), encoding="utf-8")
    os.replace(tmp, archivo)


def anadir_linea(archivo: Path, datos) -> None:
    archivo.parent.mkdir(parents=True, exist_ok=True)
    with archivo.open("a", encoding="utf-8") as f:
        f.write(json.dumps(datos, ensure_ascii=False) + "\n")


def leer_lineas(archivo: Path) -> list:
    if not archivo.exists():
        return []
    salida = []
    for linea in archivo.read_text(encoding="utf-8").splitlines():
        try:
            salida.append(json.loads(linea))
        except json.JSONDecodeError:
            pass                          # última línea cortada por un apagón
    return salida
