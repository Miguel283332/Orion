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
  │    · escribir el diario      │
  │    · revisar yo.md           │
  │    · o descansar             │
  └──────────────────────────────┘
```

- **Tu mensaje interrumpe**: el hilo de terminal pone el mensaje en la cola y activa la señal de cancelación. La generación en curso se corta al instante (Orion cierra la conexión con Ollama y Ollama deja de generar). La tarea interrumpida no deja nada a medias y se reintenta más tarde.
- **Ritmo** (`/ritmo`): cada cuánto piensa a solas si no le hablas. *bajo* 15 min, *medio* 2 min, *alto* 20 s. Si no hay nada nuevo que pensar, **descansa** y no gasta CPU. Si falla (por ejemplo, Ollama caído), espera cada vez más entre intentos.
- **Gracia**: tras hablar contigo espera 20 s antes de ponerse a pensar a solas, para no quitarte el modelo si sigues escribiendo.

## Un solo modelo

Todo lo hace **el mismo modelo**: conversar, reflexionar, escribir el diario y revisar `yo.md`. Hay una sola identidad y un solo conjunto de pesos (lo que se mejorará en la fase 5). En el portátil (i5, 12 GB) es `qwen2.5:3b`; con la RTX bastará `/modelo usar qwen2.5:7b` o mayor, y la memoria se conserva.

El acceso al modelo está aislado en `Ollama` (`src/ollama.cpp`). Para la fase 5 (ajustar sus pesos) se podrá sustituir por llama.cpp integrado sin tocar el resto.

## Memoria (`datos/`)

Archivos de texto legibles, que Orion podrá leer y modificar ella misma:

| Archivo | Tipo de memoria | Contenido |
|---|---|---|
| `conversacion.jsonl` | episódica | cada turno, con fecha |
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

En la fase 4 el guardián también aplicará las versiones nuevas de Orion que ella misma compile, y volverá a la última que funcionaba si la nueva no arranca.

## Código

| Archivo | Qué hace |
|---|---|
| `util` | texto (UTF-8, sin tildes), tiempo, archivos atómicos, consola UTF-8 en Windows |
| `http` | HTTP/1.1 propio sobre sockets (Windows y Linux): cliente con streaming y cancelación, servidor mínimo |
| `ollama` | chat en streaming, `/api/show` (parámetros, cuantización, contexto) y `/api/ps` (RAM/VRAM) |
| `memoria` | los archivos de `datos/`, BM25, diario, `yo.md` y estadísticas |
| `sistema` | hardware (CPU, RAM, disco, memoria del proceso) y ejecución de procesos (git, guardián) |
| `mente` | contexto y prompts; responder, reflexionar, diario, `yo.md` |
| `nucleo` | bucle de vida, hilos, interrupción, comandos |
