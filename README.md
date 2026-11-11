# Demostrador

Resuelve **problemas matemáticos y físicos** con un modelo local de **Ollama** (p. ej. `qwen2.5:7b`) y un **verificador externo**: el modelo *propone* y el verificador *decide*. Está pensado para problemas **largos** y para poder **interrumpirlo en cualquier momento** sin perder trabajo.

| Backend | Para qué | Juez |
|---|---|---|
| `lean` | Matemáticas: demostraciones formales | **Lean 4** (compila la demostración y comprueba los axiomas) |
| `sympy` | Física y cálculo simbólico: derivaciones | Criterios fijados **antes** de resolver, ejecutados con **SymPy** |

Hay **dos implementaciones** con el mismo algoritmo y el **mismo formato de estado** (puedes pausar con una y reanudar con la otra):

| | Archivo | Para qué |
|---|---|---|
| **Python** | [`demostrador.py`](demostrador.py) (un solo archivo, solo biblioteca estándar) | Google Colab y cualquier PC |
| **C++** | [`src/`](src), [`include/`](include), [`CMakeLists.txt`](CMakeLists.txt) | Local y rápido, sin intérprete (para tu RTX) |

> El agente conversacional y el modelo de bytes anteriores siguen en [`legacy/`](legacy).

## Cómo funciona

```
 enunciado ──► FASE 0: formalizar ──► árbol de demostración (se guarda tras CADA intento)
              (el verificador comprueba     │
               el enunciado; el modelo       ├─ 1. tácticas automáticas   (first | omega | simp | decide | ...)
               revisa su fidelidad)          ├─ 2. intentos directos       (con el código y el error del intento anterior)
                                             └─ 3. descomposición en lemas (recursiva): cada lema es un subárbol;
                                                  los ya demostrados se conservan si el plan falla
```

- **El juez es externo.** En Lean, una demostración solo vale si compila **y** `#print axioms` solo muestra `propext`, `Classical.choice`, `Quot.sound` (sin `sorry` ni axiomas propios). El modelo no puede declarar que algo está demostrado.
- **Reparación guiada.** Cada reintento recibe el código anterior y el error del verificador, no solo "falló".
- **Reanudable.** Todo el árbol (lemas probados, intentos, errores, semillas) está en `estado.json`, escrito de forma atómica tras cada intento. Ejecutar de nuevo el mismo comando **continúa exactamente donde se quedó**, también tras Ctrl-C, `SIGTERM`, una caída de Ollama o un corte de la sesión de Colab.
- **Presupuesto.** `--horas` y `--max-intentos` limitan *una sesión*; al llegar, se pausa con el progreso guardado.

## Empezar

### Google Colab
Abre [`colab/Demostrador_Colab.ipynb`](colab/Demostrador_Colab.ipynb): instala Ollama, Lean 4 y el modelo, monta Drive (para que el progreso sobreviva) y resuelve los dos problemas de ejemplo.

