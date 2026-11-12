# Automejora

Orion se mejora de dos formas:

| | Qué cambia | Quién decide | Cómo se comprueba |
|---|---|---|---|
| **Automejora medida** (automática) | sus **instrucciones** (`prompts/sistema.md`) y parámetros (temperatura, herramientas por turno) | ella, a solas, cada `automejora_cada_h` horas o con `/mejorate` | banco de pruebas + examen de referencia, con la misma semilla |
| **Cambio de código** (con herramientas) | **cualquier archivo**, incluido su C++ | ella, si se lo pides o lo decide en una conversación o una meta | compila en `build-auto/`, prueba `--version`; el guardián aplica la versión o vuelve atrás |

## 1. El ciclo medido

```
medir la versión actual con el banco ──► proponer UN cambio (instrucciones y/o parámetros)
          ▲                                          │
          │                                          ▼
   registrar en mejoras.jsonl ◄── aceptar/rechazar ◄── medir la propuesta con el banco
                                         ▲                       │ ¿mejora?
                                         │                       ▼
                                         └──── ¿el examen de referencia no empeora?
```

1. **Mide** su versión actual con `evaluacion/banco.json` (6 tareas: cálculo, contar y crear archivos con herramientas, leer un archivo, honestidad, razonamiento). Cada tarea se ejecuta con el agente completo, herramientas incluidas, en una carpeta temporal y con **semilla fija** (42), en un proceso aparte (`orion --evaluar`). Las mediciones se guardan en caché.
2. **Propone un cambio**: ve sus instrucciones actuales, las tareas que falló (con su respuesta) y los intentos anteriores, y devuelve instrucciones nuevas completas, una temperatura y un máximo de herramientas por turno.
3. **Mide la propuesta** con el banco. Si **no mejora estrictamente**, se rechaza.
4. **Confirma con el examen de referencia** (`evaluacion/examen_referencia.json`, otras 6 tareas que nunca se usan para elegir cambios): si la propuesta lo **empeora**, se rechaza. Así se evita que se especialice en su banco a costa de lo demás.
5. **Acepta**: escribe `prompts/sistema.md`, guarda los parámetros en `config.json` y hace **commit en git** ("Orion: mejora automática: … (banco 3/6 → 4/6, examen 5/6 → 5/6)").
6. **Todo queda registrado** en `datos/mejoras.jsonl`: motivo, antes y después, mediciones, si se aceptó y por qué, commit. Las instrucciones de antes se guardan para poder volver.

Comandos: `/mejorate` (ahora), `/mejoras` (historial), `/volver <n>` (deshace la mejora `n`: restaura sus instrucciones y parámetros, con commit).

**Cuándo lo hace sola:** si los permisos son `total`, lleva al menos 30 minutos sin que le hables y han pasado `automejora_cada_h` horas (12 por defecto; 0 lo desactiva). Tu mensaje la interrumpe incluso a mitad de una medición: el proceso se detiene y el ciclo se retoma más tarde.

### Por qué las instrucciones y no el C++

Con un modelo de 3B, reescribir C++ a solas acabaría casi siempre en código que no compila o que se comporta peor sin que nadie lo note. Sus instrucciones, en cambio:
- deciden gran parte de su comportamiento (cuándo usar herramientas, cómo responder, honestidad);
- se pueden medir en minutos y sin recompilar;
- un modelo pequeño las edita bien.

Cuando tengas la RTX y un modelo mayor, el mismo ciclo puede extenderse a cambios de código (compilar la propuesta en `build-auto/` y medir ese ejecutable). La medición ya funciona con cualquier ejecutable: `evaluar_con_proceso`.

## 2. Cambiar su código

Con acceso total puede leer y modificar cualquier archivo de su repositorio y:
1. registrar el cambio con git (`ejecutar_comando`);
2. `compilar_nueva_version`: compila en `build-auto/` sin pararse y prueba que la versión nueva arranca;
3. `reiniciarme`: el **guardián** coloca la versión nueva y, si se cae en su primer minuto, **vuelve a la anterior**. Ella lo sabrá al arrancar.

Revisa sus cambios con `git log` y `git diff`, y deshazlos con `git revert`.

## Límites (honestos)

- **El banco es pequeño** (6 tareas) para que una medición no tarde horas en CPU. Una mejora de "3/6 → 4/6" puede deberse en parte al azar, aunque la semilla fija lo reduce mucho. Con la RTX conviene ampliar el banco (lo puede hacer ella, queda en git) y el examen (mejor tú).
- **El examen no es inviolable.** Con acceso total, ella podría leerlo o modificarlo. Lo que sí es seguro:
  - su huella SHA-256 se anota la primera vez y, si cambia, `/mejoras` lo avisa;
  - cualquier cambio queda en git;
  - el examen nunca se usa para elegir cambios, solo para vetarlos.
  
  Para un examen realmente fuera de su alcance hace falta otro usuario de Windows u otra máquina.
- **Mejora sus instrucciones, no su inteligencia.** El techo lo pone el modelo; cambiar sus pesos es la fase 5. Los ejemplos verificados de su práctica (`datos/practica.jsonl`) serán los datos de ese entrenamiento.
- Una medición del banco hace unas 6-12 llamadas al modelo. En el i5 eso son **10-30 minutos**; un ciclo completo (base, propuesta, examen) puede llevar una hora. Por eso lo hace cuando no le hablas y como mucho cada 12 horas.
