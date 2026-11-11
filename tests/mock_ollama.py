#!/usr/bin/env python3
"""Servidor Ollama SIMULADO por reglas (mismo protocolo HTTP). Sirve para probar Python y C++ sin modelo.

  python tests/mock_ollama.py --port 11555 --reglas reglas.json [--log peticiones.jsonl]

`reglas.json`: lista de {"cuando": ["texto1", ...], "responde": "texto" | {json}, "veces": N (opcional)}.
Se usa la primera regla cuyos textos aparezcan TODOS en el mensaje de usuario y que no haya agotado sus `veces`.
Si ninguna coincide responde con {"error": "sin regla"} (para detectarlo en las pruebas).
"""
import argparse, json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=11555)
    ap.add_argument("--reglas", required=True)
    ap.add_argument("--log", default=None)
    a = ap.parse_args()
    reglas = json.load(open(a.reglas, encoding="utf-8"))
    usos = [0] * len(reglas)
    lock = threading.Lock()

    class H(BaseHTTPRequestHandler):
        def _send(self, obj, code=200):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_GET(self):
            self._send({"models": [{"name": "mock:latest"}]})

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            usuario = body["messages"][-1]["content"]
            with lock:
                if a.log:
                    open(a.log, "a", encoding="utf-8").write(json.dumps({"body": body}, ensure_ascii=False) + "\n")
                for i, r in enumerate(reglas):
                    if all(t in usuario for t in r["cuando"]) and (r.get("veces") is None or usos[i] < r["veces"]):
                        usos[i] += 1
                        resp = r["responde"]
                        break
                else:
                    resp = {"error": "sin regla"}
            self._send({"model": "mock", "message": {"role": "assistant", "content": resp if isinstance(resp, str) else json.dumps(resp)}, "done": True})

        def log_message(self, *x):
            pass

    print(f"mock ollama en {a.port}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
