# Automejora

> **Estado (versión 0.2):** ya funciona la mecánica: puede leer y editar su código, compilarse en `build-auto/`, reiniciarse, y el guardián aplica la versión nueva o vuelve atrás. **Falta** lo que hace que la automejora sea *fiable*: un banco de pruebas que mida si un cambio la mejora, el examen de referencia externo y el registro `/mejoras` (fase 4). Hoy la decisión de cambiarse es del modelo, a veces porque se lo pides tú.

## Cómo se modifica hoy

1. Lee su código (`leer_archivo`) y decide un cambio, por iniciativa propia o porque se lo pides ("mejora cómo escribes tu diario y aplícalo").
2. Lo escribe (`escribir_archivo`) y lo registra con git (`ejecutar_comando`: `git add` / `git commit`).
3. `compilar_nueva_version`: compila en `build-auto/` sin pararse y prueba que la versión nueva arranca (`--version`). Si no compila, recibe los errores para corregirlos.
4. `reiniciarme`: al terminar el turno sale con el código 75. El guardián coloca la versión nueva y la arranca; si se cae en su primer minuto, vuelve a la anterior y ella lo sabrá al arrancar.

Puedes revisar sus cambios con `git log`, `git diff`, y deshacerlos con `git checkout` o `git revert`.

## Principio: acceso total, con memoria de lo que hace y vuelta atrás

Orion podrá modificar **cualquier parte** de sí misma: núcleo C++, prompts, memoria, interfaz, comandos, su banco de pruebas e incluso el guardián. No se le prohíbe nada. A cambio, todo cambio deja rastro y se puede deshacer:

1. **git**: cada cambio que haga es un commit con su explicación (`/versiones`, `/volver <versión>`).
2. **Guardián**: si una versión nueva no arranca o se cae, vuelve a la última que funcionaba.
3. **Examen de referencia** fuera de su carpeta (tuyo): no la limita, sirve para que **tú** sepas si mejora de verdad o solo parece que mejora.
4. **Registro de mejoras** (`/mejoras`): qué cambió, puntuación antes y después, si se aceptó o se deshizo y si tocó su propio evaluador.

## El ciclo

```
leer su código y yo.md → proponer un cambio → commit en una rama → compilar en otra carpeta
   → pasar su banco de pruebas → ¿mejor? → sí: el guardián cambia el ejecutable y la reinicia
                                         → no: se deshace (git) y se anota por qué
```

En Windows un `.exe` en uso no se puede sobrescribir: Orion compila en una carpeta aparte y es el guardián quien sustituye el ejecutable al reiniciarla.

## El riesgo principal: engañarse a sí misma

Un sistema que se optimiza a sí mismo tiende a buscar el atajo: si puede reescribir su examen, "mejora" aprobándose sola. Ya lo vimos en el demostrador anterior, donde un código podía imprimir `TODO_VERIFICADO` o redefinir `simplify` para hacer trampa. Por eso:
- el examen de referencia está fuera de su alcance y lo ejecutas tú (o el guardián) sin que ella lo vea antes;
- cada cambio en sus propias pruebas queda marcado en el registro;
- una mejora solo cuenta si mejora **también** en el examen de referencia.

## Escalar más allá del portátil

Orion mide su hardware y su velocidad, y lo anota en `yo.md`. Cuando necesite más, lo dirá y podrá **usar dispositivos que tú registres** (por ejemplo tu PC con RTX como segundo servidor de Ollama). Te pedirá permiso antes. No se conecta a máquinas que no sean tuyas: no es una limitación de su código, sino la frontera de qué hardware es suyo.

## Recomendaciones
- Que viva en una carpeta propia (`C:\Orion`) o, mejor, en una máquina virtual: con acceso total, un error suyo (borrar algo, llenar el disco) no debe afectar a tus archivos.
- Haz copias de `datos/` de vez en cuando: es su memoria.
