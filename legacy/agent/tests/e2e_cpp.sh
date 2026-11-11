#!/usr/bin/env bash
# Prueba de punta a punta del agente C++ contra un Ollama simulado + mini servidor web (procesos reales).
#   tests/e2e_cpp.sh /ruta/a/orion_agent
set -u; MOCK=""
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:?uso: tests/e2e_cpp.sh /ruta/a/orion_agent}
PORT=${PORT:-11566}
W=$(mktemp -d); trap 'kill $MOCK 2>/dev/null; rm -rf "$W"' EXIT
export OLLAMA_HOST=127.0.0.1:$PORT
unset https_proxy HTTPS_PROXY http_proxy HTTP_PROXY all_proxy ALL_PROXY
fail=0; ok() { echo "  ok  $1"; }; bad() { echo "  FALLO $1"; fail=1; }
start_mock() { kill $MOCK 2>/dev/null; python3 "$ROOT/tests/mock_ollama.py" --port $PORT --script "$1" --log "$W/req.jsonl" --upload-dir "$W/up" >/dev/null & MOCK=$!; sleep 0.7; }
A="$BIN --home $W/h"
BASE=http://127.0.0.1:$PORT

# 1) objetivo -> run -> archivo, memoria, auditoría
cat > "$W/s1.json" <<'J'
[{"thought":"escribo","tool":"fs_write","args":{"path":"hola.txt","content":"Hola desde Orion"}},
 {"thought":"verifico","tool":"fs_read","args":{"path":"hola.txt"}},
 {"thought":"guardo","tool":"remember","args":{"text":"El archivo hola.txt existe","kind":"fact"}},
 {"thought":"fin","tool":"done","args":{"summary":"Creado hola.txt"}}]
J
start_mock "$W/s1.json"
$A goal "Crea hola.txt" >/dev/null
$A run --max-cycles 3 --interval 0.2 --maintenance-every 1 > "$W/run1.log" 2>&1 < /dev/null
[ "$(cat $W/h/workspace/hola.txt 2>/dev/null)" = "Hola desde Orion" ] && ok "run: ejecuta el objetivo de la bandeja y crea el archivo" || { bad "run: archivo"; cat "$W/run1.log"; }
grep -q "Creado hola.txt" "$W/run1.log" && ok "run: resultado final mostrado" || bad "run: resultado"
ls "$W/h/inbox_done/"*.txt >/dev/null 2>&1 && ok "la bandeja mueve el objetivo a inbox_done" || bad "bandeja"
$A memory | grep -q "fact=1" && ok "memory: lo recordado aparece en las estadísticas" || bad "memory"
grep -q '"decision":"permitida"' "$W/h/audit.jsonl" && ok "auditoría escrita" || bad "auditoría"
grep -q '"format": "json"' "$W/req.jsonl" && ok "el cliente pide format=json a Ollama" || bad "format json"
ls "$W/h/trajectories/"*.jsonl >/dev/null 2>&1 && head -c 200 "$W/h/trajectories/"*.jsonl | grep -q '"good":true' && ok "trayectoria buena guardada (consolidable en pesos por la versión Python)" || bad "trayectoria"
python3 "$ROOT/orion_agent.py" --home "$W/h" memory | grep -q "hola.txt existe" && ok "interoperable: la versión Python lee la memoria que escribió la C++" || bad "interop memoria"

# 2) herramientas web contra el mini servidor (con --allow-private-network)
head -c 102400 /dev/urandom > /dev/null
cat > "$W/s2.json" <<J
[{"tool":"web_get","args":{"url":"$BASE/web/redir"}},
 {"tool":"download","args":{"url":"$BASE/web/blob","path":"d/blob.bin"}},
 {"tool":"download","args":{"url":"$BASE/web/big","path":"big.bin","max_mb":1}},
 {"tool":"fs_write","args":{"path":"datos.txt","content":"contenido de prueba"}},
 {"tool":"upload","args":{"url":"$BASE/web/upload","path":"datos.txt"}},
 {"tool":"upload","args":{"url":"$BASE/web/upload","path":"datos.txt","method":"POST","field":"archivo","headers":{"Authorization":"Bearer {{secret:TOK}}"}}},
 {"tool":"done","args":{"summary":"web ok"}}]
