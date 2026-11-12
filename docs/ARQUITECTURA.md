# Arquitectura

## Idea general

Una persona no funciona "mensaje → respuesta": recibe estímulos sin parar y piensa también cuando nadie le habla. Orion imita esa estructura con un **bucle de vida** y dos hilos:

```
            tú (terminal)
                 │  mensajes y /comandos
                 ▼
  ┌──────────────────────────────┐    los /comandos se atienden al momento
  │ HILO DE TERMINAL             │──► (estadísticas, memoria, pausa...)
  └──────────────┬───────────────┘
                 │ cola de mensajes  +  señal "deja lo que estás haciendo"
                 ▼
  ┌──────────────────────────────┐
  │ HILO DE MENTE (bucle de vida)│
  │ 1. ¿hay mensajes? → responder│◄──── un único modelo (Ollama, streaming, cancelable)
  │ 2. si no, según el ritmo:    │
  │    · reflexionar             │────► datos/  (todo se guarda al momento)
  │    · trabajar en una meta    │◄───► herramientas: comandos, archivos, internet,
  │    · escribir el diario      │      memoria, compilarse, reiniciarse
  │    · revisar yo.md           │
  │    · o descansar             │
  └──────────────────────────────┘
```

- **Tu mensaje interrumpe**: el hilo de terminal pone el mensaje en la cola y activa la señal de cancelación. La generación en curso se corta al instante (Orion cierra la conexión con Ollama y Ollama deja de generar). La tarea interrumpida no deja nada a medias y se reintenta más tarde.
- **Ritmo** (`/ritmo`): cada cuánto piensa a solas si no le hablas. *bajo* 15 min, *medio* 2 min, *alto* 20 s. Si no hay nada nuevo que pensar, **descansa** y no gasta CPU. Si falla (por ejemplo, Ollama caído), espera cada vez más entre intentos.
- **Gracia**: tras hablar contigo espera 20 s antes de ponerse a pensar a solas, para no quitarte el modelo si sigues escribiendo.

## Herramientas (acceso total)

Al responder, y al trabajar en una meta, el modelo puede pedir **herramientas** con el formato nativo de Ollama (`tools` / `tool_calls`). Orion las ejecuta, devuelve el resultado al modelo como mensaje `tool` y repite hasta que el modelo responde sin pedir más, con un máximo de `max_pasos`. Detalles:
- Si el modelo escribe la llamada como texto (`<tool_call>{...}</tool_call>`, algo habitual en modelos pequeños), también se entiende.
- Si el modelo no admite herramientas, responde sin ellas.
- Cada uso se muestra en la terminal y se anota en `datos/acciones.jsonl`.

Lista y permisos en [ACCESO_TOTAL.md](ACCESO_TOTAL.md).

## "Mi cuerpo"

En cada conversación el contexto incluye una sección que le describe su propio cuerpo:
- dónde está su **código fuente** y qué hace cada archivo, y cómo modificarse (editar, `compilar_nueva_version`, `reiniciarme`, `git commit`);
- su **memoria**;
- sus **pesos**: la ruta del archivo GGUF que usa Ollama (sacada de `/api/show`), su tamaño, sus parámetros y su cuantización;
- su **hardware** y sus **permisos**.

