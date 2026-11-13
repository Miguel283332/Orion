# Orion en Google Colab

[`colab/Orion_Colab.ipynb`](../colab/Orion_Colab.ipynb) clona este repositorio, **compila Orion en C++** y lo ejecuta en Colab con Ollama. Ábrelo en Colab de una de estas formas:
- *Archivo → Subir cuaderno*;
- [este enlace](https://colab.research.google.com/github/Miguel283332/Orion/blob/main/colab/Orion_Colab.ipynb).

Después: *Entorno de ejecución → Cambiar tipo → GPU (T4)* y *Ejecutar todas*.

## Qué hace cada celda

| Celda | Qué hace |
|---|---|
| 1. Configuración | repositorio, rama, modelo (`qwen2.5:7b` con GPU; `qwen2.5:3b` sin ella), si guarda la memoria en Drive |
| 2. GPU | comprueba si hay GPU |
| 3. Ollama | instala Ollama, lo arranca en segundo plano y descarga el modelo |
| 4. Repositorio | `git clone` (o `git pull` si ya estaba) y configura git para que Orion pueda hacer commit de sus mejoras |
| 5. Memoria | monta Google Drive: `MyDrive/orion_datos` sobrevive a que se cierre Colab |
| 6. Compilar | `cmake -S . -B build`, `cmake --build build` y las pruebas (`orion_tests`) |
| 7. Arrancar | lanza el **guardián** (que lanza Orion) en segundo plano; define `orion("texto")` y `actividad()` |
| 8. Interfaz web | abre la interfaz de Orion a través del proxy de Colab (con su token) |
| 9. Hablar | `orion("hola")`, `orion("/modelo")`, `orion("/estado")`… |
| 10. Apagar | `orion("/salir")` |
| 11. Borrar la memoria | apaga Orion y borra toda su memoria (pide `CONFIRMAR = True`; opción para conservar `config.json`) |

## Diferencias con el PC

- Las celdas son Python solo como pegamento de Colab. **Orion es el mismo programa en C++.**
- La interfaz web pasa por el proxy de Colab. Por eso Orion se arranca con `--web-cualquier-host`, que desactiva solo la comprobación del `Host`; **el token sigue siendo obligatorio**.
- Colab cierra la sesión tras un rato sin uso o unas horas. Con Drive su memoria se conserva. **Sus cambios de código** (commits de la automejora) quedan en el clon de Colab y se pierden al cerrar, salvo que los subas a una rama (`git push origin HEAD:orion-colab`).
- Con la T4 el modelo de 7B va mucho más rápido que en el portátil, así que la automejora y la práctica tardan minutos, no horas.
