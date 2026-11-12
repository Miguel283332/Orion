# Comandos

Lo que escribas sin `/` es conversación. Los comandos se atienden al momento, aunque Orion esté pensando o respondiendo. En la interfaz web también funcionan, salvo `/salir` y `/reinicia`.

## Estado y medidas

| Comando | Qué hace |
|---|---|
| `/ayuda` | lista de comandos |
| `/estado` | qué está haciendo, ritmo, tiempo encendida (esta vez y en total), llamadas, tokens, velocidad, reflexiones, cola |
| `/modelo` | **parámetros** (aproximado y exacto), familia, cuantización, contexto, **memoria que ocupa** (RAM/VRAM), si va en CPU o GPU, tokens/s |
| `/modelo usar <nombre>` | cambiar el modelo base (se guarda) |
| `/hardware` | sistema, CPU e hilos, RAM total y libre, memoria de Orion, disco libre |
| `/limites` | si el hardware la limita (velocidad, RAM, contexto) y qué propone |
| `/config` | configuración actual |

## Memoria

| Comando | Qué hace |
|---|---|
| `/memoria` | recuerdos por tipo, cuántos están **indexados por significado**, turnos, diario, `yo.md`, espacio en disco |
| `/recuerdos [n]` | últimos `n` recuerdos con su id |
| `/recuerda <texto>` · `/olvida <id>` | guardar o borrar un recuerdo |
| `/buscar <texto>` | buscar por palabras **y por significado** en recuerdos y conversaciones |
| `/diario [n]` | últimas `n` entradas del diario |
| `/yo` | su documento sobre sí misma |
| `/reflexiona` | reflexionar ahora sobre la conversación |

## Aprender

| Comando | Qué hace |
|---|---|
| `/metas` | sus metas: plan, paso actual, estado y resumen de lo aprendido |
| `/aprende <tema>` | ponerle una meta; la trabaja a solas paso a paso y avisa con `★` |
| `/habilidades` | lo que ya sabe hacer (scripts verificados), con usos y éxitos |
| `/habilidades borrar <nombre>` | borrar una habilidad |
| `/practica` | su práctica con juez real: nivel (1-5), aciertos, últimos ejercicios |
| `/practica ahora` | practicar ya |

## Automejora

| Comando | Qué hace |
|---|---|
| `/mejorate` | un ciclo ahora: se mide, propone un cambio, lo mide y solo se lo queda si mejora sin empeorar el examen |
| `/mejoras` | historial: cada propuesta, sus mediciones (banco y examen), si se aceptó y por qué, commit; aviso si el examen cambió |
| `/volver <n>` | deshacer la mejora `n` (restaura sus instrucciones y parámetros, con commit) |
| `/versiones` | últimas versiones de su código (git) |

## Dispositivos y web

| Comando | Qué hace |
|---|---|
| `/dispositivos` | este ordenador y los que registraste: si responden, su velocidad, dónde piensa ahora (▶) |
| `/dispositivo añadir <nombre> <url> [modelo]` | registrar otro ordenador tuyo con Ollama (ver [ESCALAR.md](ESCALAR.md)) |
| `/dispositivo usar <nombre\|local>` · `quitar <nombre>` · `medir` · `auto on\|off` | gestionar los dispositivos |
| `/web` · `/web abrir` | dirección de la interfaz web local (con su token), o abrirla en el navegador |

## Control

| Comando | Qué hace |
|---|---|
| `/acciones [n]` | últimas acciones con herramientas: fecha, origen (chat, meta, evaluación), argumentos y resultado |
| `/permisos [total\|lectura\|ninguno]` | qué herramientas puede usar (ver [ACCESO_TOTAL.md](ACCESO_TOTAL.md)) |
| `/pausa` · `/sigue` | dejar o volver a pensar a solas (siempre te sigue respondiendo) |
| `/ritmo [bajo\|medio\|alto]` | cada cuánto piensa a solas si no le hablas: 15 min, 2 min o 20 s. Con trabajo pendiente encadena las tareas, salvo con `bajo` |
| `/para` | cortar la respuesta que está escribiendo |
| `/reinicia` | reiniciarse (el guardián la vuelve a arrancar) |
| `/salir` | apagarse; antes termina lo que le pediste |

## Pedirle cosas

Además de conversar, puedes pedirle que **haga** cosas. Ella decide qué herramientas usar y verás cada paso. Por ejemplo:
- "mira qué hay en mi carpeta de Descargas"
- "busca en internet…"
- "crea un script que… y guárdalo como habilidad"
- "lee tu código y explícame cómo funciona tu memoria"
- "aprende a usar ffmpeg"

## Opciones al arrancar

`orion` (o `guardian`, que pasa las opciones a Orion):

| Opción | Por defecto |
|---|---|
| `--datos DIR` | `datos` |
| `--modelo M` | el de `datos/config.json` (al principio `qwen2.5:3b`) |
| `--ollama URL` | `http://127.0.0.1:11434` (o la variable `OLLAMA_HOST`) |
| `--repo DIR` | la carpeta actual si es el repositorio; si no, se busca hacia arriba desde el ejecutable |
| `--prompts DIR` | `<repo>/prompts` |
| `--web-puerto N` | el de `config.json` (7860); `0` desactiva la web |
| `--web-cualquier-host` | aceptar la web a través de un proxy (Colab); el token sigue siendo obligatorio |
| `--sin-autonomia` | piensa a solas |
| `--sin-color` | colores si es una terminal |
| `--evaluar ARCHIVO` | modo medición: ejecuta las tareas, imprime `RESULTADO_EVALUACION {...}` y termina (lo usa la automejora) |
| `--temperatura X` · `--max-pasos N` · `--num-ctx N` | sustituyen a los de `config.json` |

## `datos/config.json`

Se crea en el primer arranque. Lo puedes editar tú (con Orion apagada) y también ella.

| Clave | Por defecto | Qué es |
|---|---|---|
| `modelo` | `qwen2.5:3b` | el modelo base |
| `ollama` | `http://127.0.0.1:11434` | Ollama de este ordenador |
| `ritmo` | `medio` | cada cuánto piensa a solas |
| `permisos` | `total` | `total`, `lectura` o `ninguno` |
| `num_ctx` | `8192` | tokens de contexto |
| `num_predict` | `1024` | longitud máxima de cada respuesta (si se corta, lo avisa: dile «sigue») |
| `temperatura` | `0.7` | creatividad al conversar (la automejora puede ajustarla) |
| `max_pasos` | `10` | herramientas por respuesta como máximo (la automejora puede ajustarlo) |
| `cmake` | `cmake` | ruta a cmake (para compilarse a sí misma) |
| `practica_cada_min` | `60` | práctica con juez real a solas (0 = nunca) |
| `automejora_cada_h` | `12` | ciclo de automejora a solas (0 = nunca) |
| `web_puerto` | `7860` | interfaz web local (0 = sin web) |
| `verificar_codigo` | `true` | ejecutar el código que aparezca en sus respuestas y hacerla corregir lo que no cuadre |
| `dispositivos` | `[]` | otros ordenadores tuyos: `[{"nombre": "pc", "ollama": "http://IP:11434", "modelo": "qwen2.5:7b"}]` |
| `dispositivo` | `local` | dónde piensa ahora |
| `dispositivo_auto` | `true` | pasarse sola al más rápido |