El código se localiza así: la carpeta actual, si es el repositorio; si no, se busca hacia arriba desde el ejecutable (`build\Release\` → repositorio); o con `--repo`.

## Metas: "no lo sé, voy a aprender"

Si le pides algo que no sabe hacer, se le indica que lo diga y que cree una meta (`crear_meta_de_aprendizaje`). También puedes crearla tú con `/aprende`. Cuando no le hablas, trabaja en la meta más antigua:
1. usa sus herramientas (busca y lee en internet; comprueba con código o comandos si se puede);
2. guarda lo importante como recuerdos;
3. termina con un resumen.

El resumen queda en la meta (`/metas`), en un recuerdo de tipo `aprendido` y en la conversación. Te avisa con `★ He terminado de aprender sobre…`. Si no termina en 3 intentos, la meta se marca como fallida. Tu mensaje interrumpe el trabajo, que se retoma después.

## Un solo modelo

Todo lo hace **el mismo modelo**: conversar, reflexionar, escribir el diario y revisar `yo.md`. Hay una sola identidad y un solo conjunto de pesos (lo que se mejorará en la fase 5). En el portátil (i5, 12 GB) es `qwen2.5:3b`; con la RTX bastará `/modelo usar qwen2.5:7b` o mayor, y la memoria se conserva.

El acceso al modelo está aislado en `Ollama` (`src/ollama.cpp`). Para la fase 5 (ajustar sus pesos) se podrá sustituir por llama.cpp integrado sin tocar el resto.

## Memoria (`datos/`)

Archivos de texto legibles, que Orion podrá leer y modificar ella misma:

| Archivo | Tipo de memoria | Contenido |
|---|---|---|
| `conversacion.jsonl` | episódica | cada turno, con fecha |
| `metas.json` | metas | lo que quiere aprender, su estado y lo aprendido |
| `acciones.jsonl` | registro | cada uso de una herramienta (fecha, origen, argumentos, resultado) |
| `actualizacion/` | automejora | la versión nueva compilada, esperando a que el guardián la aplique |
| `recuerdos.jsonl` | semántica | hechos duraderos (`usuario`, `proyecto`, `preferencia`, `yo`, `mundo`, `manual`) con id `rN` |
| `diario.md` | autobiográfica | entradas en primera persona |
| `yo.md` | modelo de sí misma | quién es, qué sabe hacer, sus límites (con su hardware medido), qué quiere aprender, cambios |
| `yo_versiones/` | historial | cada versión anterior de `yo.md` |
| `estadisticas.json` | contadores | llamadas, tokens, velocidad, tiempo encendida, arranques, marcas de reflexión |
| `config.json` | configuración | modelo, dirección de Ollama, ritmo, contexto, temperatura |
| `guardian.log` | registro | arranques, reinicios y caídas |

**Por qué archivos y no SQLite:** no hay dependencias que instalar en Windows, se pueden abrir con cualquier editor, Orion puede leerlos y modificarlos directamente, y para decenas de miles de recuerdos la búsqueda en memoria es instantánea. Si algún día hace falta, se cambia solo `src/memoria.cpp`.

**Búsqueda:** por palabras (BM25, sin tildes ni mayúsculas). En la fase 2 se añade búsqueda por significado (embeddings del mismo modelo).

**Cómo se usa al hablar:** en cada respuesta, el contexto incluye `yo.md`, una línea con su estado y hardware medidos, los 6 recuerdos más relevantes para tu mensaje y la conversación reciente.

## Reflexión a solas

1. **Reflexionar** (tras unos 6 turnos nuevos, o 2 si llevas 5 min sin hablar): lee la conversación reciente y extrae hechos duraderos en JSON. No guarda duplicados (más del 80 % de palabras en común).
2. **Diario** (tras 6 turnos ya reflexionados, o 3 recuerdos nuevos): escribe una entrada sincera de lo que ha pasado.
3. **yo.md** (tras cada entrada nueva del diario): reescribe su documento con lo vivido y con **hechos medidos** (CPU, RAM, modelo, parámetros, si se ejecuta en CPU o GPU, tokens/s). Así "sabe" que está limitada por el hardware. Si la versión nueva no es válida, se conserva la anterior.

## Guardián

`guardian` arranca `orion` en la misma consola y:
- si sale con código **0** (`/salir`), termina;
- si sale con **75** (`/reinicia`), la vuelve a arrancar;
- si **se cae**, la vuelve a arrancar a los 3 s; si se cae 3 veces en 2 minutos, se detiene y lo anota en `datos/guardian.log`.

Además aplica las **versiones nuevas** que Orion compila de sí misma:
1. si encuentra `datos/actualizacion/orion.exe`, guarda la actual como `orion.anterior.exe` y pone la nueva en su lugar;
2. si la nueva se cae en su primer minuto, **vuelve a la anterior**;
3. Orion lo dice al arrancar y lo añade a su conversación, para saber que su cambio falló.

## Código

| Archivo | Qué hace |
|---|---|
| `util` | texto (UTF-8, sin tildes), tiempo, archivos atómicos, consola UTF-8 en Windows |
| `http` | HTTP/1.1 propio sobre sockets (Windows y Linux): cliente con streaming y cancelación, servidor mínimo |
| `ollama` | chat en streaming, `/api/show` (parámetros, cuantización, contexto) y `/api/ps` (RAM/VRAM) |
| `memoria` | los archivos de `datos/`, BM25, diario, `yo.md` y estadísticas |
| `sistema` | hardware (CPU, RAM, disco, memoria del proceso) y ejecución de procesos (git, guardián) |
| `herramientas` | comandos, archivos, internet (curl), memoria, metas, compilarse y reiniciarse; registro de acciones |
| `mente` | contexto ("Mi cuerpo", recuerdos, estado); bucle de herramientas; responder, metas, reflexionar, diario, `yo.md` |
| `nucleo` | bucle de vida, hilos, interrupción, comandos |
