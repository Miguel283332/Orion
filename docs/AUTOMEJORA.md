# Automejora (diseño de la fase 4)

> Esta parte **todavía no está implementada**: es el diseño acordado. La fase 1 ya deja preparadas las piezas (guardián, git, memoria, medidas).

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
