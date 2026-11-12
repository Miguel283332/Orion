# Compilar y ejecutar Orion en Windows 11 con VS Code

Guía paso a paso para tu portátil (i5, 12 GB, sin GPU). Sirve igual para el PC con RTX.

## 1. Instalar lo necesario (una sola vez)

| Programa | Para qué | Cómo comprobarlo (en una terminal nueva) |
|---|---|---|
| **Git** | descargar el código y que Orion guarde sus versiones | `git --version` |
| **CMake** (marca *Add CMake to the system PATH*) | compilar; Orion también lo usa para compilarse a sí misma | `cmake --version` |
| **Build Tools for Visual Studio 2022** (o Visual Studio 2022), con la carga *Desarrollo para el escritorio con C++* | el compilador de C++ (MSVC) | se comprueba al compilar |
| **VS Code** con las extensiones *C/C++* y *CMake Tools* (de Microsoft) | editar y compilar | — |
| **Ollama para Windows** | ejecutar el modelo | `ollama --version` |
| **curl** | internet para Orion | ya viene con Windows 10/11: `curl --version` |

Descarga el modelo (unos 2 GB):
```bash
ollama pull qwen2.5:3b
```

> Si ya compilaste antes el proyecto con MSVC (el error de `/O2` y `/RTC1` era de entonces), el compilador ya está instalado.

## 2. Obtener el código

Primera vez (elige la carpeta donde vivirá Orion; recomendado `C:\Orion`):
```bash
git clone https://github.com/Miguel283332/Orion.git C:\Orion
```

Si ya lo tenías clonado (por ejemplo en el Escritorio), actualízalo:
```bash
cd C:\ruta\a\Orion
```
```bash
git checkout main
```
```bash
git pull origin main
```

> Si `git pull` se queja de cambios locales, guárdalos con `git stash` (o descártalos con `git checkout -- .`) y repite.

## 3. Compilar en VS Code

1. **Archivo → Abrir carpeta…** y elige la carpeta de Orion.
2. Cuando CMake Tools pregunte por el *kit*, elige **Visual Studio Build Tools 2022 Release - amd64** (o *Visual Studio Community 2022 - amd64*).
3. Abajo, en la barra de estado: **preset de configuración → `Orion (build/)`**, **preset de compilación → `Release (rápido)`**.
4. Pulsa **Build** (o `F7`). Se crean:
   - `build\Release\orion.exe`: el agente
   - `build\Release\guardian.exe`: el guardián que la arranca y la reinicia
   - `build\Release\orion_tests.exe`: las pruebas

   (Si VS Code usa el generador *Ninja* en vez de *Visual Studio*, los ejecutables quedan directamente en `build\`.)
5. Opcional, para comprobar que todo funciona: **Run CTest** en la barra de estado, o:
   ```bash
   ctest --test-dir build -C Release --output-on-failure
   ```

Lo mismo desde la terminal (*Terminal → Nueva terminal* en VS Code), en la carpeta de Orion:
```bash
cmake --preset orion
```
```bash
cmake --build build --config Release
```

## 4. Arrancar Orion

1. Comprueba que **Ollama está abierto** (icono de la llama junto al reloj). Si no, ábrelo desde el menú Inicio.
2. En la terminal, **desde la carpeta de Orion** (así sabe dónde está su código y guarda su memoria en `datos\`):
   ```bash
   build\Release\guardian.exe
   ```
3. Verás algo así:
   ```
   Orion · 0 recuerdos, 0 turnos de conversación, 0 entradas de diario · datos en C:\Orion\datos
   Modelo qwen2.5:3b (3.09 mil millones de parámetros, Q4_K_M) · Ollama ...
   Permisos: total (comandos, archivos, internet, su código y su memoria; todo queda en datos/acciones.jsonl)
   Escribe para hablar conmigo. /ayuda para ver los comandos.
   ```
4. Escribe y pulsa Enter. `/salir` para apagarla; `Ctrl+C` también funciona (todo está guardado al momento).

> La primera respuesta tarda más: Ollama carga el modelo en memoria (10-30 s). Después, en un i5 sin GPU, cuenta con unos 6-10 tokens/s.

## 5. Actualizar cuando haya cambios nuevos

1. Apaga Orion (`/salir`): Windows no deja reemplazar un `.exe` que se está ejecutando.
2. Descarga los cambios y recompila:
   ```bash
   git pull origin main
   ```
   ```bash
   cmake --build build --config Release
   ```
3. Arráncala otra vez (`build\Release\guardian.exe`). Su memoria (`datos\`) se conserva entre versiones.

## 6. Cuando Orion se modifica a sí misma

Orion puede editar su código, compilarlo en `build-auto\` (una carpeta aparte, sin pararse) y pedir reiniciarse. El guardián:
- guarda la versión actual como `build\Release\orion.anterior.exe`;
- coloca la nueva en `build\Release\orion.exe` y la arranca;
- si la nueva se cae en su primer minuto, **vuelve a la anterior** y Orion te lo dice al arrancar.

Sus cambios de código quedan en tu carpeta del repositorio. Míralos con:
```bash
git status
```
```bash
git diff
```
Si un cambio suyo no te gusta, descártalo con `git checkout -- <archivo>` y recompila. Para que no se mezclen con los tuyos, puedes pedirle que trabaje en una rama (`git checkout -b orion-cambios`).

## 7. Problemas frecuentes

| Síntoma | Solución |
|---|---|
| `No encuentro Ollama en http://127.0.0.1:11434` | Abre la aplicación Ollama (o `ollama serve` en otra terminal) |
| `Ollama: model "qwen2.5:3b" not found` | `ollama pull qwen2.5:3b` |
| Al compilar: `LNK1168: cannot open orion.exe for writing` | Orion está en marcha: `/salir` y vuelve a compilar |
| `cmake` no se reconoce | Reinstala CMake marcando *Add to PATH*, o pon su ruta en `datos\config.json` (`"cmake": "C:\\Program Files\\CMake\\bin\\cmake.exe"`) |
| CMake no encuentra el compilador | En VS Code elige el *kit* de Visual Studio; en la terminal usa *Developer PowerShell for VS 2022* |
| Tildes raras en la terminal | Usa *Windows Terminal* (por defecto en Windows 11) |
| Va muy lento | Normal en CPU. `/ritmo bajo` para que piense menos a solas; con la RTX, `/modelo usar qwen2.5:7b` |
| Se reinicia una y otra vez | El guardián se detiene tras 3 caídas en 2 minutos; mira `datos\guardian.log` |
| `No pude buscar (¿hay conexión?…)` | Comprueba la conexión y `curl --version` |

## 8. Cuando tengas el PC con RTX

1. Instala lo mismo (sección 1) y los drivers de NVIDIA; Ollama usa la GPU solo.
2. Copia la carpeta `datos\` del portátil al PC: es su memoria (recuerdos, diario, `yo.md`, metas).
3. `ollama pull qwen2.5:7b` (o `qwen2.5:14b` si la GPU tiene 12 GB o más) y, dentro de Orion: `/modelo usar qwen2.5:7b`.
4. `/modelo` debe decir *se ejecuta en: GPU*.
