# Orion

Orion es un modelo de lenguaje que **se mejora a sí mismo de forma autónoma y continua**: cambia sus propios pesos, mide cada cambio y, cuando se estanca, **amplía su propia capacidad** (hasta añadirse capas). Tú solo lo arrancas.

Parte de un modelo ya entrenado (Qwen2.5, elegido según tu GPU) y repite este ciclo sin parar:

```
 EXPLORAR ──► ENTRENAR ──► MEDIRSE ──► ¿mejora claramente? ──sí──► se queda con los pesos nuevos
    ▲       (sus pesos)  (validación)          │no
    │                                          ▼
    │                              vuelve a los pesos anteriores
    │                                          │
    └──── CRECER (si lleva varias sin mejorar) ◄┘
```

0. **Inventar.** Se inventa ejercicios de programación nuevos (enunciado, solución y entradas). Los que se ejecutan bien, son deterministas y no están repetidos pasan a su currículo: más adelante tendrá que resolverlos solo con el enunciado.
1. **Explorar.** Elige problemas de su currículo (más de lo que más falla) y de sus propios ejercicios, da varios intentos a cada uno y un **juez automático** marca los correctos. No hay opiniones: una respuesta es correcta o no.
2. **Entrenar.** Ajusta sus pesos (adaptador LoRA) con sus propias respuestas verificadas. De los problemas que no resuelve en ningún intento estudia una solución correcta conocida.
3. **Medirse.** Repite la **validación**: 180 problemas fijos que nunca entran en el entrenamiento.
4. **Aceptar o deshacer.** Solo se queda con los pesos nuevos si gana claramente más problemas de los que pierde. Si no, vuelve a los anteriores.
5. **Crecer.** Tras 3 generaciones seguidas sin mejorar, amplía su capacidad, de lo más barato a lo más caro:
   explorar el doble → duplicar el rango del adaptador (16 → 128) → extender el adaptador a las capas MLP → **añadirse una capa nueva** (hasta 4) → explorar el cuádruple → **trasladarse a un modelo base mayor**.
   El traslado solo ocurre si el modelo mayor cabe en la GPU: lo entrena con todo lo que ha verificado hasta entonces y se queda en él solo si no acierta menos. Si no cabe, lo dice y sigue trabajando.

Las ampliaciones **conservan lo aprendido**: justo después de ampliar, el modelo responde igual que antes; lo nuevo empieza sin efecto y solo cuenta si el entrenamiento posterior lo aprovecha.

Aparte hay un **examen** (otros 180 problemas) que nunca decide nada: solo informa de si el progreso es real y no una adaptación a la validación.

## Qué aprende

Todo lo que tiene una respuesta comprobable por una máquina. Ahora, 12 dominios con 5 niveles de dificultad y problemas sin límite:

| Área | Dominios | Juez |
|---|---|---|
| Matemáticas | aritmética, ecuaciones lineales y cuadráticas, polinomios, derivadas, integrales, sistemas | SymPy |
| Física | cinemática, energía y electricidad | SymPy |
| Razonamiento | lógica (secuencias, deducciones, expresiones booleanas, conjuntos, aritmética modular) | respuesta exacta |
| Lenguaje | manipular texto letra a letra (invertir, contar, ordenar, cifrar) | respuesta exacta |
| Programación | escribir funciones de Python (21 familias, de sumar a programación dinámica) | **ejecuta el código** contra casos de prueba |
| Ejercicios propios | los que se inventa ella | ejecuta el código |

Cada dominio empieza en el nivel 1 y desbloquea el siguiente al acertar el 60 %. Añadir un dominio es escribir un generador y, si hace falta, un juez.

**Sobre ejecutar código:** las funciones que escribe el modelo se ejecutan en un proceso aparte, con límite de tiempo y memoria, y solo si pasan un filtro que prohíbe archivos, red e importaciones fuera de una lista corta (`math`, `itertools`…). En Windows no hay límite de memoria, solo de tiempo.

## Arrancarlo

**Google Colab (GPU gratis):** abre [colab/Orion_Colab.ipynb](colab/Orion_Colab.ipynb) y ejecuta las celdas. Guarda todo en tu Google Drive; cuando Colab corte la sesión, vuelve a ejecutarlas y continúa donde estaba.

**PC con RTX (Windows o Linux):**

