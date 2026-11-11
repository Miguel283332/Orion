#!/usr/bin/env bash
# Prueba de punta a punta del agente Python contra un Ollama simulado (proceso independiente).
set -u; MOCK=""
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${PORT:-11555}
W=$(mktemp -d); trap 'kill $MOCK 2>/dev/null; rm -rf "$W"' EXIT
export OLLAMA_HOST=127.0.0.1:$PORT
fail=0; ok() { echo "  ok  $1"; }; bad() { echo "  FALLO $1"; fail=1; }
start_mock() { kill $MOCK 2>/dev/null; python3 "$ROOT/tests/mock_ollama.py" --port $PORT --script "$1" --log "$W/req.jsonl" >/dev/null & MOCK=$!; sleep 0.7; }
A="python3 $ROOT/orion_agent.py --home $W/h"

# 1) objetivo en la bandeja -> run -> archivo creado
cat > "$W/s1.json" <<'J'
[{"thought":"escribo","tool":"fs_write","args":{"path":"hola.txt","content":"Hola desde Orion"}},
 {"thought":"verifico","tool":"fs_read","args":{"path":"hola.txt"}},
 {"thought":"guardo","tool":"remember","args":{"text":"El archivo hola.txt existe","kind":"fact"}},
 {"thought":"fin","tool":"done","args":{"summary":"Creado hola.txt"}}]
J
start_mock "$W/s1.json"
$A goal "Crea hola.txt" >/dev/null
$A run --max-cycles 3 --interval 0.2 --maintenance-every 1 > "$W/run1.log" 2>&1
[ "$(cat $W/h/workspace/hola.txt 2>/dev/null)" = "Hola desde Orion" ] && ok "run: ejecuta el objetivo de la bandeja y crea el archivo" || { bad "run: archivo"; cat "$W/run1.log"; }
grep -q "Creado hola.txt" "$W/run1.log" && ok "run: resultado final mostrado" || bad "run: resultado"
[ -f "$W/h/inbox_done/"*.txt ] && ok "la bandeja mueve el objetivo a inbox_done" || bad "bandeja"
python3 "$ROOT/orion_agent.py" --home "$W/h" memory | grep -q "fact" && ok "memory: lista lo recordado" || bad "memory"
grep -q '"decision": "permitida"' "$W/h/audit.jsonl" && ok "auditoría escrita" || bad "auditoría"
grep -q '"format": "json"' "$W/req.jsonl" && ok "el cliente pide format=json a Ollama" || bad "format json"

# 2) aprobación SIN terminal (modo Colab): shell pide permiso -> approve desde otro proceso
cat > "$W/s2.json" <<'J'
[{"thought":"shell","tool":"shell","args":{"cmd":"echo aprobado > shell_out.txt"}},
 {"thought":"fin","tool":"done","args":{"summary":"hecho"}}]
J
start_mock "$W/s2.json"
$A goal "ejecuta un comando" >/dev/null
$A run --max-cycles 2 --interval 0.2 > "$W/run2.log" 2>&1 < /dev/null &
RUN=$!
for i in $(seq 1 40); do ID=$($A approvals | sed -n 's/^\[\([0-9a-f]*\)\].*/\1/p' | head -1); [ -n "$ID" ] && break; sleep 0.25; done
[ -n "${ID:-}" ] && ok "aprobaciones: la petición shell queda pendiente ($ID)" || bad "no apareció la petición"
[ ! -f "$W/h/workspace/shell_out.txt" ] && ok "mientras esté pendiente, el comando NO se ejecuta" || bad "se ejecutó sin permiso"
$A approve "$ID" >/dev/null
wait $RUN
grep -q aprobado "$W/h/workspace/shell_out.txt" 2>/dev/null && ok "tras approve, el comando se ejecuta" || { bad "no se ejecutó tras aprobar"; cat "$W/run2.log"; }

# 3) denegación
start_mock "$W/s2.json"
$A goal "otro comando" >/dev/null
rm -f "$W/h/workspace/shell_out.txt"
$A run --max-cycles 2 --interval 0.2 > "$W/run3.log" 2>&1 < /dev/null &
RUN=$!
for i in $(seq 1 40); do ID=$($A approvals | sed -n 's/^\[\([0-9a-f]*\)\].*/\1/p' | head -1); [ -n "$ID" ] && break; sleep 0.25; done
$A deny "$ID" >/dev/null
wait $RUN
[ ! -f "$W/h/workspace/shell_out.txt" ] && grep -q "DENEGADO" "$W/run3.log" && ok "deny: el comando no se ejecuta y el agente lo sabe" || bad "deny"

# 4) chat por stdin
cat > "$W/s4.json" <<'J'
[{"thought":"respondo","tool":"done","args":{"summary":"Hola Miguel, soy Orion."}}]
J
start_mock "$W/s4.json"
OUT=$(printf 'hola\n/memoria\n/salir\n' | $A chat 2>&1)
echo "$OUT" | grep -q "Hola Miguel, soy Orion." && ok "chat: responde por stdin" || { bad "chat"; echo "$OUT"; }

# 5) STOP
$A stop >/dev/null
$A run --max-cycles 2 2>&1 | grep -q "STOP" && ok "stop/resume: el agente respeta STOP" || bad "stop"
$A resume >/dev/null

# 6) sin Ollama: error claro
kill $MOCK 2>/dev/null; sleep 0.3
OUT=$($A eval 2>&1 | head -5)
echo "$OUT" | grep -qi "ollama" && ok "sin Ollama: mensaje de ayuda" || bad "mensaje sin Ollama: $OUT"
exit $fail
