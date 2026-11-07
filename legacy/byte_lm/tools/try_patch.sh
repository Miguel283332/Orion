#!/usr/bin/env bash
# Compuerta de auto-modificación del CÓDIGO de Orion.
#
#   tools/try_patch.sh cambio.diff [--apply]
#
# 1. Rechaza parches que toquen al "juez" (tests/, tools/, CMakeLists de tests, .git).
# 2. Aplica el parche en una COPIA del proyecto (nunca en el original).
# 3. Compila la copia y la línea base, y ejecuta orion_tests en la copia.
# 4. Compara con la base: pérdida de validación tras un entrenamiento fijo (misma semilla)
#    y velocidad. Solo si los tests pasan, la validación no empeora y no es >10% más lenta,
#    el parche se considera bueno. Con --apply se aplica al proyecto y se hace commit.
#
# Quien PRODUCE el parche (una persona, un LLM, el propio Orion) es indiferente: la compuerta
# es la misma. Es el mismo principio que la auto-mejora de pesos: proponer, medir, aceptar o descartar.
set -euo pipefail

PATCH=${1:?uso: tools/try_patch.sh cambio.diff [--apply]}
APPLY=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PATCH=$(cd "$(dirname "$PATCH")" && pwd)/$(basename "$PATCH")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STEPS=${ORION_GATE_STEPS:-300}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}

say() { printf '\n[compuerta] %s\n' "$*"; }
fail() { printf '\n[compuerta] RECHAZADO: %s\n' "$*"; exit 1; }

say "comprobando que el parche no toca el juez"
if grep -E '^(\+\+\+|---) [ab]/(tests/|tools/|\.git)' "$PATCH" >/dev/null; then
  fail "el parche modifica tests/ o tools/ (el juez no puede cambiarse a sí mismo)"
fi

mkdir -p "$WORK/base" "$WORK/cand"
copy_tree() { (cd "$ROOT" && tar --exclude=.git --exclude='build*' --exclude=orion_home -cf - .) | tar -xf - -C "$1"; }
copy_tree "$WORK/base"
copy_tree "$WORK/cand"

say "aplicando el parche a una copia"
(cd "$WORK/cand" && patch -p1 --batch < "$PATCH" >/dev/null) || fail "el parche no se aplica limpiamente"

build() {
  cmake -S "$1" -B "$1/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$1/build" -j"$JOBS" 2>&1 | tail -n 3
}
say "compilando línea base"
build "$WORK/base" >/dev/null || fail "la línea base no compila"
say "compilando candidato"
build "$WORK/cand" >"$WORK/cand_build.log" 2>&1 || { cat "$WORK/cand_build.log"; fail "el candidato no compila"; }

say "ejecutando tests del candidato"
(cd "$WORK/cand/build" && ./orion_tests >"$WORK/tests.log" 2>&1) || { tail -n 15 "$WORK/tests.log"; fail "fallan los tests"; }
tail -n 1 "$WORK/tests.log"

measure() {  # $1 = directorio del build ; imprime "val segundos"
  local home="$WORK/home_$(basename "$(dirname "$1")")"
  rm -rf "$home"
  local t0 t1 out
  t0=$(date +%s.%N)
  out=$("$1/orion" --home "$home" --device cpu --seed 1 train --steps "$STEPS" 2>&1) || { echo "$out" | tail -n 5; return 1; }
  t1=$(date +%s.%N)
  local val
  val=$(echo "$out" | sed -n 's/.*mejor val=\([0-9.]*\).*/\1/p')
  echo "$val $(echo "$t1 - $t0" | bc -l)"
}

say "midiendo línea base y candidato ($STEPS pasos, misma semilla)"
read -r BASE_VAL BASE_T < <(measure "$WORK/base/build")
read -r CAND_VAL CAND_T < <(measure "$WORK/cand/build")
printf '  base:      val=%s  %.1fs\n  candidato: val=%s  %.1fs\n' "$BASE_VAL" "$BASE_T" "$CAND_VAL" "$CAND_T"

OKVAL=$(echo "$CAND_VAL <= $BASE_VAL * 1.01" | bc -l)
OKTIME=$(echo "$CAND_T <= $BASE_T * 1.10" | bc -l)
[ "$OKVAL" = 1 ] || fail "la validación empeora (más de un 1%)"
[ "$OKTIME" = 1 ] || fail "el candidato es más de un 10% más lento"

say "ACEPTADO: compila, pasa los tests, no empeora la validación ni la velocidad"
if [ "$APPLY" = "--apply" ]; then
  (cd "$ROOT" && patch -p1 --batch < "$PATCH" >/dev/null && git add -A && \
   git commit -q -m "Auto-modificación aceptada por la compuerta (val $BASE_VAL -> $CAND_VAL)" && echo "[compuerta] aplicado y commit creado")
else
  say "usa --apply para aplicarlo al proyecto y crear el commit"
fi
