#!/usr/bin/env bash
# Prueba de punta a punta del demostrador Python: procesos reales (CLI + Ollama simulado + Lean simulado/SymPy real).
set -u; MOCK=""
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${PORT:-11588}
W=$(mktemp -d); trap 'kill $MOCK 2>/dev/null; rm -rf "$W"' EXIT
export OLLAMA_HOST=127.0.0.1:$PORT
unset https_proxy HTTPS_PROXY http_proxy HTTP_PROXY all_proxy ALL_PROXY
fail=0; ok() { echo "  ok  $1"; }; bad() { echo "  FALLO $1"; fail=1; }
start_mock() { kill $MOCK 2>/dev/null; python3 "$ROOT/tests/mock_ollama.py" --port $PORT --reglas "$1" --log "$W/req.jsonl" >/dev/null & MOCK=$!; sleep 0.8; }
D="python3 $ROOT/demostrador.py"
FIRMA='theorem par_mayor (n : Nat) : ∃ m : Nat, m > n ∧ m % 2 = 0 := by'

# ---------------------------------------------------------------- Lean (simulado): problema fácil
cat > "$W/oraculo.json" <<'J'
{"teoremas": [{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"], "axiomas": ["propext"]}]}
J
python3 - "$W" "$FIRMA" <<'PY'
import json, sys
W, F = sys.argv[1], sys.argv[2]
reglas = [
 {"cuando": ["Traduce el siguiente problema"], "responde": {"definiciones": "", "teorema": F}},
 {"cuando": ["Comprueba si este enunciado"], "responde": {"equivalente": True, "motivo": ""}},
 {"cuando": ["Demuestra este teorema en Lean 4", "TU INTENTO ANTERIOR"], "responde": "```lean\n" + F + "\n  exact ⟨2 * n + 2, by omega⟩\n```"},
 {"cuando": ["Demuestra este teorema en Lean 4"], "responde": "```lean\n" + F + "\n  omega\n```", "veces": 1},
]
json.dump(reglas, open(W + "/reglas_lean.json", "w"), ensure_ascii=False)
PY
export MOCK_LEAN_ORACLE="$W/oraculo.json"
start_mock "$W/reglas_lean.json"
$D resolver --problema "$ROOT/problemas/facil_par_mayor.json" --lean "$ROOT/tests/mock_lean.py" --dir "$W/r1" > "$W/r1.log" 2>&1
RC=$?
[ $RC -eq 0 ] && ok "Lean: el problema fácil se resuelve (código de salida 0)" || { bad "Lean: salida $RC"; cat "$W/r1.log"; }
grep -q "2 \* n + 2" "$W/r1/resultado.lean" 2>/dev/null && ok "Lean: resultado.lean con la demostración" || bad "Lean: resultado"
grep -q "PROBLEMA RESUELTO" "$W/r1.log" && grep -q "FALLA" "$W/r1.log" && ok "Lean: el 1.er intento falla y el 2.º lo repara" || bad "Lean: reparación"
grep -q '"format": "json"' "$W/req.jsonl" && ok "el cliente pide JSON en las fases estructuradas" || bad "format json"
$D estado --dir "$W/r1" | grep -q "verificado" && ok "estado: muestra 'verificado'" || bad "estado"
$D resolver --problema "$ROOT/problemas/facil_par_mayor.json" --lean "$ROOT/tests/mock_lean.py" --dir "$W/r1" 2>&1 | grep -q "ya estaba verificado" && ok "volver a ejecutar un problema resuelto no gasta llamadas" || bad "ya verificado"

# ---------------------------------------------------------------- interrupción a mitad de trabajo y reanudación
start_mock "$W/reglas_lean.json"
MOCK_LEAN_SLEEP=2 $D resolver --problema "$ROOT/problemas/facil_par_mayor.json" --lean "$ROOT/tests/mock_lean.py" --dir "$W/r2" > "$W/r2.log" 2>&1 &
PID=$!
for i in $(seq 1 40); do grep -q "Formalización aceptada" "$W/r2.log" 2>/dev/null && break; sleep 0.5; done
sleep 1
kill -TERM $PID 2>/dev/null; wait $PID; RC=$?
[ $RC -eq 130 ] && ok "SIGTERM a mitad de trabajo: salida limpia (130)" || { bad "SIGTERM: salida $RC"; cat "$W/r2.log"; }
python3 -c "import json;e=json.load(open('$W/r2/estado.json'));assert e['estado']=='pausado' and e['formalizado'], e['estado']" 2>/dev/null && ok "el estado quedó guardado (pausado, formalizado)" || bad "estado tras SIGTERM"
start_mock "$W/reglas_lean.json"
$D resolver --problema "$ROOT/problemas/facil_par_mayor.json" --lean "$ROOT/tests/mock_lean.py" --dir "$W/r2" > "$W/r2b.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "Reanudando" "$W/r2b.log" && ok "reanudación: continúa y termina sin rehacer la formalización" || { bad "reanudación (salida $RC)"; cat "$W/r2b.log"; }
[ "$(grep -c 'Traduce el siguiente problema' "$W/req.jsonl")" -le 1 ] || true

# ---------------------------------------------------------------- límite de intentos -> pausa (3) -> reanudar
start_mock "$W/reglas_lean.json"
$D resolver --problema "$ROOT/problemas/facil_par_mayor.json" --lean "$ROOT/tests/mock_lean.py" --dir "$W/r3" --max-intentos 2 > "$W/r3.log" 2>&1; RC=$?
[ $RC -eq 3 ] && grep -q "PAUSADO" "$W/r3.log" && ok "--max-intentos: pausa ordenada (3)" || bad "pausa por presupuesto (salida $RC)"


# ---------------------------------------------------------------- currículo: 2 niveles; el 2.º recibe el 1.º como ejemplo verificado
python3 - "$W" <<'PY'
import json, sys
W = sys.argv[1]
F1 = "theorem cur_a (n : Nat) (h : n % 2 = 1) : (n + 1) % 2 = 0 := by"
F2 = "theorem cur_b (n : Nat) (h : n % 4 = 1) : (n + 3) % 4 = 0 := by"
json.dump({"version": 1, "lean": [
  {"nivel": 1, "id": "cur_a", "titulo": "A", "enunciado": "Si n es impar entonces n + 1 es par (resto modulo 2).", "idea": "omega", "etiquetas": ["omega"]},
  {"nivel": 2, "id": "cur_b", "titulo": "B", "enunciado": "Si n es impar con resto 1 modulo 4 entonces n + 3 es multiplo de 4 (resto modulo).", "idea": "omega", "etiquetas": ["omega"]}]},
  open(W + "/cur.json", "w"), ensure_ascii=False)
json.dump({"teoremas": [{"enunciado_contiene": "cur_a", "acepta_contiene": ["omega"], "axiomas": ["propext"]},
                        {"enunciado_contiene": "cur_b", "acepta_contiene": ["omega"], "axiomas": ["propext"]}]}, open(W + "/or_cur.json", "w"))
reglas = []
for F, k in ((F1, "Si n es impar entonces"), (F2, "Si n es impar con resto")):
    reglas += [{"cuando": ["Traduce el siguiente problema", k], "responde": {"definiciones": "", "teorema": F}},
               {"cuando": ["Demuestra este teorema en Lean 4", F.split(" (")[0]], "responde": "```lean\n" + F + "\n  omega\n```"}]
json.dump(reglas, open(W + "/reglas_cur.json", "w"), ensure_ascii=False)
PY
export MOCK_LEAN_ORACLE="$W/or_cur.json"
start_mock "$W/reglas_cur.json"
$D curriculo --archivo "$W/cur.json" --pista lean --lean "$ROOT/tests/mock_lean.py" --dir "$W/cur" --sin-fidelidad > "$W/cur.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "2/2 verificados" "$W/cur.log" && ok "currículo: 2/2 niveles verificados" || { bad "currículo (salida $RC)"; cat "$W/cur.log"; }
python3 -c "import json;b=json.load(open('$W/cur/biblioteca_lean.json'));assert len(b)==2" 2>/dev/null && ok "currículo: la biblioteca guarda las demostraciones verificadas" || bad "biblioteca"
$D curriculo --archivo "$W/cur.json" --pista lean --lean "$ROOT/tests/mock_lean.py" --dir "$W/cur" 2>&1 | grep -q "ya verificado" && ok "currículo: reanuda sin repetir niveles resueltos" || bad "currículo reanudación"

# ---------------------------------------------------------------- problema incluido de física: la especificación revisada evita la fase 0 (el modelo solo deriva)
python3 - "$W" <<'PY'
import json, sys
W = sys.argv[1]
json.dump([{"cuando": ["Resuelve este problema DERIVANDO"], "responde": "```python\nE = Rational(1, 2)*m*v**2 - G*M*m/r\nv_esc = solve(Eq(E, 0), v)[0]\n```"}],
          open(W + "/reglas_esc.json", "w"), ensure_ascii=False)
PY
start_mock "$W/reglas_esc.json"
$D resolver --problema escape_schwarzschild --dir "$W/esc" > "$W/esc.log" 2>&1; RC=$?
[ $RC -eq 0 ] && grep -q "Especificación incluida" "$W/esc.log" && ok "escape_schwarzschild: especificación incluida, resuelto con una sola llamada al modelo" || { bad "escape incluido (salida $RC)"; cat "$W/esc.log"; }

# ---------------------------------------------------------------- sin Ollama: error claro y estado a salvo
kill $MOCK 2>/dev/null; sleep 0.3
$D resolver --problema "$ROOT/problemas/facil_par_mayor.json" --lean "$ROOT/tests/mock_lean.py" --dir "$W/r3" > "$W/r4.log" 2>&1; RC=$?
[ $RC -eq 4 ] && grep -qi "ollama" "$W/r4.log" && ok "sin Ollama: mensaje claro y salida 4 (el estado sigue guardado)" || { bad "sin Ollama (salida $RC)"; cat "$W/r4.log"; }
unset MOCK_LEAN_ORACLE

# ---------------------------------------------------------------- FÍSICA con SymPy REAL: velocidad de escape y radio de Schwarzschild
python3 - "$W" <<'PY'
import json, sys
W = sys.argv[1]
espec = {"descripcion": "Velocidad de escape v_esc(r) y su valor c en r = 2GM/c^2",
         "preambulo": "G, M, c, r = symbols('G M c r', positive=True)\nr_s = 2*G*M/c**2",
         "variables": ["v_esc"],
         "criterios": ["simplify(v_esc**2 - 2*G*M/r) == 0", "simplify(v_esc.subs(r, r_s) - c) == 0"]}
bien = "```python\nm, v = symbols('m v', positive=True)\nE = Rational(1, 2)*m*v**2 - G*M*m/r\nv_esc = solve(Eq(E, 0), v)[0]\n```"
mal = "```python\nv_esc = c\n```"
reglas = [
 {"cuando": ["Convierte este problema"], "responde": espec},
 {"cuando": ["Comprueba si estos criterios"], "responde": {"equivalente": True, "motivo": ""}},
 {"cuando": ["Resuelve este problema DERIVANDO", "TU INTENTO ANTERIOR"], "responde": bien},
 {"cuando": ["Resuelve este problema DERIVANDO"], "responde": mal, "veces": 1},
]
json.dump(reglas, open(W + "/reglas_py.json", "w"), ensure_ascii=False)
PY
start_mock "$W/reglas_py.json"
$D resolver --problema "$ROOT/problemas/facil_velocidad_escape_schwarzschild.json" --dir "$W/f1" > "$W/f1.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "Física: velocidad de escape = c en el radio de Schwarzschild, verificada con SymPy real" || { bad "Física: salida $RC"; cat "$W/f1.log"; }
grep -q "CRITERIO_FALLA 1" "$W/f1.log" && ok "Física: la respuesta incorrecta (v_esc = c) la rechaza el criterio" || bad "Física: criterio"
grep -q "solve(Eq(E, 0), v)" "$W/f1/resultado.py" && ok "Física: resultado.py con la derivación" || bad "Física: resultado"
python3 "$W/f1/resultado.py" | grep -q TODO_VERIFICADO && ok "Física: resultado.py se ejecuta por sí solo y verifica" || bad "Física: re-ejecución"
# ---------------------------------------------------------------- caso real: sin la carpeta problemas/ (cuaderno de Colab)
export MOCK_LEAN_ORACLE="$W/oraculo.json"
start_mock "$W/reglas_lean.json"
mkdir -p "$W/sin_problemas" && cd "$W/sin_problemas"
$D resolver --problema par_mayor --lean "$ROOT/tests/mock_lean.py" --dir "$W/n1" > "$W/n1.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "problema incluido por nombre (--problema par_mayor): no necesita archivos" || { bad "por nombre (salida $RC)"; cat "$W/n1.log"; }
start_mock "$W/reglas_lean.json"
$D resolver --problema problemas/facil_par_mayor.json --lean "$ROOT/tests/mock_lean.py" --dir "$W/n2" > "$W/n2.log" 2>&1; RC=$?
[ $RC -eq 0 ] && ok "la ruta antigua problemas/facil_par_mayor.json funciona aunque el archivo no exista" || { bad "ruta antigua (salida $RC)"; cat "$W/n2.log"; }
$D resolver --problema no_existe.json --dir "$W/n3" 2>&1 | grep -q "Problemas incluidos" && ok "un problema desconocido da un error claro (sin traceback)" || bad "error claro"
cd "$ROOT"
exit $fail
