# Escalar: pensar en otro ordenador tuyo

Orion mide su hardware y su velocidad. Cuando nota que el hardware la limita, te lo dice, como mucho una vez al día y también con `/limites`. Los motivos:
- piensa a menos de 4 tokens/s;
- queda menos de 1,5 GB de RAM libre;
- su memoria de trabajo (contexto) se llena.

La solución es que use **otro ordenador tuyo** con Ollama, por ejemplo el PC con la RTX. Solo usa los dispositivos que **tú registras**: no busca ni usa otras máquinas por su cuenta.

## 1. Preparar el PC con la RTX (una vez)

1. Instala Ollama y descarga el modelo, por ejemplo:
   ```bash
   ollama pull qwen2.5:7b
   ```
2. Haz que Ollama escuche en la red local, no solo en `127.0.0.1`. En Windows: *Configuración → Sistema → Información → Configuración avanzada del sistema → Variables de entorno*, y añade la variable de usuario `OLLAMA_HOST` con el valor `0.0.0.0`. Después **cierra y vuelve a abrir Ollama** (icono junto al reloj → *Quit* y ábrelo otra vez).
3. Permite el puerto en el cortafuegos (PowerShell **como administrador**):
   ```powershell
   New-NetFirewallRule -DisplayName "Ollama" -Direction Inbound -Protocol TCP -LocalPort 11434 -Action Allow -Profile Private
   ```
4. Averigua su IP local con `ipconfig` (por ejemplo `192.168.1.50`).

> Hazlo solo en tu red de casa (perfil *Privado*). No abras el puerto 11434 a internet: cualquiera podría usar tu Ollama.

## 2. Registrarlo en Orion (en el portátil)

```
/dispositivo añadir pc http://192.168.1.50:11434 qwen2.5:7b
```
- El modelo es opcional: si no lo pones, usa el mismo que en el portátil. Sigue habiendo **un solo modelo a la vez**: el del ordenador donde piensa.
- `/dispositivos` muestra cada uno: si responde y su velocidad medida.
- `/dispositivo medir` mide la velocidad de todos.

## 3. Qué hace sola

Cada 10 minutos, sin usar el modelo:
- **Mide** los dispositivos que respondan y no estén medidos.
- **Se pasa al más rápido** si lo es claramente (al menos 1,5 veces más que el actual). Te lo dice (`★ Me paso a pensar en «pc» (45.0 tokens/s frente a 7.0 aquí)`) y lo anota en su conversación.
- Si el dispositivo actual **deja de responder** (apagas el PC), **vuelve al portátil** y te avisa.

Su memoria, su código y sus herramientas siguen en el **portátil**: en el otro ordenador solo se ejecuta el modelo. Por eso los comandos y archivos siguen actuando en el portátil.

Manual:
- `/dispositivo usar pc` (o `usar local`): cambiar a mano;
- `/dispositivo auto off`: que no cambie sola, salvo para volver al local si el otro cae;
- `/dispositivo quitar pc`: quitarlo.

## Cuando el PC con RTX sea el principal

Si prefieres que Orion viva en el PC, con su memoria y sus herramientas allí:
1. copia la carpeta `datos\`;
2. compila el repositorio en el PC (ver [WINDOWS.md](WINDOWS.md));
3. arranca el guardián allí;
4. usa `/modelo usar qwen2.5:7b`.

Entonces puedes registrar el portátil como dispositivo, o no hacerlo.
