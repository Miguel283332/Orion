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

1. **Explorar.** Elige problemas de su currículo (más de lo que más falla), da varios intentos a cada uno y un **juez automático** (SymPy) marca los correctos. No hay opiniones: una respuesta es correcta o no.
2. **Entrenar.** Ajusta sus pesos (adaptador LoRA) con sus propias respuestas verificadas. De los problemas que no resuelve en ningún intento estudia la solución calculada por SymPy.
3. **Medirse.** Repite la **validación**: 180 problemas fijos que nunca entran en el entrenamiento.
4. **Aceptar o deshacer.** Solo se queda con los pesos nuevos si gana claramente más problemas de los que pierde. Si no, vuelve a los anteriores.
5. **Crecer.** Tras 3 generaciones seguidas sin mejorar, amplía su capacidad, de lo más barato a lo más caro:
   explorar el doble → duplicar el rango del adaptador (16 → 128) → extender el adaptador a las capas MLP → **añadirse una capa nueva** (hasta 4) → explorar el cuádruple.
   Cuando ya no cabe más en tu hardware, lo dice: hace falta un modelo base mayor.

Las ampliaciones **conservan lo aprendido**: justo después de ampliar, el modelo responde igual que antes; lo nuevo empieza sin efecto y solo cuenta si el entrenamiento posterior lo aprovecha.

Aparte hay un **examen** (otros 180 problemas) que nunca decide nada: solo informa de si el progreso es real y no una adaptación a la validación.

## Qué aprende

Matemáticas y física con respuesta comprobable, en 9 dominios y 5 niveles de dificultad: aritmética, ecuaciones lineales y cuadráticas, desarrollo de polinomios, derivadas, integrales, sistemas de ecuaciones, cinemática y energía/electricidad. Los problemas se generan sin límite. Cada dominio empieza en el nivel 1 y desbloquea el siguiente al acertar el 60 %.

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

**Modelo base** (automático según la memoria de la GPU): 20 GB o más → `Qwen/Qwen2.5-3B-Instruct`; 6 GB o más → `Qwen2.5-1.5B-Instruct`; menos o sin GPU → `Qwen2.5-0.5B-Instruct`. Sin GPU funciona, pero es demasiado lento para progresar.

## Qué hay en `datos/`

| Archivo | Contenido |
|---|---|
| `config.json` | todos los ajustes (problemas por generación, paciencia, rango máximo, capas máximas…) |
| `estado.json` | generación, modelo base, adaptador aceptado, capas añadidas, resultados y currículo |
| `generaciones.jsonl` | el registro de cada generación y cada ampliación |
| `ejemplos.jsonl` | cada respuesta verificada con la que se ha entrenado |
| `adaptadores/` | sus pesos aprendidos (la versión aceptada) |
| `bases/` | el modelo base ampliado, cuando se ha añadido capas |

## Código

| Archivo | Qué hace |
|---|---|
| `orion/problemas.py` | generadores de problemas por dominio y nivel; validación y examen fijos |
| `orion/juez.py` | decide si una respuesta es correcta (no ejecuta nada de lo que escribe el modelo) |
| `orion/curriculo.py` | qué practicar, cuándo subir de nivel y cuándo una mejora es clara |
| `orion/modelo.py` | el modelo: generar, entrenar, deshacer, ampliar el adaptador y añadir capas |
| `orion/bucle.py` | el bucle de vida |
| `tests/` | 17 pruebas (`python -m pytest tests`), sin GPU ni internet |

## Estado real

**Comprobado** con pruebas automáticas, usando una red diminuta con la misma arquitectura que Qwen:
- el juez acepta la respuesta correcta de 450 problemas y rechaza respuestas falsas o con código;
- entrenar cambia los pesos y deshacer los recupera exactamente;
- ampliar el adaptador y añadir una capa no alteran la salida, y lo nuevo recibe aprendizaje;
- el ciclo completo: acepta una mejora real, deshace un entrenamiento que empeora, recorre todas las ampliaciones al estancarse, continúa tras un reinicio y no deja nada a medias si se corta.

**No comprobado todavía:**
- la ejecución con un Qwen real en GPU (aquí no había GPU ni acceso a Hugging Face);
- cuánto mejora de verdad y a qué ritmo: eso lo dirán `estado` y el examen tras unas horas en tu máquina.

**Límites:**
- Mejora en los dominios que tienen juez. Demostrar teoremas (Lean) y programar con tests son los siguientes dominios que añadir.
- Una capa nueva aprende a través del adaptador, no entera; aporta menos que pasar a un modelo base mayor.
- Todavía no modifica su propio código.

Los proyectos anteriores (el agente en C++ con Ollama incluido) están en `legacy/`.