J
start_mock "$W/s2.json"
export ORION_SECRET_TOK=tokensecretoabc123
$A goal "prueba web" >/dev/null
$A --autonomy full --allow-private-network run --max-cycles 2 --interval 0.2 > "$W/run2.log" 2>&1 < /dev/null
unset ORION_SECRET_TOK
grep -q "Hola mundo" "$W/run2.log" && ok "web_get: sigue la redirección y extrae el texto" || { bad "web_get"; cat "$W/run2.log" | head -20; }
EXPECT=$(python3 -c "import hashlib;print(hashlib.sha256(bytes(range(256))*400).hexdigest()[:16])")
grep -q "$EXPECT" "$W/run2.log" && cmp -s "$W/h/workspace/d/blob.bin" <(python3 -c "import sys;sys.stdout.buffer.write(bytes(range(256))*400)") && ok "download: contenido y sha256 correctos" || bad "download"
[ ! -e "$W/h/workspace/big.bin" ] && grep -qi "demasiado grande" "$W/run2.log" && ok "download: respeta el tamaño máximo y no deja restos" || bad "tamaño máximo"
python3 - "$W/up/last_upload.json" "$W/up/last_upload.bin" <<'PY' && ok "upload POST multipart con secreto inyectado en la cabecera" || bad "upload POST"
import json,sys
m=json.load(open(sys.argv[1])); b=open(sys.argv[2],"rb").read()
assert m["method"]=="POST" and m["headers"].get("Authorization")=="Bearer tokensecretoabc123", m
assert b'name="archivo"' in b and b"contenido de prueba" in b
PY
grep -q "tokensecretoabc123" "$W/run2.log" "$W/h/audit.jsonl" "$W/h/memory/"*.jsonl 2>/dev/null && bad "el secreto se filtró a logs/auditoría/memoria" || ok "el secreto no aparece en la salida, la auditoría ni la memoria"

# 3) SSRF: sin --allow-private-network no se puede llegar a localhost; redirección a red privada bloqueada
cat > "$W/s3.json" <<J
[{"tool":"web_get","args":{"url":"$BASE/web/page"}},
 {"tool":"done","args":{"summary":"fin"}}]
J
start_mock "$W/s3.json"
$A goal "ssrf" >/dev/null
$A run --max-cycles 2 --interval 0.2 > "$W/run3.log" 2>&1 < /dev/null
grep -q "privada" "$W/run3.log" && ok "SSRF: localhost bloqueado por defecto" || { bad "SSRF"; cat "$W/run3.log"; }

# 4) aprobación sin terminal: shell pide permiso; approve desde otro proceso
cat > "$W/s4.json" <<'J'
[{"thought":"shell","tool":"shell","args":{"cmd":"echo aprobado > shell_out.txt"}},
 {"thought":"fin","tool":"done","args":{"summary":"hecho"}}]
J
start_mock "$W/s4.json"
$A goal "ejecuta un comando" >/dev/null
$A run --max-cycles 2 --interval 0.2 > "$W/run4.log" 2>&1 < /dev/null &
RUN=$!
for i in $(seq 1 40); do ID=$($A approvals | sed -n 's/^\[\([0-9a-f]*\)\].*/\1/p' | head -1); [ -n "$ID" ] && break; sleep 0.25; done
[ -n "${ID:-}" ] && ok "aprobaciones: la petición shell queda pendiente ($ID)" || bad "no apareció la petición"
[ ! -f "$W/h/workspace/shell_out.txt" ] && ok "mientras esté pendiente, el comando NO se ejecuta" || bad "se ejecutó sin permiso"
python3 "$ROOT/orion_agent.py" --home "$W/h" approvals | grep -q "$ID" && ok "interoperable: la versión Python ve la petición de la C++" || bad "interop aprobaciones"
$A approve "$ID" >/dev/null
wait $RUN
grep -q aprobado "$W/h/workspace/shell_out.txt" 2>/dev/null && ok "tras approve, el comando se ejecuta" || { bad "no se ejecutó tras aprobar"; cat "$W/run4.log"; }

# 5) denegación
start_mock "$W/s4.json"
$A goal "otro comando" >/dev/null
rm -f "$W/h/workspace/shell_out.txt"
$A run --max-cycles 2 --interval 0.2 > "$W/run5.log" 2>&1 < /dev/null &
RUN=$!
for i in $(seq 1 40); do ID=$($A approvals | sed -n 's/^\[\([0-9a-f]*\)\].*/\1/p' | head -1); [ -n "$ID" ] && break; sleep 0.25; done
$A deny "$ID" >/dev/null
wait $RUN
[ ! -f "$W/h/workspace/shell_out.txt" ] && grep -q "DENEGADO" "$W/run5.log" && ok "deny: el comando no se ejecuta y el agente lo sabe" || bad "deny"

# 6) chat por stdin, STOP, sin Ollama
cat > "$W/s6.json" <<'J'
[{"thought":"respondo","tool":"done","args":{"summary":"Hola Miguel, soy Orion."}}]
J
start_mock "$W/s6.json"
OUT=$(printf 'hola\n/memoria\n/salir\n' | $A chat 2>&1)
echo "$OUT" | grep -q "Hola Miguel, soy Orion." && ok "chat: responde por stdin" || { bad "chat"; echo "$OUT"; }
$A stop >/dev/null; $A run --max-cycles 2 2>&1 | grep -q "STOP" && ok "stop/resume: el agente respeta STOP" || bad "stop"; $A resume >/dev/null
kill $MOCK 2>/dev/null; sleep 0.3
$A eval 2>&1 | head -5 | grep -qi "ollama" && ok "sin Ollama: mensaje de ayuda" || bad "mensaje sin Ollama"
exit $fail
