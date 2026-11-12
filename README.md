# Orion

Orion es una IA autónoma que **vive en tu ordenador**: está siempre en marcha, **habla contigo por la terminal**, **recuerda** lo que hablasteis (también después de reiniciarse), **reflexiona a solas** cuando no le hablas, escribe un **diario** y mantiene un documento sobre **cómo se ve a sí misma**. Usa **un único modelo de lenguaje local** (a través de [Ollama](https://ollama.com)) y está escrita en **C++** por rendimiento.

El objetivo a largo plazo es que **se mejore a sí misma**: que aprenda lo que no sabe ("voy a aprender") y que modifique **cualquier parte de su propio código** con un historial de versiones y vuelta atrás. Eso llega por fases (ver [Hoja de ruta](docs/HOJA_DE_RUTA.md)); **esta versión es la fase 1**.

| Qué hace ya (fase 1) | Qué llega después |
|---|---|
| Chat en la terminal mientras piensa a solas (tu mensaje la interrumpe) | Fase 2: búsqueda de recuerdos por significado |
| Memoria persistente: conversación, recuerdos, diario, `yo.md` | Fase 3: "voy a aprender", autoestudio con juez real, habilidades |
| Reflexión a solas: extrae recuerdos, escribe el diario, reescribe `yo.md` | Fase 4: automejora de **todo** su código (git + guardián) |
| Mide su hardware y su modelo (`/modelo`, `/hardware`, `/estado`...) | Fase 5 (con GPU): ajuste de sus propios pesos |
| Guardián que la reinicia si se cae o si pide reiniciarse | |

## Instalación en Windows 11 (VS Code + CMake)

1. **Ollama**: instala [Ollama para Windows](https://ollama.com/download) y descarga el modelo (en un portátil con i5 y 12 GB, el de 3B):
   ```bash
   ollama pull qwen2.5:3b
   ```
2. **Compilador de C++**: si ya compilaste antes con MSVC, lo tienes. Si no, instala *Build Tools for Visual Studio 2022* (carga de trabajo "Desarrollo para el escritorio con C++").
3. En **VS Code**, con las extensiones *C/C++* y *CMake Tools*: abre la carpeta del repositorio. Elige el preset **Orion (build/)** y compila (**Release**). O desde una terminal:
   ```bash
   cmake --preset orion
   cmake --build build --config Release
   ctest --test-dir build -C Release --output-on-failure
   ```
4. **Arranca** Orion a través del guardián, desde la carpeta del repositorio (así sus datos quedan en `datos/`):
   ```bash
   build\Release\guardian.exe
   ```
   Con MinGW, o en Linux, los ejecutables están en `build/` directamente (`build/guardian`).

> Recomendación: deja que Orion viva en una carpeta propia (por ejemplo `C:\Orion`) y no en tus documentos personales. En la fase 4 tendrá acceso total a su carpeta y a comandos.

## Uso

Escribe y pulsa Enter para hablar con ella. Mientras está pensando a solas verás notas tenues (`· reflexionando…`). Si le escribes, **deja lo que estaba haciendo y te atiende**.

```
Orion · 12 recuerdos, 84 turnos de conversación, 5 entradas de diario · datos en C:\Orion\datos
Modelo qwen2.5:3b (3.09 mil millones de parámetros, Q4_K_M) · Ollama x.y.z
Hola, ¿qué recuerdas de mí?
Orion> Que te llamas Ana y que estás construyéndome en C++...
  · 54 tokens · 7.8 tokens/s
/modelo
Modelo: qwen2.5:3b
Parámetros: 3.09 mil millones (3085938688)
...
```

Comandos principales (todos en [docs/COMANDOS.md](docs/COMANDOS.md)): `/estado`, `/modelo`, `/hardware`, `/memoria`, `/recuerdos`, `/buscar`, `/diario`, `/yo`, `/reflexiona`, `/pausa`, `/ritmo`, `/para`, `/versiones`, `/reinicia`, `/salir`.

## Cómo está hecha

- [Arquitectura](docs/ARQUITECTURA.md): el bucle de vida, los dos hilos, la memoria y por qué un solo modelo.
- [Automejora](docs/AUTOMEJORA.md): cómo modificará su propio código (fase 4), el guardián y la vuelta atrás.
- [Consciencia](docs/CONSCIENCIA.md): qué imita esta arquitectura, qué no sabemos y qué se registra.
- [Hoja de ruta](docs/HOJA_DE_RUTA.md): fases y criterios para dar cada una por terminada.

```
Orion/
├─ src/, include/orion/   el código (C++20): util, http, ollama, memoria, sistema, mente, nucleo + main_orion, main_guardian
├─ tests/test_orion.cpp   pruebas con un Ollama simulado (no necesitan Ollama ni red)
├─ third_party/json.hpp   nlohmann/json (MIT), incluido: no hay nada que descargar
├─ docs/                  documentación
├─ datos/                 (se crea al arrancar) memoria, diario, yo.md, configuración, registro del guardián
└─ legacy/                proyectos anteriores: demostrador (Python/C++/Colab), agente Python, modelo de bytes
```

## Estado real

- **Probado** con un Ollama *simulado* (88 comprobaciones): HTTP propio con streaming y cancelación, chat, memoria y su persistencia, reflexión, diario, `yo.md`, comandos, interrupción, `/para`, los ejecutables reales por la terminal, Ollama caído, y el guardián (reinicio a petición y parada tras caídas repetidas). Sin errores con AddressSanitizer, UBSan y ThreadSanitizer, y con clang `-Werror`. Compila para Windows con MinGW.
- **No probado todavía**: con Ollama real, con MSVC y ejecutándose en Windows. La calidad de las reflexiones depende del modelo de 3B: revisa `/recuerdos` y usa `/olvida` si guarda algo mal.
