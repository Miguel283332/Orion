# Cómo aprende Orion (sin cambiar sus pesos)

Sus pesos no cambian hasta la fase 5. Lo que sí cambia, y se acumula, es lo que **recuerda**, lo que **sabe hacer** (habilidades), sus **instrucciones** (automejora medida) y su **calibración** (práctica). Todo está en archivos legibles en `datos/` y en el repositorio.

## Memoria por significado

- Cada recuerdo se convierte en un **vector de significado** con el **mismo modelo** (`/api/embed` de Ollama) y se guarda en `datos/vectores.bin`. Lo hace a solas, en tandas de 16 ("ordenando mis recuerdos por significado").
- Al buscar (al hablar contigo, con `/buscar` o con la herramienta `buscar_en_memoria`) combina:
  - **palabras** (BM25, sin tildes ni palabras vacías);
  - **significado** (coseno entre vectores).
  
  Así encuentra "A Ana le apasiona la física" cuando le preguntas "¿qué ciencia le gusta?".
- Si el modelo no da vectores, lo dice una vez y sigue buscando por palabras; vuelve a probar cada 30 minutos.
- Si cambias de modelo, los vectores antiguos no se mezclan: se vuelve a indexar con el nuevo.
- `/memoria` muestra cuántos recuerdos están indexados.

## Resúmenes de conversaciones antiguas

Al conversar solo caben en el contexto los últimos 16 turnos. Cuando la parte antigua de la conversación suma 20 turnos más, la **resume** (6-10 frases: temas, decisiones, pendientes) y guarda el resumen como un recuerdo de tipo `resumen`, con fecha y número de turnos. Así lo antiguo sigue siendo encontrable por palabras y por significado.

## Metas "voy a aprender", por pasos

1. **Planificar**: divide la meta en 2-6 pasos que pueda hacer sola, y el último siempre es comprobar o poner en práctica.
2. **Un paso por sesión**: usa sus herramientas (internet, comandos, archivos), guarda lo importante como recuerdos `aprendido` y, si crea un script útil que funciona, lo guarda como habilidad. Las notas de cada paso se acumulan en la meta.
3. **Resumen final**: lo esencial, cómo lo comprobó y qué sigue sin estar claro. Te avisa con `★`.

Si le hablas a mitad, lo deja y lo retoma **en el mismo paso**. Si falla 3 veces seguidas en un paso, abandona la meta y lo anota. `/metas` muestra el plan y el paso actual; la interfaz web también.

## Habilidades

Una habilidad es un script (PowerShell o cmd en Windows; sh en Linux) con nombre, descripción y una prueba:
- **Solo se guarda si su prueba funciona** (código de salida 0).
- Cuenta sus usos y sus éxitos.
- Las más relevantes para lo que hablas aparecen en su contexto ("Mis habilidades"), y las usa con `usar_habilidad`.
- Viven en `datos/habilidades/` (`.json` con la descripción y el script). `/habilidades` las lista y `/habilidades borrar <nombre>` borra una.

## Práctica con juez real

Cuando no tiene nada mejor que hacer (cada `practica_cada_min` minutos, 60 por defecto):
1. **Inventa un ejercicio**: un script corto y determinista de la dificultad actual (1 a 5).
2. **Lo ejecuta de verdad.** Si no funciona, lo descarta.
3. **Predice** lo que imprime, sin verlo, en un contexto limpio.
4. **Compara** la predicción con la salida real: eso es el juez, sin opiniones.
5. **Ajusta la dificultad**: si acierta 3 de las últimas 4, sube; si acierta 1 o ninguna, baja. Así practica siempre cerca de su límite (~50 %), que es donde más se aprende.
6. Si falla, saca **una lección** en una frase y la guarda como recuerdo (`leccion`).

Cada ejercicio verificado queda en `datos/practica.jsonl`: script, predicción, salida real y acierto. Son exactamente los datos que se usarán para **entrenar sus pesos en la fase 5** (con GPU). `/practica` muestra su nivel y sus aciertos; `/practica ahora` la pone a practicar.
