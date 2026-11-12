# Hoja de ruta

| Fase | Entrega | Terminada cuando |
|---|---|---|
| **0. Limpieza** ✅ | Proyectos anteriores en `legacy/`, esqueleto CMake, documentación | Compila con un solo comando |
| **1. Base** ✅ | Guardián, cliente Ollama con streaming y cancelación, chat en terminal que interrumpe el pensamiento a solas, comandos de estadísticas, memoria persistente, reflexión, diario, `yo.md` | Hablas con ella; `/modelo` y `/memoria` dan datos reales; al reiniciar recuerda la conversación |
| **1.5. Acceso total** ✅ (versión 0.2) | Herramientas (comandos, archivos, internet, memoria), "Mi cuerpo" (código, memoria, pesos, hardware), metas "voy a aprender" trabajadas a solas, compilarse y reiniciarse, guardián que aplica versiones y vuelve atrás, `/acciones`, `/permisos`, `/metas`, `/aprende` | Le pides algo en el ordenador y lo hace mostrando cada paso; una meta termina con `★`; una versión rota se revierte sola |
| **2. Memoria y yo** | Búsqueda por significado (embeddings del mismo modelo), resúmenes de conversaciones largas, mejor `yo.md` | Recuerda hechos tuyos de días anteriores sin repetírselos, aunque uses otras palabras |
| **3. Aprender** | Biblioteca de habilidades (scripts que guarda y reutiliza); autoestudio con juez real (ejecutar código, demostrador Lean de `legacy/`); curiosidad (tareas que acierta ~50 %). Las metas básicas ya existen desde la 0.2 | Le pides algo que no sabe, dice "voy a aprender" y vuelve con ello resuelto y verificado |
| **4. Automejora fiable** | Banco de pruebas propio y examen de referencia externo; cada cambio se mide antes de aceptarlo; `/mejoras`, `/volver`. La mecánica (editar, compilar, aplicar, volver atrás) ya existe desde la 0.2 | `/mejoras` muestra cambios aceptados que suben también el examen de referencia |
| **5. Pesos (con RTX)** | llama.cpp integrado; ajuste LoRA con lo aprendido | Sus pesos cambian y el examen de referencia lo confirma |

Cada fase termina con pruebas automáticas y una PR.

## Hardware

- **Ahora**: portátil con i5 y 12 GB de RAM, Windows 11, sin GPU → `qwen2.5:3b` (en torno a 6-10 tokens/s según el i5).
- **Después**: PC con RTX → `/modelo usar qwen2.5:7b` (o mayor) y fase 5.
