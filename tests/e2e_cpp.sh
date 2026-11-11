#!/usr/bin/env bash
# Prueba de punta a punta del demostrador C++: procesos reales (CLI + Ollama simulado + Lean simulado/SymPy real)
# e interoperabilidad con la versión Python (pausar con una y reanudar con la otra).
#   tests/e2e_cpp.sh /ruta/a/demostrador
set -u; MOCK=""
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:?uso: tests/e2e_cpp.sh /ruta/a/demostrador}
PORT=${PORT:-11599}
W=$(mktemp -d); trap 'kill $MOCK 2>/dev/null; rm -rf "$W"' EXIT
export OLLAMA_HOST=127.0.0.1:$PORT
unset https_proxy HTTPS_PROXY http_proxy HTTP_PROXY all_proxy ALL_PROXY
fail=0; ok() { echo "  ok  $1"; }; bad() { echo "  FALLO $1"; fail=1; }
start_mock() { kill $MOCK 2>/dev/null; python3 "$ROOT/tests/mock_ollama.py" --port $PORT --reglas "$1" --log "$W/req.jsonl" >/dev/null & MOCK=$!; sleep 0.8; }
FIRMA='theorem par_mayor (n : Nat) : ∃ m : Nat, m > n ∧ m % 2 = 0 := by'
LEAN="--lean $ROOT/tests/mock_lean.py"
PROB="$ROOT/problemas/facil_par_mayor.json"
PY="python3 $ROOT/demostrador.py"

