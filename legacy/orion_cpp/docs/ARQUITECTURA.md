# Arquitectura

## Idea general

Una persona no funciona "mensaje → respuesta": recibe estímulos sin parar y piensa también cuando nadie le habla. Orion imita esa estructura con un **bucle de vida**:

```
     tú (terminal)        tú (navegador: interfaz web local, 127.0.0.1 + token)
          │                      │
          ▼                      ▼
  ┌──────────────────┐   ┌──────────────────┐     los /comandos se atienden al momento
  │ HILO DE TERMINAL │   │ SERVIDOR WEB     │──►  (un comando cada vez, venga de donde venga)
  └────────┬─────────┘   └────────┬─────────┘
           └──────────┬───────────┘
                      │ cola de mensajes  +  señal "deja lo que estás haciendo"
                      ▼
  ┌──────────────────────────────────────────┐
  │ HILO DE MENTE (bucle de vida)            │◄── un único modelo (Ollama local o el de otro
  │ 1. ¿hay mensajes? → responder            │    ordenador tuyo, si es más rápido)
  │ 2. cada 10 min: dispositivos y límites   │
  │ 3. si no le hablas, por prioridad:       │──► datos/  (todo se guarda al momento)
  │    reflexionar · trabajar en una meta    │
  │    (un paso) · indexar por significado · │◄─► herramientas: comandos, archivos, internet,
  │    resumir lo antiguo · diario · yo.md · │    memoria, habilidades, compilarse, reiniciarse
  │    automejora medida · practicar ·       │
  │    descansar                             │──► orion --evaluar (proceso aparte, para medirse)
  └──────────────────────────────────────────┘
```

- **Tu mensaje interrumpe**: el hilo de terminal pone el mensaje en la cola y activa la señal de cancelación. La generación en curso se corta al instante (Orion cierra la conexión con Ollama y Ollama deja de generar). La tarea interrumpida no deja nada a medias y se reintenta más tarde.
- **Ritmo** (`/ritmo`): cada cuánto piensa a solas si no le hablas. *bajo* 15 min, *medio* 2 min, *alto* 20 s. Si hay trabajo pendiente (por ejemplo, el siguiente paso de una meta), lo encadena sin esperar, salvo con *bajo*. Si no hay nada nuevo que pensar, **descansa** y no gasta CPU. Si falla (por ejemplo, Ollama caído), espera cada vez más entre intentos.
- **Prioridades a solas** (de mayor a menor):
  1. reflexionar si hay conversación nueva;
  2. el siguiente paso de una meta;
  3. indexar recuerdos por significado;
  4. resumir conversación antigua;
  5. diario;
  6. `yo.md`;
  7. automejora medida (cada 12 h, tras 30 min sin hablar);
  8. práctica (cada 60 min).
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
| `recuerdos.jsonl` | semántica | hechos duraderos (`usuario`, `proyecto`, `preferencia`, `yo`, `mundo`, `manual`, `aprendido`, `leccion`, `resumen`) con id `rN` |
| `vectores.bin` | semántica | el vector de significado de cada recuerdo (con el modelo que lo generó) |
| `habilidades/` | procedimental | scripts verificados (`nombre.json` + el script) |
| `practica.jsonl` | práctica | cada ejercicio: script, predicción, salida real y acierto (datos para la fase 5) |
| `mejoras.jsonl` | automejora | cada propuesta, sus mediciones y si se aceptó |
| `automejora/` | automejora | caché de mediciones, la propuesta en prueba y la carpeta de trabajo de las evaluaciones |
| `examen_huella.txt` | automejora | huella SHA-256 del examen de referencia |
| `web_token.txt` | interfaz | token de la interfaz web |
| `diario.md` | autobiográfica | entradas en primera persona |
| `yo.md` | modelo de sí misma | quién es, qué sabe hacer, sus límites (con su hardware medido), qué quiere aprender, cambios |
| `yo_versiones/` | historial | cada versión anterior de `yo.md` |
| `estadisticas.json` | contadores | llamadas, tokens, velocidad, tiempo encendida, arranques, marcas de reflexión |
| `config.json` | configuración | modelo, dirección de Ollama, ritmo, contexto, temperatura |
| `guardian.log` | registro | arranques, reinicios y caídas |

**Por qué archivos y no SQLite:** no hay dependencias que instalar en Windows, se pueden abrir con cualquier editor, Orion puede leerlos y modificarlos directamente, y para decenas de miles de recuerdos la búsqueda en memoria es instantánea. Si algún día hace falta, se cambia solo `src/memoria.cpp`.

**Búsqueda:** híbrida. Por palabras (BM25, sin tildes, mayúsculas ni palabras vacías) y por **significado** (coseno con vectores del mismo modelo, normalizado en el rango observado). Detalles en [APRENDER.md](APRENDER.md).

**Cómo se usa al hablar:** en cada respuesta, el contexto incluye:
- sus instrucciones (`prompts/sistema.md`) y "Mi cuerpo";
- `yo.md` y una línea con su estado y hardware medidos;
- las habilidades relevantes y los 6 recuerdos más relevantes para tu mensaje;
- la conversación reciente.

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
| `http` | HTTP/1.1 propio sobre sockets (Windows y Linux): cliente con streaming y cancelación; servidor con cabeceras que recoge sus hilos (interfaz web y pruebas) |
| `ollama` | chat en streaming con herramientas, vectores (`/api/embed`), `/api/show` (parámetros, cuantización, contexto, ruta de los pesos) y `/api/ps` (RAM/VRAM); cambiar de dirección o modelo sin carreras entre hilos |
| `memoria` | los archivos de `datos/`, BM25, diario, `yo.md` y estadísticas |
| `sistema` | hardware (CPU, RAM, disco, memoria del proceso) y ejecución de procesos (git, guardián) |
| `herramientas` | comandos, archivos, internet (curl), memoria, metas, compilarse y reiniciarse; registro de acciones |
| `habilidades` | biblioteca de scripts verificados (guardar con prueba, usar, relevantes) |
| `mente` | contexto (instrucciones, "Mi cuerpo", habilidades, recuerdos, estado); bucle de herramientas; responder, metas por pasos, práctica, vectores, resúmenes, reflexión, diario, `yo.md` |
| `evaluacion` | tareas con comprobación objetiva; `orion --evaluar`; medir en un proceso aparte |
| `automejora` | ciclo medido (banco, propuesta, examen, aceptar o rechazar, git), `/volver`, huella del examen |
| `nucleo` | bucle de vida, hilos, interrupción, comandos, interfaz web, dispositivos y límites |

## Interfaz web

Al arrancar abre `http://127.0.0.1:7860/?t=TOKEN` (`/web`, `/web abrir`). Tiene:
- **chat** con todo lo que sale por la terminal, en directo;
- paneles de **estado**, **metas**, **automejora**, **últimas acciones** y **recuerdos**;
- botones para pausa, reflexionar, mejorarse, practicar y parar.

La página es `interfaz/index.html` del repositorio y se lee del disco en cada visita, así que **Orion puede modificar su propia interfaz**.

Seguridad, porque tiene acceso total:
- solo escucha en `127.0.0.1`;
- la API exige el token en la cabecera `X-Orion-Token` (una web ajena no puede enviarla sin pasar el control del navegador);
- rechaza cualquier `Host` que no sea `127.0.0.1` o `localhost` (protección contra *DNS rebinding*);
- desde la web no se puede apagar ni reiniciar.

## Dispositivos

Ver [ESCALAR.md](ESCALAR.md). La dirección y el modelo de Ollama se pueden cambiar en caliente: lo que se esté generando termina con la conexión anterior.
