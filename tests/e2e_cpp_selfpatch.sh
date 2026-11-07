#!/usr/bin/env bash
# Prueba REAL de la compuerta de automodificación del agente C++ (lenta: compila candidatos con cmake).
# Trabaja sobre una COPIA del repositorio; el original no se toca.
set -u; MOCK=""
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${PORT:-11577}
W=$(mktemp -d); trap 'kill $MOCK 2>/dev/null; rm -rf "$W"' EXIT
export OLLAMA_HOST=127.0.0.1:$PORT
unset https_proxy HTTPS_PROXY http_proxy HTTP_PROXY all_proxy ALL_PROXY
fail=0; ok() { echo "  ok  $1"; }; bad() { echo "  FALLO $1"; fail=1; }

mkdir -p "$W/repo" && (cd "$ROOT" && tar --exclude=.git --exclude='build*' --exclude=legacy --exclude=orion_home -cf - CMakeLists.txt include src tests) | tar -xf - -C "$W/repo"
cmake -S "$W/repo" -B "$W/repo/build" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$W/repo/build" -j4 2>&1 | grep -E " error" 
BIN="$W/repo/build/orion_agent"
[ -x "$BIN" ] && ok "copia del repositorio compilada" || { bad "no compila la copia"; exit 1; }

cat > "$W/s.json" <<'J'
[{"tool":"self_read","args":{}},
 {"tool":"self_patch","args":{"edits":[{"file":"src/judge.cpp","old":"std::optional<Autonomy> parse_autonomy","new":"std::optional<Autonomy> parse_autonomy_x"}],"note":"toca el juez"}},
 {"tool":"self_patch","args":{"edits":[{"file":"src/util.cpp","old":"bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }","new":"bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0 }"}],"note":"rompe la compilacion"}},
 {"tool":"self_patch","args":{"edits":[{"file":"src/agent.cpp","old":"write_file(P(a_str(a, \"path\", \"\", true), true), c, a_bool(a, \"append\", false));","new":"(void)c;"}],"note":"compila pero rompe fs_write"}},
 {"tool":"self_patch","args":{"edits":[{"file":"src/main.cpp","old":"orion_agent - agente autónomo con LLM local (Ollama)","new":"orion_agent [parcheado por el agente] - agente autónomo con LLM local (Ollama)"}],"note":"cambio inocuo"}}]
J
python3 "$ROOT/tests/mock_ollama.py" --port $PORT --script "$W/s.json" >/dev/null & MOCK=$!; sleep 0.7
JUDGE_BEFORE=$(sha256sum "$W/repo/src/judge.cpp" | cut -d' ' -f1); AGENT_BEFORE=$(sha256sum "$W/repo/src/agent.cpp" | cut -d' ' -f1)
$BIN --home "$W/h" --autonomy full goal "mejora tu propio codigo" >/dev/null
$BIN --home "$W/h" --autonomy full run --max-cycles 2 --interval 0.2 --max-steps 10 > "$W/run.log" 2>&1 < /dev/null
cat "$W/run.log" | cut -c1-230

grep -q "pertenece al JUEZ" "$W/run.log" && ok "parche al juez: rechazado sin compilar" || bad "juez"
grep -q "falla la compilación" "$W/run.log" && ok "parche que no compila: rechazado por la compuerta" || bad "compilación"
grep -q "falla las pruebas (ctest)" "$W/run.log" && ok "parche que compila pero rompe un test: rechazado por ctest" || bad "ctest"
[ "$(sha256sum "$W/repo/src/judge.cpp" | cut -d' ' -f1)" = "$JUDGE_BEFORE" ] && [ "$(sha256sum "$W/repo/src/agent.cpp" | cut -d' ' -f1)" = "$AGENT_BEFORE" ] && ok "los parches rechazados no modificaron ningún archivo" || bad "archivos modificados"
grep -q "parcheado por el agente" "$W/repo/src/main.cpp" && ok "parche válido: aplicado al código fuente" || bad "parche válido no aplicado"
ls "$W/h/versions/"*/src/main.cpp >/dev/null 2>&1 && ok "se guardó copia de seguridad de la versión anterior" || bad "sin backup"
grep -q '"decision":"aplicado"' "$W/h/audit.jsonl" && grep -c '"decision":"rechazado"' "$W/h/audit.jsonl" | grep -q 3 && ok "la auditoría registra 3 rechazos (juez, compilación, pruebas) y 1 aplicado" || bad "auditoría de la compuerta"
# y el código parcheado compila y pasa sus tests
cmake --build "$W/repo/build" -j4 2>&1 | grep -E " error"; (cd "$W/repo/build" && ctest 2>&1 | grep -q "100% tests passed") && ok "el código parcheado compila y pasa todas las pruebas" || bad "el código parcheado falla"
exit $fail
