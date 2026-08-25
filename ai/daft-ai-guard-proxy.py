#!/usr/bin/env python3
"""
daft-ai-guard-proxy.py — local AI proxy that enforces daft-ai-guard in front of
Ollama. Listens on 11435; forwards to Ollama on 127.0.0.1:11434.

Request prompts are scanned by daft-ai-guard.py: weaponized/illegal content is
blocked (HTTP 403) before reaching the model. Responses are passed through.
This is the safe-by-default path for the local model; raw Ollama (11434) stays
available for trusted use.
"""
import json
import os
import shutil
import subprocess
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

UPSTREAM = "http://127.0.0.1:11434"
GUARD = "/opt/daft/ai/daft-ai-guard.py"
LISTEN = ("127.0.0.1", 11435)


def _blocked(text: str) -> bool:
    if not os.path.exists(GUARD):
        return False
    p = subprocess.run(["python3", GUARD], input=text,
                       capture_output=True, text=True)
    return p.returncode == 2


def _extract_prompt(body: bytes):
    try:
        d = json.loads(body or b"{}")
    except Exception:
        return None
    if "prompt" in d:
        return str(d["prompt"])
    if "messages" in d and isinstance(d["messages"], list):
        for m in reversed(d["messages"]):
            c = m.get("content")
            if c:
                return str(c)
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if self.path.rstrip("/") in ("/api/generate", "/api/chat"):
            prompt = _extract_prompt(body)
            if prompt and _blocked(prompt):
                self.send_response(403)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(
                    {"error": "blocked by daft-ai-guard: weaponized/illegal content"}).encode())
                return
        self._proxy(body)

    def do_GET(self):
        self._proxy(b"")

    def _proxy(self, body: bytes):
        url = UPSTREAM + self.path
        req = urllib.request.Request(url, data=body or None,
                                     headers={k: v for k, v in self.headers.items()
                                              if k.lower() != "host"}, method=self.command)
        try:
            with urllib.request.urlopen(req, timeout=300) as resp:
                self.send_response(resp.status)
                for k, v in resp.getheaders():
                    if k.lower() not in ("transfer-encoding", "connection"):
                        self.send_header(k, v)
                self.end_headers()
                while True:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except Exception as e:  # upstream down or refused
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": f"upstream error: {e}"}).encode())


def main():
    if shutil.which("ollama") is None:
        print("ollama not found; proxy will return 502 until ollama is running")
    print(f"daft-ai-guard listening on http://{LISTEN[0]}:{LISTEN[1]} -> {UPSTREAM}")
    HTTPServer(LISTEN, Handler).serve_forever()


if __name__ == "__main__":
    main()