Primero instala PyTorch con CUDA con el comando que te dé [pytorch.org/get-started/locally](https://pytorch.org/get-started/locally/) para tu sistema. Después:

```bash
pip install -r requirements.txt
```
```bash
python -m orion vivir
```

Se para con Ctrl-C y se puede cortar en cualquier momento: al volver a arrancar continúa desde la última versión aceptada.

| Orden | Qué hace |
|---|---|
| `python -m orion vivir` | el bucle continuo (`--generaciones N` o `--minutos N` para limitarlo) |
| `python -m orion estado` | generación, capas, parámetros, validación, examen, currículo y últimos pasos |
| `python -m orion examen` | repite ahora la validación y el examen |
| `python -m orion chat` | plantearle problemas a mano |
| `python -m orion autoprueba` | comprueba el juez (sin modelo) |

Opciones comunes: `--datos CARPETA` (por defecto `datos/`) y, solo la primera vez, `--modelo NOMBRE`.

**Modelo base** (automático, el mayor que cabe en la GPU): 36 GB o más → `Qwen/Qwen2.5-7B-Instruct`; 14 GB → `Qwen2.5-3B-Instruct`; 6 GB → `Qwen2.5-1.5B-Instruct`; menos o sin GPU → `Qwen2.5-0.5B-Instruct`. Sin GPU funciona, pero es demasiado lento para progresar. Si un día arranca en una GPU mayor (por ejemplo, pasas de Colab al PC con RTX copiando `datos/`), se trasladará sola al modelo siguiente cuando se estanque.

## Qué hay en `datos/`

| Archivo | Contenido |
|---|---|
| `config.json` | todos los ajustes (problemas por generación, paciencia, rango máximo, capas máximas…) |
| `estado.json` | generación, modelo base, adaptador aceptado, capas añadidas, resultados y currículo |
| `generaciones.jsonl` | el registro de cada generación y cada ampliación |
| `ejemplos.jsonl` | cada respuesta verificada con la que se ha entrenado |
| `inventados.jsonl` | los ejercicios que se ha inventado y que funcionan |
| `adaptadores/` | sus pesos aprendidos (la versión aceptada) |
| `bases/` | el modelo base ampliado, cuando se ha añadido capas |

## Código

| Archivo | Qué hace |
|---|---|
| `orion/problemas.py` | matemáticas y física; validación y examen fijos |
| `orion/razonamiento.py` | lógica y lenguaje |
| `orion/programas.py` | programación: ejecución aislada, familias de problemas y ejercicios inventados |
| `orion/juez.py` | decide si una respuesta es correcta |
| `orion/curriculo.py` | qué practicar, cuándo subir de nivel y cuándo una mejora es clara |
| `orion/modelo.py` | el modelo: generar, entrenar, deshacer, ampliar el adaptador y añadir capas |
| `orion/bucle.py` | el bucle de vida: inventar, explorar, entrenar, medir, crecer y trasladarse |
| `tests/` | 24 pruebas (`python -m pytest tests`), sin GPU ni internet |

## Estado real

**Comprobado** con pruebas automáticas, usando una red diminuta con la misma arquitectura que Qwen:
- el juez acepta la respuesta correcta de 600 problemas de los 12 dominios y rechaza las falsas;
- el ejecutor de código corre funciones normales y bloquea archivos, importaciones peligrosas, bucles infinitos y errores;
- un ejercicio inventado solo se guarda si su solución funciona, no es trivial y no está repetido;
- el traslado a un modelo mayor se hace si mejora y se descarta si empeora;
- entrenar cambia los pesos y deshacer los recupera exactamente;
- ampliar el adaptador y añadir una capa no alteran la salida, y lo nuevo recibe aprendizaje;
- el ciclo completo: acepta una mejora real, deshace un entrenamiento que empeora, recorre todas las ampliaciones al estancarse, continúa tras un reinicio y no deja nada a medias si se corta.

**No comprobado todavía:**
- la ejecución con un Qwen real en GPU (aquí no había GPU ni acceso a Hugging Face);
- cuánto mejora de verdad y a qué ritmo: eso lo dirán `estado` y el examen tras unas horas en tu máquina.

**Límites:**
- Solo aprende lo que una máquina puede comprobar. Lo que queda por añadir, por orden de alcance: demostrar teoremas (Lean), aprender leyendo (internet o documentos) y modificar su propio código fuente.
- Un modelo pequeño inventará pocos ejercicios válidos al principio, y un enunciado inventado puede ser ambiguo; por eso sus ejercicios no entran en la validación ni en el examen.
- Una capa nueva aprende a través del adaptador, no entera; aporta menos que trasladarse a un modelo base mayor.
- El techo lo pone la GPU: en una T4 o una RTX doméstica hablamos de modelos de 1,5 a 7 mil millones de parámetros.

Los proyectos anteriores (el agente en C++ con Ollama incluido) están en `legacy/`.
