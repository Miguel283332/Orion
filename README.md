# Orion

Orion es una IA autónoma que **vive en tu ordenador**. Está siempre en marcha y piensa también cuando no le hablas. Usa **un único modelo de lenguaje local** a través de [Ollama](https://ollama.com) (en el portátil, `qwen2.5:3b`) y está escrita en **C++20**, sin dependencias que descargar.

Lo que hace:
- **Habla contigo** por la terminal y por una **interfaz web local** con paneles.
- **Usa el ordenador con acceso total**: comandos, archivos e internet.
- **Recuerda por significado**, no solo por palabras, y resume las conversaciones antiguas para no olvidarlas.
- **Aprende**:
  - se pone metas ("voy a aprender") y las trabaja a solas, paso a paso;
  - guarda como **habilidades** los scripts que le funcionan;
  - **practica con un juez real**: predice lo que hace un script y lo comprueba ejecutándolo.
- **Se mejora a sí misma con medida**: propone cambios en sus instrucciones, los mide con un banco de pruebas y un examen de referencia, y solo se queda con los que la mejoran. También puede modificar y recompilar su código C++; el guardián vuelve a la versión anterior si algo falla.
- **Escala**: si registras otro ordenador tuyo con Ollama (la RTX), se pasa sola a él cuando es más rápido y vuelve cuando se apaga. Te avisa cuando el hardware la limita.

> **Para empezar:** [docs/WINDOWS.md](docs/WINDOWS.md) explica paso a paso cómo hacer `git pull`, compilar en VS Code y arrancarla.

## Versión 0.3: qué hay

| Capacidad | Estado |
|---|---|
| Chat en terminal y web; tu mensaje interrumpe lo que piensa a solas; `/para` | ✅ |
| Memoria persistente: conversación, recuerdos, diario, `yo.md` con versiones | ✅ |
| **Memoria por significado** (vectores del mismo modelo) + palabras, y **resúmenes** de conversaciones antiguas | ✅ nuevo |
| Acceso total: comandos, archivos, internet, su código, su memoria; "Mi cuerpo" (código, memoria, pesos, hardware) | ✅ |
| **Metas por pasos**: planifica, hace un paso cada vez, se puede interrumpir y retomar, avisa con `★` | ✅ mejorado |
| **Habilidades**: scripts verificados que guarda y reutiliza (`/habilidades`) | ✅ nuevo |
| **Práctica con juez real**: ajusta la dificultad para acertar ~la mitad; los ejemplos verificados quedan para entrenar en la fase 5 | ✅ nuevo |
| **Automejora medida**: banco de pruebas + examen de referencia (con huella), acepta solo lo que mejora, commit en git, `/mejoras`, `/volver` | ✅ nuevo |
| Automejora del código C++: compilarse y reiniciarse; el guardián aplica la versión o vuelve atrás | ✅ |
| **Interfaz web local** (`/web`): chat, actividad en directo, estado, metas, mejoras, acciones y recuerdos. La puede modificar ella | ✅ nuevo |
| **Escalado**: dispositivos registrados, cambio automático al más rápido, vuelta al local si cae, avisos de límites | ✅ nuevo |
| Cambiar sus pesos (entrenamiento LoRA con GPU) | fase 5, pendiente |

## Uso

```
build\Release\guardian.exe
```
Escribe y pulsa Enter, o abre la interfaz web con `/web abrir`. Ejemplos:
- "mira qué hay en mi carpeta de Descargas"
- "busca qué es la gravedad cuántica de bucles y guárdame un resumen en un archivo"
- `/aprende cómo funciona git por dentro`
- `/mejorate` (se mide, propone un cambio, lo mide y decide)

Comandos ([lista completa](docs/COMANDOS.md)):
- **Estado y medidas:** `/estado` · `/modelo` · `/hardware` · `/limites` · `/config`
- **Memoria:** `/memoria` · `/recuerdos` · `/buscar` · `/diario` · `/yo`
- **Aprendizaje:** `/metas` · `/aprende` · `/habilidades` · `/practica`
- **Automejora:** `/mejoras` · `/mejorate` · `/volver`
- **Dispositivos y web:** `/dispositivos` · `/web`
- **Control:** `/acciones` · `/permisos` · `/reflexiona` · `/pausa` · `/sigue` · `/ritmo` · `/para` · `/versiones` · `/reinicia` · `/salir`

## Documentación

| Documento | Contenido |
|---|---|
| [WINDOWS.md](docs/WINDOWS.md) | instalación, `git pull`, compilación en VS Code, arranque, actualización y problemas frecuentes |
| [ACCESO_TOTAL.md](docs/ACCESO_TOTAL.md) | qué puede hacer en tu ordenador, qué hace a solas, cómo ver todo y cómo cambiar los permisos |
| [APRENDER.md](docs/APRENDER.md) | memoria por significado, metas por pasos, habilidades y práctica con juez real |
| [AUTOMEJORA.md](docs/AUTOMEJORA.md) | el ciclo medido, el banco y el examen, qué puede cambiar y qué no, y sus límites |
| [ESCALAR.md](docs/ESCALAR.md) | usar tu PC con RTX (u otro) desde el portátil, y los avisos de límites |
| [COMANDOS.md](docs/COMANDOS.md) | todos los comandos, opciones de arranque y `datos/config.json` |
| [ARQUITECTURA.md](docs/ARQUITECTURA.md) | bucle de vida, prioridades a solas, memoria, herramientas, web, guardián y código |
| [CONSCIENCIA.md](docs/CONSCIENCIA.md) | qué estructura imita, qué no sabemos y qué se registra |
| [HOJA_DE_RUTA.md](docs/HOJA_DE_RUTA.md) | fases y en qué punto está cada una |

## Estructura

```
Orion/
├─ src/, include/orion/   C++20: util, http, ollama, memoria, sistema, herramientas, habilidades, mente, evaluacion,
│                         automejora, nucleo; main_orion (el agente) y main_guardian (el guardián)
├─ prompts/sistema.md     sus instrucciones de comportamiento (las mejora ella, midiendo)
├─ evaluacion/            banco.json (su banco de pruebas) y examen_referencia.json
├─ interfaz/index.html    su interfaz web (la puede modificar ella)
├─ tests/test_orion.cpp   pruebas con un Ollama simulado (no necesitan Ollama ni red)
├─ third_party/json.hpp   nlohmann/json (MIT), incluido
├─ docs/                  documentación
├─ datos/                 (se crea al arrancar) su memoria, configuración y registros
└─ legacy/                proyectos anteriores
```

## Estado real (qué está comprobado y qué no)

**Comprobado** con **202 comprobaciones** automáticas y un Ollama simulado:
- memoria por significado (encuentra "física" al preguntar por "ciencia") y resúmenes;
- habilidades que solo se guardan si funcionan; práctica que ejecuta, compara, ajusta la dificultad y saca lecciones;
- metas por pasos;
- un **ciclo de automejora completo**, con procesos y git reales: mide (0/2 → 1/2), acepta; en el segundo ciclo rechaza un cambio que empeora el examen; `/volver` lo deshace; avisa si el examen cambia;
- la interfaz web (token, protección del `Host`, chat, comandos, sondeo continuo);
- cambio automático al dispositivo más rápido y vuelta al local si cae; avisos de límites;
- lo anterior: herramientas, guardián y versiones, interrupción, ejecutables reales.

Sin errores con AddressSanitizer, UBSan y ThreadSanitizer, ni con clang `-Werror`. Compila para Windows (MinGW). La página web se probó en Chromium (claro, oscuro y móvil) sin errores de JavaScript.

**No comprobado todavía:**
- con Ollama y `qwen2.5:3b` reales: calidad de sus decisiones, si el modelo da vectores de significado (si no, busca por palabras) y velocidad real de las evaluaciones;
- con MSVC en Windows;
- la búsqueda web real.
