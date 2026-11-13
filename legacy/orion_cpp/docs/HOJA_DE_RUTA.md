# Hoja de ruta

| Fase | Entrega | Terminada cuando |
|---|---|---|
| **0. Limpieza** ✅ | Proyectos anteriores en `legacy/`, esqueleto CMake, documentación | Compila con un solo comando |
| **1. Base** ✅ | Guardián, cliente Ollama con streaming y cancelación, chat en terminal que interrumpe el pensamiento a solas, comandos de estadísticas, memoria persistente, reflexión, diario, `yo.md` | Hablas con ella; `/modelo` y `/memoria` dan datos reales; al reiniciar recuerda la conversación |
| **1.5. Acceso total** ✅ (versión 0.2) | Herramientas (comandos, archivos, internet, memoria), "Mi cuerpo" (código, memoria, pesos, hardware), metas "voy a aprender" trabajadas a solas, compilarse y reiniciarse, guardián que aplica versiones y vuelve atrás, `/acciones`, `/permisos`, `/metas`, `/aprende` | Le pides algo en el ordenador y lo hace mostrando cada paso; una meta termina con `★`; una versión rota se revierte sola |
| **2. Memoria y yo** ✅ (0.3) | Búsqueda por significado (vectores del mismo modelo) + palabras; resúmenes de conversaciones antiguas | Encuentra "física" al preguntar por "ciencia"; lo antiguo sigue siendo encontrable |
| **3. Aprender** ✅ (0.3) | Metas por pasos reanudables; habilidades (scripts verificados); práctica con juez real y dificultad adaptativa; lecciones de los fallos | Le pides algo que no sabe, dice "voy a aprender", lo trabaja por pasos y vuelve con ello (`★`) |
| **4. Automejora fiable** ✅ (0.3) | Banco de pruebas + examen de referencia con huella; ciclo medido sobre sus instrucciones y parámetros; commit; `/mejoras`, `/mejorate`, `/volver` | `/mejoras` muestra cambios aceptados que mejoran el banco sin empeorar el examen |
| **Interfaz** ✅ (0.3) | Interfaz web local con paneles, segura (127.0.0.1, token, `Host`), modificable por ella | Se ve en el navegador lo que hace, y se habla con ella |
| **Escalar** ✅ (0.3) | Dispositivos registrados; cambio automático al más rápido y vuelta al local; avisos de límites | Con el PC encendido piensa allí; si lo apagas, vuelve al portátil |
| **5. Pesos (con RTX)** | llama.cpp integrado; ajuste LoRA con lo aprendido y verificado (`practica.jsonl`, metas, conversaciones) | Sus pesos cambian y el examen de referencia lo confirma |
| **Pendiente (pequeño)** | Confirmación opcional antes de acciones irreversibles; tareas programadas; copias automáticas de `datos/` | — |

Cada fase termina con pruebas automáticas y una PR.

## Hardware

- **Ahora**: portátil con i5 y 12 GB de RAM, Windows 11, sin GPU → `qwen2.5:3b` (en torno a 6-10 tokens/s según el i5).
- **Después**: PC con RTX → `/modelo usar qwen2.5:7b` (o mayor) y fase 5.
