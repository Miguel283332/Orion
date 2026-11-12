# Comandos

Todo lo que escribas sin `/` es conversación. Los comandos se atienden al momento, aunque Orion esté pensando o respondiendo.

| Comando | Qué hace |
|---|---|
| `/ayuda` | lista de comandos |
| `/estado` | qué está haciendo, ritmo, tiempo encendida (esta vez y en total), arranques, llamadas al modelo, tokens generados, última velocidad, reflexiones, mensajes en cola |
| `/modelo` | modelo en uso: **parámetros** (aproximado y exacto), familia, cuantización, contexto en uso y máximo, **memoria que ocupa** (RAM y VRAM), si se ejecuta en CPU o GPU, tokens/s medidos |
| `/modelo usar <nombre>` | cambiar de modelo (se aplica al terminar lo que esté haciendo y se guarda en `config.json`) |
| `/hardware` | sistema operativo, CPU e hilos, RAM total y libre, memoria de Orion (sin el modelo), disco libre |
| `/memoria` | número de recuerdos por tipo, turnos de conversación, entradas de diario, tamaño de `yo.md` y espacio en disco de `datos/` |
| `/recuerdos [n]` | últimos `n` recuerdos (15 por defecto) con su id |
| `/recuerda <texto>` | guardar un recuerdo a mano |
| `/olvida <id>` | borrar un recuerdo (por ejemplo `/olvida r12`) |
| `/buscar <texto>` | buscar en recuerdos y conversaciones |
| `/diario [n]` | últimas `n` entradas del diario (1 por defecto) |
| `/yo` | su documento sobre sí misma (`yo.md`) |
| `/reflexiona` | reflexionar ahora sobre la conversación reciente |
| `/pausa` | dejar de pensar a solas (sigue respondiéndote) |
| `/sigue` | volver a pensar a solas |
| `/ritmo [bajo\|medio\|alto]` | cada cuánto piensa a solas: 15 min, 2 min o 20 s (se guarda) |
| `/para` | cortar la respuesta que está escribiendo |
| `/versiones` | últimas versiones de su código (git), si arrancó dentro del repositorio |
| `/config` | configuración actual |
| `/reinicia` | reiniciarse (el guardián la vuelve a arrancar) |
| `/salir` | apagarse; antes termina de responder lo que le hayas pedido |

## Opciones al arrancar

`orion` (o `guardian`, que pasa las opciones a Orion):

| Opción | Por defecto |
|---|---|
| `--datos DIR` | `datos` |
| `--modelo M` | el de `datos/config.json` (al principio `qwen2.5:3b`) |
| `--ollama URL` | `http://127.0.0.1:11434` (o la variable `OLLAMA_HOST`) |
| `--repo DIR` | la carpeta actual si tiene `.git` |
| `--sin-autonomia` | piensa a solas |
| `--sin-color` | colores si es una terminal |
