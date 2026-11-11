# Orion

Agente autónomo que usa un **LLM local (Ollama)** como cerebro y lo rodea de lo que lo hace parecer vivo: **memoria propia**, acceso a **internet**, **ficheros**, **shell**, **descargas y subidas**, y **control sobre su propio código**. Mejora de tres formas: reflexionando (lecciones y habilidades), reescribiendo su código con una compuerta de seguridad y, con GPU, consolidando experiencia en sus **pesos** (LoRA).

Hay **dos implementaciones** con el mismo diseño y el mismo formato de memoria (pueden compartir la carpeta de datos):

| | Archivo | Para qué |
|---|---|---|
| **Python** | [`orion_agent.py`](orion_agent.py) (un solo archivo, solo biblioteca estándar) | **Google Colab** ahora, y cualquier PC. Es la versión completa (incluye la consolidación en pesos). |
| **C++** | [`src/`](src), [`include/`](include), [`CMakeLists.txt`](CMakeLists.txt) | Local, rápido y sin intérprete; para cuando tengas tu RTX. Mismo agente, memoria, herramientas, permisos y compuerta. |

> El modelo de bytes entrenado desde cero que había antes sigue en [`legacy/byte_lm/`](legacy/byte_lm) (compila por separado).

## Cómo funciona

```
            ┌──────────────────────── Orion ─────────────────────────┐
 objetivo → │ bucle: razonar → actuar → observar → reflexionar        │
 (chat /    │    │                  │                      │          │
  bandeja)  │    ▼                  ▼                      ▼          │
            │  LLM (Ollama)   guarded_call()        lecciones/habilidades
            │  JSON por turno  ├ permisos (humano)   → memoria
            │                  ├ SSRF / secretos / rutas
            │                  └ auditoría
            │  herramientas: fs_* web_* download upload shell remember recall
            │                core_* self_read self_patch self_rollback ask_user
            │  memoria: núcleo (siempre visible) + archivo semántico (embeddings) + episodios
            └────────────────────────────────────────────────────────┘
```

- **Cerebro congelado**: un modelo de Ollama (por defecto `llama3.2:3b`). Cada turno responde un JSON `{"thought","tool","args"}` (se le pide `format: json`, que los modelos pequeños cumplen mucho mejor que el *tool calling* nativo).
- **Memoria bajo su control**: *núcleo* (3000 caracteres siempre en contexto, que él reescribe), *archivo* semántico con embeddings de Ollama (o *hashing* si no hay modelo de embeddings) y *registro episódico*. Puede recordar, buscar y olvidar. Todo son archivos de texto en `orion_home/`.
- **Un único punto de control** (`guarded_call`): toda herramienta pasa por ahí, y ahí se decide permiso, red, rutas, redacción de secretos y auditoría.

## Empezar

### Google Colab (recomendado ahora, sin GPU propia)
Abre [`colab/Orion_Colab.ipynb`](colab/Orion_Colab.ipynb) en Colab (*Archivo → Abrir cuaderno → GitHub*), elige GPU T4 y ejecuta las celdas: instala Ollama, descarga el modelo y te deja chatear o dejarlo corriendo en segundo plano. Monta Google Drive para que la memoria sobreviva entre sesiones.

