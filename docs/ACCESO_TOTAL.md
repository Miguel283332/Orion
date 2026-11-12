# Acceso total: qué puede hacer Orion en tu ordenador

Por decisión tuya, Orion arranca con **permisos totales**. Este documento explica qué significa, cómo verlo todo y cómo cambiarlo.

## Sus herramientas

El modelo decide cuándo usarlas (llamadas a herramientas nativas de Ollama; `qwen2.5` las admite).

| Herramienta | Qué hace | Permiso |
|---|---|---|
| `ejecutar_comando` | cualquier orden en `cmd.exe` (y PowerShell con `powershell -Command`), con tiempo máximo | total |
| `escribir_archivo` | escribir o añadir a cualquier archivo | total |
| `guardar_habilidad` · `usar_habilidad` | guardar un script que funciona (se prueba antes) y volver a usarlo | total |
| `compilar_nueva_version` | compilar su código fuente en `build-auto\` y dejar la versión lista | total |
| `reiniciarme` | reiniciarse (el guardián aplica la versión nueva si la hay) | total |
| `leer_archivo`, `listar_directorio` | leer cualquier archivo o carpeta | lectura |
| `buscar_en_internet` | buscar (DuckDuckGo, con `curl`) | lectura |
| `leer_pagina_web` | descargar una página y quedarse con el texto | lectura |
| `guardar_recuerdo`, `buscar_en_memoria` | su memoria | siempre |
| `crear_meta_de_aprendizaje` | proponerse aprender algo ("voy a aprender") | siempre |

Además "conoce su cuerpo": en cada conversación recibe una sección **Mi cuerpo** con:
- dónde está su **código fuente** y qué hace cada archivo;
- dónde está su **memoria** y qué archivos la forman;
- dónde están sus **pesos**: el archivo GGUF que usa Ollama, con su tamaño, número de parámetros y cuantización;
- su **hardware** y sus **permisos**.

## Qué hace a solas, sin que se lo pidas

Cuando no le hablas, y con permisos `total`:
- **Metas**: usa sus herramientas (internet, comandos, archivos) para aprender lo que se propuso.
- **Práctica**: inventa scripts cortos y los **ejecuta** en `datos/practica/`, para comprobar si predice bien lo que hacen.
- **Automejora medida**: ejecuta `orion --evaluar` (que a su vez usa herramientas en carpetas temporales de `datos/automejora/`), y si acepta un cambio **reescribe `prompts/sistema.md` y hace commit en git** en tu repositorio.
- **Dispositivos**: mide y usa los ordenadores que hayas registrado.

Con `lectura` no practica ni se automejora. Con `ninguno` tampoco trabaja en metas. Todo aparece en la terminal y en la web, y queda en `acciones.jsonl` y `mejoras.jsonl`.

## Sus pesos, siendo honestos

Puede **leer, copiar y medir** el archivo de sus pesos, y crear variantes del modelo con `ollama create` (otras instrucciones de sistema u otros parámetros). Pero **cambiar los pesos de verdad** significa entrenar, y eso es la fase 5: necesita GPU y llama.cpp integrado. En el portátil no es viable. Lo sabe, porque está en su contexto.

## Cómo ver todo lo que hace

- **En la terminal, en directo**: cada herramienta aparece como `· ⚙ ejecutar_comando: …` seguida de `→` y la primera línea del resultado.
- **`/acciones [n]`**: las últimas acciones, con fecha, origen (`chat` o `meta mN`), argumentos y resultado.
- **`datos\acciones.jsonl`**: el registro completo, una línea JSON por acción.
- **Su código**: `git status` y `git diff` en la carpeta del repositorio.
- **`datos\guardian.log`**: arranques, reinicios, versiones aplicadas y vueltas atrás.

## Cómo cambiar los permisos

`/permisos total | lectura | ninguno` (se guarda en `datos\config.json`):
- **total**: todo lo anterior.
- **lectura**: puede leer archivos e internet, pero no ejecutar, escribir ni compilarse.
- **ninguno**: solo su memoria. Tampoco trabaja en metas.

## Recomendaciones (no limitaciones)

- **Carpeta propia**: deja que viva en `C:\Orion`. Con acceso total, un error del modelo (es pequeño y se equivoca) podría borrar o sobrescribir algo. Mejor que no sean tus documentos.
- **Copias de `datos\`**: es su memoria. Cópiala de vez en cuando, o pídele que la versione con git.
- **Ollama solo en local**: no expongas el puerto 11434 a internet (en la red de casa sí, si usas otro dispositivo: ver [ESCALAR.md](ESCALAR.md)).
- **Interfaz web**: solo es accesible desde tu ordenador y exige un token (`datos/web_token.txt`); no lo compartas.
- **Una cuenta de Windows sin administrador** limita lo que un comando equivocado puede romper. Si en algún momento quieres un aislamiento total, una máquina virtual (Hyper-V o VirtualBox) es lo más seguro.
- **Antes de lo irreversible**: se le indica que diga qué va a hacer antes de borrar, sobrescribir o instalar. Es una instrucción, no un bloqueo: un modelo de 3B puede no seguirla siempre.
