# Hoja de ruta

| Fase | Entrega | Terminada cuando |
|---|---|---|
| **0. Limpieza** ✅ | Proyectos anteriores en `legacy/`, esqueleto CMake, documentación | Compila con un solo comando |
| **1. Base** ✅ (pendiente de probar con Ollama real en Windows) | Guardián, cliente Ollama con streaming y cancelación, chat en terminal que interrumpe el pensamiento a solas, comandos de estadísticas, memoria persistente, reflexión, diario, `yo.md` | Hablas con ella; `/modelo` y `/memoria` dan datos reales; al reiniciar recuerda la conversación |
| **2. Memoria y yo** | Búsqueda por significado (embeddings del mismo modelo), resúmenes de conversaciones largas, mejor `yo.md` | Recuerda hechos tuyos de días anteriores sin repetírselos, aunque uses otras palabras |
| **3. Aprender** | Lenguaje de scripts integrado (Lua) para habilidades sin recompilar; metas "voy a aprender"; autoestudio con juez real (ejecutar código, demostrador Lean de `legacy/`); biblioteca de habilidades; curiosidad (tareas que acierta ~50 %) | Le pides algo que no sabe, dice "voy a aprender" y vuelve con ello resuelto y verificado |
| **4. Automejora total** | Edita cualquier archivo; commit → compilar → probar → aceptar o deshacer; el guardián aplica versiones y vuelve atrás; `/mejoras`, `/volver`; examen de referencia externo | `/mejoras` muestra cambios aceptados que suben también el examen de referencia |
| **5. Pesos (con RTX)** | llama.cpp integrado; ajuste LoRA con lo aprendido | Sus pesos cambian y el examen de referencia lo confirma |

Cada fase termina con pruebas automáticas y una PR.

## Hardware

- **Ahora**: portátil con i5 y 12 GB de RAM, Windows 11, sin GPU → `qwen2.5:3b` (en torno a 6-10 tokens/s según el i5).
- **Después**: PC con RTX → `/modelo usar qwen2.5:7b` (o mayor) y fase 5.