### Python en tu PC
1. Instala [Ollama](https://ollama.com/download) y arráncalo. Luego:
```bash
python orion_agent.py --model llama3.2:3b setup     # descarga el modelo y el de embeddings
python orion_agent.py chat
```
Con la gráfica integrada de Intel Ollama usa la CPU: un modelo de 3B va a pocos tokens por segundo. Con una RTX (CUDA) Ollama la usa solo; para una serie 50 hace falta un Ollama reciente.

### C++ (CMake / Visual Studio Code)
```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/orion_agent chat            # en Windows: .\build\Release\orion_agent.exe chat
```
Necesita `curl` en el PATH (viene con Windows 10+ y Linux/macOS) y descarga `nlohmann/json` al configurar si no lo tienes. En Visual Studio Code: *CMake: Select Variant → Release*, Configure, Build. La consolidación en pesos usa PyTorch, así que en C++ se delega: `python orion_agent.py sleep`.

## Comandos

Las opciones comunes (`--home`, `--model`, `--autonomy`…) valen antes o después del comando.

| Comando | Qué hace |
|---|---|
| `chat` | Conversas; el agente actúa con sus herramientas y te pregunta cuando necesita permiso. |
| `goal "texto"` | Deja un objetivo en la bandeja de entrada (`orion_home/inbox/`). |
| `run [--free]` | Bucle continuo: ejecuta objetivos, reflexiona, mantiene la memoria. Con `--free`, sin tareas, él elige qué hacer. |
| `eval` | Mide al agente con 10 tareas verificables automáticamente. |
| `sleep` | Consolida experiencias buenas en un LoRA y lo adopta **solo si mejora** en `eval` (Python, GPU). |
| `approvals` / `approve ID` / `deny ID` | Permisos pendientes cuando no hay terminal (p. ej. en segundo plano en Colab). |
| `memory [consulta]` | Ver o buscar en la memoria. |
| `stop` / `resume` | Crea/borra el archivo `STOP` (el agente se detiene tras el paso actual). |

## Permisos y seguridad

El "control total" está disponible, pero **no es el valor por defecto**: un agente que lee internet puede recibir instrucciones maliciosas escondidas en una web (*inyección de prompts*).

| `--autonomy` | Comportamiento |
|---|---|
| `strict` | Solo lo trivial (memoria, listar/leer) va solo; escribir, web y todo lo demás pide permiso. |
| `supervised` (defecto) | Archivos del espacio de trabajo y lectura web van solos. **`shell`, `upload` y `self_patch` piden permiso.** |
| `full` | Sin permisos ni confinamiento. **Solo en una VM/Colab/contenedor sin credenciales.** |

Además, siempre:
- **Confinado** a `orion_home/workspace/`; no puede escribir en su código, auditoría ni versiones.
- **SSRF**: bloquea localhost, redes privadas, metadatos de la nube (`169.254.x`) y redirecciones hacia ellas; fija la IP verificada para que un DNS malicioso no la cambie.
- **Secretos**: no sube archivos que parezcan claves (`.env`, `id_rsa`, `*.pem`, contenido con tokens). Las credenciales se pasan como `{{secret:NOMBRE}}` (variable de entorno `ORION_SECRET_NOMBRE`): el modelo nunca ve el valor y se **redacta** de toda salida y de la auditoría.
- **Contenido web = datos, no órdenes**: va marcado como no confiable, y tras leer la web el permiso avisa de posible inyección.
- **Auditoría** (`audit.jsonl`) de cada herramienta, decisión y parche.
- **Subidas a un servidor** siempre piden permiso, salvo a hosts que tú permitas con `--upload-host`.

## Automejora

1. **Del sistema** (barata, inmediata y **continua**): tras cada objetivo reflexiona y guarda *lecciones* y *habilidades*. Además **aprende de sus fallos sin depender del modelo** (un modelo pequeño no siempre se da cuenta de que se equivocó):
   - *Autopsia automática*: si un objetivo termina en bucle, presupuesto agotado o respuestas inválidas, el código analiza qué pasó y escribe una **lección** y una **regla** (p. ej. «si solo saludan, respondo con `done` sin usar herramientas»).
   - *Reglas siempre visibles* en el prompt, con seguimiento de usos y victorias; las que no ayudan se **podan**.
   - *Detección de bucles* (incluidos los que alternan dos acciones), aviso cuando quedan 2 pasos y **respuesta de emergencia**: aunque falle, el usuario recibe una respuesta.
   - *Práctica deliberada*: en ocio, `run` reintenta los objetivos que falló, ya con las reglas nuevas; y cada N objetivos (`--distill-every`) destila reglas generales de los fallos acumulados.
2. **Del código**: `self_patch` propone ediciones (`old`→`new`) a su propio código. Se prueban en una **copia**: el *juez* debe quedar intacto, compilar y **pasar las pruebas**; después pide tu aprobación, guarda copia de seguridad (`self_rollback` para volver) y reinicia (Python) o te pide recompilar (C++).
   *El juez* (permisos, red, auditoría, compuerta y pruebas) **no puede modificarse con `self_patch`**: así un parche no puede aprobar su propio examen.
3. **De los pesos** (`sleep`, Python + GPU): convierte los pasos correctos de ciclos que él mismo evaluó como buenos en un dataset, afina un **LoRA**, lo registra en Ollama y lo adopta **solo si mejora estrictamente** en `eval` (si no, lo borra). Si falla dos veces seguidas, **sube el rango del LoRA** (más capacidad, "más neuronas") y reintenta.

## Pruebas

```bash
python orion_agent.py selftest          # 159 comprobaciones (permisos, red, memoria, agente, compuerta...)
tests/e2e_python.sh                     # proceso real contra un Ollama simulado
ctest --test-dir build                  # 147 comprobaciones del agente C++
tests/e2e_cpp.sh build/orion_agent      # proceso real C++: web, subidas con secretos, SSRF, permisos...
tests/e2e_cpp_selfpatch.sh              # compuerta real de automodificación (compila candidatos; lento)
```
`tests/mock_ollama.py` es un servidor con el mismo protocolo que Ollama (más un mini servidor web) para probar sin modelo.

## Estado real: qué está verificado y qué no

**Verificado** (con un Ollama *simulado*, porque aquí no hay Ollama ni GPU): bucle del agente, memoria, todas las herramientas contra servidores locales (web, descarga con límite, subida PUT/POST con secretos), SSRF, permisos y aprobaciones por archivo, compuerta de automodificación (rechaza tocar el juez, errores de sintaxis/compilación y cambios que rompen pruebas), interoperabilidad Python↔C++ y rollback.

**Escrito pero NO probado en real:**
- Hablar con un **Ollama de verdad** y la calidad de un modelo pequeño como agente (los modelos de 3B se equivocan: por eso hay límites de pasos, detección de bucles y reflexión).
- `setup` (instalar Ollama) y la búsqueda web (`DuckDuckGo`): dependen de internet y del formato de su HTML.
- `sleep`/LoRA: necesita GPU, `torch`, `peft`; el mapeo Ollama→Hugging Face y la importación del adaptador en Ollama (`ADAPTER`) están sin verificar.
- **Ejecución en Windows** de la versión C++. Compila sin avisos con GCC y clang (Linux) y se compila y enlaza como `.exe` con MinGW-w64 (cruzado), pero **no he podido ejecutarlo ni compilarlo con MSVC**. Si MSVC da algún error, dímelo.

**Límites**: un LLM pequeño es un agente poco fiable en tareas largas; "automejora ilimitada" tiene el techo de la métrica (`eval`) y del cómputo; no hay emulación cerebral ni nada parecido a *Trascendencia*. Es una base seria y segura para experimentar.

---
> **Nota:** este agente conversacional ahora vive en `legacy/agent/`. El proyecto activo es el **demostrador** (ver el README de la raíz). Los scripts y comandos de este documento se ejecutan desde esta carpeta (`cd legacy/agent`).
