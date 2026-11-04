# Orion

IA de **texto** escrita en C++17 que se evalúa, se muta y se mejora a sí misma, y que corre en
**GPU dedicada, GPU integrada o CPU**, eligiendo sola la más rápida.

> Esta versión sustituye al simulador de neuronas anterior (OpenCV/`arecord`), que sigue en el
> historial de git (commit `3fa0710`).

## Qué es (y qué no)

Es un modelo de lenguaje **a nivel de byte** (vocabulario 256, sin tokenizador) con un bucle de
**auto-mejora real y medible**. No es un LLM moderno ni una conciencia: es una base honesta para
experimentar con la idea de una IA que se modifica a sí misma, con las salvaguardas que hacen que
esa idea no se desmorone.

| Pieza | Qué hace |
|---|---|
| `backend` | Una interfaz de cómputo con dos implementaciones: **CPU** (OpenMP + SIMD) y **OpenCL** (NVIDIA, AMD, Intel Arc, Intel/AMD integradas…). Al arrancar mide cada dispositivo y elige el más rápido. |
| `model` | Red de contexto por bytes (embeddings → Dense+ReLU → bloque residual → logits), backprop y Adam propios, ~1,2 M de parámetros por defecto. |
| `evolve` | Bucle de generaciones: **mutar → reentrenar → medir en validación → aceptar o hacer rollback**. |
| `dataset` | Corpus append-only con partición estable train/val/test; memoria incremental. |
| `chat` | Muestreo (temperatura, top-k) y conversación. |
| `tools/try_patch.sh` | Compuerta para que Orion (o quien sea) cambie su **código fuente** con seguridad. |

## Compilar

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Opciones de CMake: `-DORION_OPENCL=ON|OFF` (GPU), `-DORION_OPENMP=ON|OFF`,
`-DORION_NATIVE=ON|OFF` (`-march=native`; ponlo a OFF para un binario portable),
`-DORION_BUILD_TESTS=ON|OFF`. Para la GPU necesitas OpenCL (`ocl-icd-opencl-dev` +
el driver de tu GPU); si no está, Orion compila igualmente y usa solo la CPU.

## Usar

```bash
./build/orion bench                         # dispositivos detectados y GFLOP/s de cada uno
./build/orion train --steps 800             # entrena y guarda el MEJOR punto de validación
./build/orion evolve --generations 20       # auto-mejora (0 = hasta Ctrl-C)
./build/orion chat --auto                   # conversa mientras se mejora en segundo plano
./build/orion gen "Usuario: hola\nOrion:"   # continúa un texto
./build/orion --device cpu|gpu|cl:0:0 ...   # forzar dispositivo (por defecto: auto)
```

Todo el estado vive en `orion_home/` (`--home DIR`): `data/` (corpus), `memory/` (registro de
evolución y lo aprendido), `checkpoints/` (pesos, escritura atómica). Orion puede leer todos esos
archivos, y con `--with-source` también **su propio código** como parte del corpus.

En el chat: `/good` aprueba la última respuesta, `/teach pregunta | respuesta` enseña un par,
`/ingest RUTA` añade textos (o código), `/grow` ensancha la red, `/stats`, `/auto on|off`.

### ¿Base de datos o textos?
**Textos.** El corpus son archivos `.txt` y el modelo aprende de bytes, así que no hace falta una
base de datos. Lo que se aprende de las conversaciones se guarda en `memory/aprendido.txt`.

## Cómo se automejora

Cada generación:

1. Elige un operador con un **bandido**: favorece los que más han mejorado por segundo, sin dejar de explorar.
2. Lo aplica sobre una copia del estado. Operadores: `finetune`, `lr_up/down`, `wd_up/down`,
   `drop_up/down` (regularización), `mutate` (perturba una fracción de los **pesos**, como "tocar
   parámetros" de un modelo existente), `grow_hidden` / `grow_ctx` (la red **crece sin perder lo
   aprendido**: calcula exactamente la misma función antes de seguir entrenando), `batch_up`.
3. Reentrena un presupuesto de pasos y mide la pérdida en **validación**.
4. Si mejora lo suficiente → guarda checkpoint. Si no → **rollback exacto**. Nunca empeora.
5. Registra todo en `memory/evolution.log`.

Resultado medido aquí (CPU de 4 núcleos, corpus de 820 KB): 14 generaciones, ~80 s,
validación 1,133 → 0,980 (perplejidad por byte 3,11 → 2,66). Descubrió solo que quitar el
*context dropout* y bajar el LR ayudaban, y rechazó con rollback `lr_up` y `wd_up`.

### Salvaguardas de diseño
- **El juez no se toca.** La evaluación y los tests quedan fuera del alcance de las mutaciones;
  si pudiera editar su examen, aprendería a aprobarlo en lugar de mejorar.
- **Test reservado.** Se elige con `val`; `test` solo se *informa*. Si `test` se separa de `val`,
  el sistema se está sobreajustando a la validación (`/stats`, `orion info`).
- **No aprende de su propia salida sin supervisión.** `conversations.log` es solo registro; al
  corpus entra únicamente lo que una persona aprueba (`/good`, `/teach`). Entrenar con la propia
  salida degrada el modelo.
- **El chat siempre usa pesos aceptados**: si hablas durante una generación, esta se interrumpe y se revierte.

### Modificar el código fuente
```bash
tools/try_patch.sh cambio.diff            # prueba en una copia: compila, tests, validación y velocidad
tools/try_patch.sh cambio.diff --apply    # y, si pasa, lo aplica y hace commit
```
Rechaza parches que toquen `tests/` o `tools/`, que no compilen, que fallen los tests, que
empeoren la validación (>1 %) o que sean >10 % más lentos. Quién escribe el parche (una persona,
un LLM, el propio Orion) da igual: la compuerta es la misma. Ejecútalo en un contenedor/VM sin
credenciales.

## Límites (para no engañarse)

- **Los datos mandan.** Con el corpus semilla (15 KB) el modelo solo memoriza y el texto generado
  es ruido; la validación se estanca en perplejidad ≈ 10 por byte. Con ~1 MB ya generaliza y mejora
  de forma continua. Para conversar en español con sentido hacen falta **MB de texto**
  (`tools/fetch_corpus.sh` baja libros de dominio público; sin verificar, requiere internet).
- **Arquitectura limitada.** Una ventana fija de contexto no es un Transformer. El backend ya
  ofrece `gemm`, softmax, etc.; añadir atención es el siguiente paso natural.
- **GPU real sin probar.** El backend OpenCL está validado contra la CPU (gemm, gradientes,
  entrenamiento completo) usando `pocl` (OpenCL sobre CPU). No hay una GPU en el entorno de
  desarrollo, así que el rendimiento en una GPU real **no está medido**; el kernel de `gemm` es
  sencillo (tiles 16×16), no está afinado.
- **"Evolucionar infinitamente" tiene techo**: lo marcan los datos, el cómputo y la métrica.
  Aun así, el bucle es el mismo que usarías a mayor escala (mutar → medir → aceptar/revertir).
- **Una sola GPU.** Con varias elige la más rápida, no reparte el trabajo.
