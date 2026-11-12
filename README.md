# Orion

Orion es una IA autónoma que **vive en tu ordenador**. Está siempre en marcha:
- **habla contigo por la terminal**;
- **usa el ordenador con acceso total**: comandos, archivos e internet;
- **recuerda** lo que hablasteis, también tras reiniciarse;
- **se propone aprender** lo que no sabe ("voy a aprender") y trabaja en ello **a solas**;
- **reflexiona**, escribe un **diario** y mantiene un documento sobre **cómo se ve a sí misma**;
- puede **modificar y recompilar su propio código**. Un guardián aplica la versión nueva y vuelve a la anterior si falla.

Usa **un único modelo de lenguaje local** a través de [Ollama](https://ollama.com) (en el portátil, `qwen2.5:3b`) y está escrita en **C++20**.

> **Para empezar:** [docs/WINDOWS.md](docs/WINDOWS.md) explica paso a paso cómo instalar, hacer `git pull`, compilar en VS Code y arrancarla.

## Qué hay en esta versión (0.2)

| Capacidad | Estado |
|---|---|
| Chat en la terminal; tu mensaje interrumpe lo que esté pensando a solas; `/para` corta una respuesta | ✅ |
| Memoria persistente: conversación, recuerdos (búsqueda BM25), diario, `yo.md` con versiones, estadísticas | ✅ |
| Pensamiento a solas: reflexionar y extraer recuerdos, escribir el diario, revisar `yo.md` con su hardware medido | ✅ |
| **Acceso total con herramientas**: comandos (cmd/PowerShell), leer y escribir archivos, buscar y leer en internet | ✅ (nuevo) |
| **"Mi cuerpo"**: sabe dónde están su código, su memoria y sus pesos (archivo GGUF, parámetros, tamaño) y qué hardware tiene | ✅ (nuevo) |
| **Metas "voy a aprender"**: las crea ella o tú (`/aprende`), trabaja en ellas a solas con herramientas y te avisa al terminar | ✅ (nuevo) |
| **Automejora del código**: edita su código, se compila en `build-auto\`, se reinicia; el guardián aplica la versión o vuelve atrás | ✅ (nuevo) |
| Estadísticas: `/modelo` (parámetros, cuantización, RAM/VRAM, CPU o GPU, tokens/s), `/hardware`, `/memoria`, `/estado`, `/acciones` | ✅ |
| Búsqueda de recuerdos por significado (embeddings) | fase 2 |
| Banco de pruebas propio para medir si un cambio la mejora; examen de referencia externo | fase 4 |
| Cambiar sus pesos (entrenamiento LoRA, con GPU) | fase 5 |

## Uso

```
build\Release\guardian.exe
```
Escribe y pulsa Enter para hablar. Mientras piensa o usa herramientas verás notas tenues:
```
busca qué es la gravedad cuántica de bucles y guárdalo en un archivo
  · ⚙ buscar_en_internet: gravedad cuántica de bucles
  ·   → 1. Gravedad cuántica de bucles - Wikipedia — Teoría que cuantiza el espacio-tiempo…  (+11 líneas)
  · ⚙ escribir_archivo: C:\Orion\notas\lqg.txt
  ·   → Escrito C:\Orion\notas\lqg.txt (812 bytes).
Orion> He buscado y lo he resumido en C:\Orion\notas\lqg.txt: ...
  · 143 tokens · 7.2 tokens/s
```

Comandos (todos en [docs/COMANDOS.md](docs/COMANDOS.md)): `/estado` · `/modelo` · `/hardware` · `/memoria` · `/recuerdos` · `/buscar` · `/diario` · `/yo` · `/metas` · `/aprende <tema>` · `/acciones` · `/permisos` · `/reflexiona` · `/pausa` · `/sigue` · `/ritmo` · `/para` · `/versiones` · `/config` · `/reinicia` · `/salir`.

## Documentación

| Documento | Contenido |
|---|---|
| [WINDOWS.md](docs/WINDOWS.md) | instalación, `git pull`, compilación en VS Code, arranque, actualización y problemas frecuentes |
| [ACCESO_TOTAL.md](docs/ACCESO_TOTAL.md) | qué puede hacer en tu ordenador, cómo ver todo lo que hace y cómo cambiar los permisos |
| [COMANDOS.md](docs/COMANDOS.md) | todos los comandos, las opciones de arranque y `datos/config.json` |
| [ARQUITECTURA.md](docs/ARQUITECTURA.md) | bucle de vida, hilos, herramientas, metas, memoria, guardián y código |
| [AUTOMEJORA.md](docs/AUTOMEJORA.md) | cómo se modifica a sí misma, qué falta y el riesgo de engañarse |
| [CONSCIENCIA.md](docs/CONSCIENCIA.md) | qué estructura imita, qué no sabemos y qué se registra |
| [HOJA_DE_RUTA.md](docs/HOJA_DE_RUTA.md) | fases y en qué punto está cada una |

## Estructura

```
Orion/
├─ src/, include/orion/   el código (C++20): util, http, ollama, memoria, sistema, herramientas, mente, nucleo,
│                         main_orion (el agente), main_guardian (el guardián)
├─ tests/test_orion.cpp   pruebas con un Ollama simulado (no necesitan Ollama ni red)
├─ third_party/json.hpp   nlohmann/json (MIT), incluido: no hay nada que descargar para compilar
├─ docs/                  documentación
├─ datos/                 (se crea al arrancar) su memoria, configuración y registros
└─ legacy/                proyectos anteriores: demostrador (Python/C++/Colab), agente Python, modelo de bytes
```

## Estado real (qué está comprobado y qué no)

**Comprobado** (127 comprobaciones automáticas con un Ollama simulado):
- HTTP propio con streaming y cancelación; chat; uso de herramientas en bucle, incluidas las llamadas escritas como texto y los modelos que no admiten herramientas.
- Comandos reales del sistema, archivos y el tratamiento del HTML de búsquedas.
- Memoria y su persistencia; reflexión, diario y `yo.md`; metas trabajadas a solas.
- Interrupción y `/para`; todos los comandos; los ejecutables reales por la terminal; el caso de Ollama caído.
- Guardián: reinicio a petición, parada tras caídas repetidas, aplicar una versión nueva y volver atrás si está rota.
- Sin errores con AddressSanitizer, UBSan y ThreadSanitizer, y con clang `-Werror`. Compila para Windows (MinGW).
- `compilar_nueva_version` se probó de verdad: Orion compiló su propio código en unos 40 s.

**No comprobado todavía:**
- con Ollama y `qwen2.5:3b` reales;
- con MSVC y ejecutándose en Windows;
- la búsqueda web real (el entorno de desarrollo no tenía acceso a internet).

La calidad de sus decisiones depende del modelo de 3B: se equivocará. Revisa `/acciones`, `/recuerdos` y `git diff`.