cat > "$W/oraculo.json" <<'J'
{"teoremas": [{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"], "axiomas": ["propext"]}]}
J
python3 - "$W" "$FIRMA" <<'PY'
import json, sys
W, F = sys.argv[1], sys.argv[2]
json.dump([
 {"cuando": ["Traduce el siguiente problema"], "responde": {"definiciones": "", "teorema": F}},
 {"cuando": ["Comprueba si este enunciado"], "responde": {"equivalente": True, "motivo": ""}},
 {"cuando": ["Demuestra este teorema en Lean 4", "TU INTENTO ANTERIOR"], "responde": "```lean\n" + F + "\n  exact ⟨2 * n + 2, by omega⟩\n```"},
 {"cuando": ["Demuestra este teorema en Lean 4"], "responde": "```lean\n" + F + "\n  omega\n```", "veces": 1},
], open(W + "/reglas_lean.json", "w"), ensure_ascii=False)
PY
export MOCK_LEAN_ORACLE="$W/oraculo.json"

# ---------------------------------------------------------------- Lean (simulado): problema fácil
start_mock "$W/reglas_lean.json"
$BIN resolver --problema "$PROB" $LEAN --dir "$W/r1" > "$W/r1.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "Lean: el problema fácil se resuelve (salida 0)" || { bad "Lean: salida $RC"; cat "$W/r1.log"; }
grep -q "2 \* n + 2" "$W/r1/resultado.lean" 2>/dev/null && ok "Lean: resultado.lean con la demostración" || bad "Lean: resultado"
grep -q "FALLA" "$W/r1.log" && grep -q "PROBLEMA RESUELTO" "$W/r1.log" && ok "Lean: el 1.er intento falla y el 2.º lo repara" || bad "Lean: reparación"
grep -q '"format": "json"' "$W/req.jsonl" && ok "el cliente pide JSON en las fases estructuradas" || bad "format json"
$BIN estado --dir "$W/r1" | grep -q "verificado" && ok "estado: muestra 'verificado'" || bad "estado"
$BIN resolver --problema "$PROB" $LEAN --dir "$W/r1" 2>&1 | grep -q "ya estaba verificado" && ok "volver a ejecutar un problema resuelto no gasta llamadas" || bad "ya verificado"

# ---------------------------------------------------------------- SIGTERM a mitad de trabajo y reanudación
start_mock "$W/reglas_lean.json"
MOCK_LEAN_SLEEP=2 $BIN resolver --problema "$PROB" $LEAN --dir "$W/r2" > "$W/r2.log" 2>&1 &
PID=$!
for i in $(seq 1 40); do grep -q "Formalización aceptada" "$W/r2.log" 2>/dev/null && break; sleep 0.5; done
sleep 1
kill -TERM $PID 2>/dev/null; wait $PID; RC=$?
[ $RC -eq 130 ] && ok "SIGTERM a mitad de trabajo: salida limpia (130)" || { bad "SIGTERM: salida $RC"; cat "$W/r2.log"; }
python3 -c "import json;e=json.load(open('$W/r2/estado.json'));assert e['estado']=='pausado' and e['formalizado'], e['estado']" 2>/dev/null && ok "el estado quedó guardado (pausado, formalizado)" || bad "estado tras SIGTERM"
start_mock "$W/reglas_lean.json"
$BIN resolver --problema "$PROB" $LEAN --dir "$W/r2" > "$W/r2b.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "Reanudando" "$W/r2b.log" && ok "reanudación: continúa y termina" || { bad "reanudación (salida $RC)"; cat "$W/r2b.log"; }

# ---------------------------------------------------------------- límite de intentos
start_mock "$W/reglas_lean.json"
$BIN resolver --problema "$PROB" $LEAN --dir "$W/r3" --max-intentos 2 > "$W/r3.log" 2>&1; RC=$?
[ $RC -eq 3 ] && grep -q "PAUSADO" "$W/r3.log" && ok "--max-intentos: pausa ordenada (3)" || bad "pausa por presupuesto (salida $RC)"

# ---------------------------------------------------------------- INTEROPERABILIDAD Python <-> C++
start_mock "$W/reglas_lean.json"
$PY resolver --problema "$PROB" $LEAN --dir "$W/i1" --max-intentos 2 > "$W/i1.log" 2>&1; RC=$?
[ $RC -eq 3 ] || bad "interop: Python no pausó (salida $RC)"
$BIN resolver --problema "$PROB" $LEAN --dir "$W/i1" > "$W/i1b.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "Reanudando" "$W/i1b.log" && ok "interop: C++ reanuda el estado que dejó Python" || { bad "interop Python->C++ (salida $RC)"; cat "$W/i1b.log"; }
start_mock "$W/reglas_lean.json"
$BIN resolver --problema "$PROB" $LEAN --dir "$W/i2" --max-intentos 2 > "$W/i2.log" 2>&1; RC=$?
[ $RC -eq 3 ] || bad "interop: C++ no pausó (salida $RC)"
$PY resolver --problema "$PROB" $LEAN --dir "$W/i2" > "$W/i2b.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "Reanudando" "$W/i2b.log" && ok "interop: Python reanuda el estado que dejó C++" || { bad "interop C++->Python (salida $RC)"; cat "$W/i2b.log"; }

# ---------------------------------------------------------------- escape incluido: especificación revisada; un estado viejo (criterios del modelo) se sustituye
python3 - "$W" <<'PY'
import json, sys
W = sys.argv[1]
json.dump([{"cuando": ["Resuelve este problema DERIVANDO"], "responde": "```python\nv_esc = solve(Eq(Rational(1, 2)*m*v**2 - G*M*m/r, 0), v)[0]\n```"}],
          open(W + "/reglas_esc.json", "w"), ensure_ascii=False)
PY
start_mock "$W/reglas_esc.json"
"$BIN" resolver --problema escape_schwarzschild --dir "$W/esc" > "$W/esc.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "Especificación incluida" "$W/esc.log" && ok "C++ escape: especificación incluida, resuelto" || { bad "C++ escape (salida $RC)"; cat "$W/esc.log"; }
python3 - "$W/esc/estado.json" <<'PY'
import json, sys
e = json.load(open(sys.argv[1])); e["criterios"] = ["v_esc == 0"]; e["estado"] = "fallido"
json.dump(e, open(sys.argv[1], "w"), ensure_ascii=False)
PY
"$BIN" resolver --problema escape_schwarzschild --dir "$W/esc" > "$W/esc2.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "especificación revisada" "$W/esc2.log" && ok "C++ escape: un estado con criterios del modelo se sustituye por la especificación revisada" || { bad "C++ escape reanudación (salida $RC)"; cat "$W/esc2.log"; }

# ---------------------------------------------------------------- sin Ollama
kill $MOCK 2>/dev/null; sleep 0.3
$BIN resolver --problema "$PROB" $LEAN --dir "$W/r3" > "$W/r4.log" 2>&1; RC=$?
[ $RC -eq 4 ] && grep -qi "ollama" "$W/r4.log" && ok "sin Ollama: mensaje claro y salida 4 (el estado sigue guardado)" || { bad "sin Ollama (salida $RC)"; cat "$W/r4.log"; }
unset MOCK_LEAN_ORACLE

# ---------------------------------------------------------------- FÍSICA con SymPy real
python3 - "$W" <<'PY'
import json, sys
W = sys.argv[1]
espec = {"descripcion": "Velocidad de escape v_esc(r) y su valor c en r = 2GM/c^2",
         "preambulo": "G, M, c, r = symbols('G M c r', positive=True)\nr_s = 2*G*M/c**2",
         "variables": ["v_esc"],
         "criterios": ["simplify(v_esc**2 - 2*G*M/r) == 0", "simplify(v_esc.subs(r, r_s) - c) == 0"]}
bien = "```python\nm, v = symbols('m v', positive=True)\nE = Rational(1, 2)*m*v**2 - G*M*m/r\nv_esc = solve(Eq(E, 0), v)[0]\n```"
mal = "```python\nv_esc = c\n```"
json.dump([
 {"cuando": ["Convierte este problema"], "responde": espec},
 {"cuando": ["Comprueba si estos criterios"], "responde": {"equivalente": True, "motivo": ""}},
 {"cuando": ["Resuelve este problema DERIVANDO", "TU INTENTO ANTERIOR"], "responde": bien},
 {"cuando": ["Resuelve este problema DERIVANDO"], "responde": mal, "veces": 1},
], open(W + "/reglas_py.json", "w"), ensure_ascii=False)
PY
start_mock "$W/reglas_py.json"
$BIN resolver --problema "$ROOT/problemas/facil_velocidad_escape_schwarzschild.json" --dir "$W/f1" > "$W/f1.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "Física: velocidad de escape = c en el radio de Schwarzschild, verificada con SymPy real" || { bad "Física: salida $RC"; cat "$W/f1.log"; }
grep -q "CRITERIO_FALLA 1" "$W/f1.log" && ok "Física: la respuesta incorrecta (v_esc = c) la rechaza el criterio" || bad "Física: criterio"
python3 "$W/f1/resultado.py" | grep -q TODO_VERIFICADO && ok "Física: resultado.py se ejecuta por sí solo y verifica" || bad "Física: re-ejecución"
# ---------------------------------------------------------------- caso real: sin la carpeta problemas/ (cuaderno de Colab)
export MOCK_LEAN_ORACLE="$W/oraculo.json"
start_mock "$W/reglas_lean.json"
mkdir -p "$W/sin_problemas" && cd "$W/sin_problemas"
$BIN resolver --problema par_mayor --lean "$ROOT/tests/mock_lean.py" --dir "$W/n1" > "$W/n1.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "problema incluido por nombre (--problema par_mayor): no necesita archivos" || { bad "por nombre (salida $RC)"; cat "$W/n1.log"; }
start_mock "$W/reglas_lean.json"
$BIN resolver --problema problemas/facil_par_mayor.json --lean "$ROOT/tests/mock_lean.py" --dir "$W/n2" > "$W/n2.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "la ruta antigua problemas/facil_par_mayor.json funciona aunque el archivo no exista" || { bad "ruta antigua (salida $RC)"; cat "$W/n2.log"; }
$BIN resolver --problema no_existe.json --dir "$W/n3" 2>&1 | grep -q "Problemas incluidos" && ok "un problema desconocido da un error claro (sin traceback)" || bad "error claro"
cd "$ROOT"
exit $fail
