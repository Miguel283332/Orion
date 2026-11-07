#!/usr/bin/env python3
"""Servidor Ollama simulado para pruebas (Python y C++): mismo protocolo HTTP que Ollama.

  python tests/mock_ollama.py --port 11555 --script respuestas.json [--log peticiones.jsonl]

`respuestas.json` es una lista de acciones del agente (objetos {"thought","tool","args"}); se sirven en
orden a las peticiones del bucle principal. Los módulos de reflexión/curiosidad/resumen se responden solos.
Cuando se agotan, responde `done`.
"""
import argparse, hashlib, json, math, re, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def hash_embed(text, dim=256):
    toks = re.findall(r"\w+", text.lower())
    vec = [0.0] * dim
    for f in toks + [a + "_" + b for a, b in zip(toks, toks[1:])]:
        h = int(hashlib.md5(f.encode("utf-8")).hexdigest(), 16)
        vec[h % dim] += 1.0 if (h >> 20) & 1 else -1.0
    n = math.sqrt(sum(v * v for v in vec)) or 1.0
    return [v / n for v in vec]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=11555)
    ap.add_argument("--script", default=None)
    ap.add_argument("--log", default=None)
    ap.add_argument("--upload-dir", default=None, help="donde guardar lo recibido en PUT/POST /web/upload")
    a = ap.parse_args()
    replies = json.load(open(a.script, encoding="utf-8")) if a.script else []
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
            if self.path.startswith("/web/"):
                return self._web()
            self._send({"models": [{"name": "mock:latest"}, {"name": "nomic-embed-text:latest"}]})

        def _web(self):
            """Mini servidor web para probar web_get / download / upload."""
            page = (b"<html><head><title>T</title><style>.x{}</style></head><body><h1>Titulo</h1><script>var s=1;</script>"
                    b"<p>Hola <b>mundo</b></p><a href='/web/otra'>enlace uno</a></body></html>")
            if self.path == "/web/redir":
                self.send_response(302)
                self.send_header("Location", "/web/page")
                self.end_headers()
                return
            if self.path == "/web/page":
                body, ctype = page, "text/html; charset=utf-8"
            elif self.path == "/web/big":
                body, ctype = bytes(3_000_000), "application/octet-stream"
            elif self.path == "/web/blob":
                body, ctype = bytes(range(256)) * 400, "application/octet-stream"
            elif self.path == "/web/private":   # redirige a una IP privada (debe bloquearse)
                self.send_response(302)
                self.send_header("Location", "http://10.0.0.1/secreto")
                self.end_headers()
                return
            else:
                body, ctype = b"no encontrado", "text/plain"
            self.send_response(200 if body != b"no encontrado" else 404)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def _upload(self):
            data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if a.upload_dir:
                import os
                os.makedirs(a.upload_dir, exist_ok=True)
                open(os.path.join(a.upload_dir, "last_upload.bin"), "wb").write(data)
                json.dump({"method": self.command, "path": self.path, "headers": dict(self.headers.items()), "length": len(data)},
                          open(os.path.join(a.upload_dir, "last_upload.json"), "w"))
            b = b"recibido"
            self.send_response(201)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_PUT(self):
            self._upload()

        def do_POST(self):
            if self.path.startswith("/web/"):
                return self._upload()
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            if a.log:
                with lock, open(a.log, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"path": self.path, "body": body}, ensure_ascii=False) + "\n")
            if self.path == "/api/embed":
                return self._send({"embeddings": [hash_embed(x) for x in body["input"]]})
            if self.path != "/api/chat":
                return self._send({"error": "no encontrado"}, 404)
            sysm = body["messages"][0]["content"]
            if "reflection module" in sysm:
                c = json.dumps({"success": True, "score": 0.9, "skill": None, "lessons": ["Verificar el resultado con una herramienta evita errores."]})
            elif "curiosity module" in sysm:
                c = json.dumps({"goal": "Ordenar mi memoria y resumir lo aprendido."})
            elif "final-answer module" in sysm:
                c = "Respuesta de emergencia."
            elif "rule-distillation module" in sysm:
                c = json.dumps({"rules": ["Si una búsqueda falla dos veces, respondo con lo que sé."]})
            elif sysm.startswith("Resume en"):
                c = "Resumen de prueba."
            else:
                with lock:
                    r = replies.pop(0) if replies else {"thought": "fin", "tool": "done", "args": {"summary": "fin"}}
                c = r if isinstance(r, str) else json.dumps(r)
            self._send({"model": "mock", "message": {"role": "assistant", "content": c}, "done": True})

        def log_message(self, *x):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    print(f"mock ollama en {a.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