### Python
Necesitas [Ollama](https://ollama.com/download) con el modelo, y Lean 4 ([elan](https://github.com/leanprover/elan)) para `lean` o `pip install sympy` para `sympy`.

```bash
ollama pull qwen2.5:7b
python demostrador.py problemas                          # lista los problemas incluidos (no hacen falta archivos)
python demostrador.py resolver --problema par_mayor
python demostrador.py resolver --problema escape_schwarzschild
python demostrador.py estado --dir demostrador_runs/par_mayor_xxxxxxxx     # progreso
```

### C++ (CMake / Visual Studio Code)
```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/demostrador resolver --problema par_mayor     # Windows: .\build\Release\demostrador.exe
```
Necesita `curl` en el PATH (viene con Windows 10+ y Linux/macOS) y `python3` con `sympy` solo para el backend `sympy`.

## Problemas

Los dos de ejemplo van **dentro del programa** (`--problema par_mayor`, `--problema escape_schwarzschild`). Para otros, un archivo JSON (`--problema ruta.json`; hay ejemplos en [`problemas/`](problemas)) o `--enunciado "texto" --backend lean|sympy`:
```json
{ "id": "par_mayor", "dominio": "matematicas", "backend": "lean",
  "enunciado": "Demuestra la existencia de un número natural m mayor que cualquier n dado, tal que m sea par." }
```
- `facil_par_mayor.json` (Lean): el problema inicial.
- `facil_velocidad_escape_schwarzschild.json` (SymPy): deduce la velocidad de escape y demuestra que vale `c` en el radio de Schwarzschild `r_s = 2GM/c²`. Es el primer paso hacia los agujeros negros.

### Problemas largos o difíciles
```bash
python demostrador.py resolver --problema problemas/tu_problema.json --horas 3 --directos 10 --replanes 5 --profundidad 3
```
Si el plan actual no basta, el motor pide lemas nuevos **sin descartar los ya demostrados**. Códigos de salida: `0` verificado · `1` no resuelto con esos límites · `3` pausa por presupuesto · `4` Ollama caído · `130` interrumpido (en los tres últimos casos, vuelve a ejecutar el mismo comando).

Para análisis real o Mathlib hace falta un proyecto *lake* con Mathlib: `--lake-dir RUTA --mathlib`.

## Currículo y autoaprendizaje (solo Python por ahora)

```bash
python demostrador.py curriculo --pista lean  --dir curriculo_runs --horas 3     # 18 niveles de Lean, de omega a mcd/Fibonacci
python demostrador.py curriculo --pista sympy --dir curriculo_runs               # 12 niveles de física hasta Schwarzschild
```
[`curriculo/curriculo.json`](curriculo/curriculo.json) ordena los problemas por dificultad. Cada demostración **verificada** se guarda en `biblioteca_<pista>.json` y las más parecidas se muestran al modelo como ejemplos en los niveles siguientes (también con `resolver --biblioteca RUTA`). No cambia los pesos del modelo: mejora el *contexto* con lo ya demostrado. El progreso se reanuda (`progreso_<pista>.json`).

La pista `abiertos` contiene conjeturas famosas (Goldbach, Collatz, primos gemelos...) **solo en su versión acotada y comprobable**; el enunciado general sigue abierto y no se afirma que el sistema pueda resolverlo. Los enunciados del currículo están revisados a mano, no compilados con Lean real.

Endurecimiento tras revisión (Python y C++): el marcador `TODO_VERIFICADO` debe aparecer una sola vez y al final, los criterios deben ser booleanos estrictos, más construcciones prohibidas (`SystemExit`, `os`, `sys`, dunders, `trace`, `+native`...) y `keep_alive` en Ollama. **Sigue sin ser un sandbox real** (ver límites).

## Errores del cuaderno original que se han corregido

1. **Se perdía el progreso del plan**: el estado no guardaba los lemas ni las demostraciones ya hechas, así que al reanudar se rehacía todo. Ahora se guarda el árbol completo tras cada intento.
2. **Las semillas se repetían tras reanudar** (`seed = 100 + intento`, y el contador volvía a 1): se reproducían los mismos intentos. Ahora son únicas y persistentes.
3. **El modelo no veía su intento anterior**, solo un texto de error. Ahora recibe el código que falló y el error de Lean, más un historial de errores para no repetirlos.
4. **`aesop` y `norm_num` en `import Lean`**: necesitan Mathlib y siempre fallaban, gastando compilaciones. Ahora la lista de tácticas depende de si hay Mathlib, y se prueban todas en **una** compilación con `first | ... | ...`.
5. **La guía de Lean era errónea**: decía que `exact ⟨t, p, q⟩` no vale para `∃ x, P x ∧ Q x`, y sí vale.
6. **Sin comprobación de axiomas**: ahora `#print axioms` rechaza `sorryAx` y axiomas propios; se prohíben también `native_decide`, `unsafe`, `macro`, `set_option`, `import`…
7. **`normalizar` aplicaba NFKC antes de reemplazar `ℕ`**, que ya se había convertido en `N` y nunca llegaba a `Nat`.
8. **Extracción frágil**: tomaba siempre el *último* bloque de código (suele ser una explicación) y no respetaba la sangría relativa del cuerpo.
9. **Sin reintentos ni control de fallos de Ollama**: una caída abortaba todo. Ahora guarda el estado y sale con un mensaje claro.
10. **Sin descomposición recursiva** y sin tiempo/intentos máximos: ahora hay ambos.

## Pruebas

```bash
python demostrador.py selftest        # 42 pruebas del motor (Lean simulado, SymPy real)
tests/e2e_python.sh                   # procesos reales: CLI + Ollama simulado + Lean simulado/SymPy real
ctest --test-dir build                # 129 comprobaciones del motor C++
tests/e2e_cpp.sh build/demostrador    # procesos reales C++, SIGTERM, reanudación e interoperabilidad con Python
```

## Estado real: qué está verificado y qué no

**Verificado** (con un Ollama y un Lean *simulados*, `tests/mock_ollama.py` y `tests/mock_lean.py`, porque en el entorno de desarrollo no había Ollama ni Lean): todo el motor (formalización, intentos, reparación, planes recursivos, conservación de lemas, axiomas, prohibidos, repetidos), la reanudación tras pausa, `SIGTERM`, caída de Ollama y estado corrupto, y la interoperabilidad Python↔C++. La parte de **física usa SymPy real**: la velocidad de escape se deriva y se verifica de verdad. La versión C++ compila sin avisos con GCC y clang y se compila y enlaza como `.exe` con MinGW-w64.

**NO verificado con el Lean real** (y puede necesitar ajustes cuando lo pruebes):
- El formato exacto de los mensajes de Lean y la sintaxis de `first | ... | ...` en varias líneas; que `omega` y `exact?` existan en tu versión de Lean.
- La calidad real de `qwen2.5:7b` formalizando y demostrando; el modelo se equivoca, y los problemas difíciles (agujeros negros) probablemente necesiten Mathlib y mucho tiempo.
- La versión C++ con **MSVC**, o ejecutada en Windows.

**Límites que conviene conocer:**
- **Física ≠ Lean.** En el backend `sympy` los *criterios* los escribe el modelo antes de resolver; la verificación es tan fiable como esos criterios. Revísalos (salen en el registro). No es una demostración formal.
- Que el enunciado formal sea **fiel** al problema solo lo comprueba el propio modelo (señal débil). En Lean, revisa la formalización a mano en problemas importantes.
- Un modelo de 7B resuelve problemas fáciles; para problemas profundos, el límite es el modelo, no el motor.
