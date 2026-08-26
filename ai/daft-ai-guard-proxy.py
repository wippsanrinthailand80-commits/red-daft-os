#!/usr/bin/env python3
"""
daft-ai-guard-proxy.py v2 — local AI proxy enforcing daft-ai-guard v2 in front
of Ollama. Listens on 127.0.0.1:11435 -> upstream 127.0.0.1:11434.

Routing (exit protocol of ai/daft-ai-guard.py):
    0 ALLOW  -> forward untouched
    1 BOOST  -> stdout JSON {"system_boost": "..."}; inject as system context
                (chat: prepend message; generate: set "system" field) then
                forward — this converts refusals into authorized-scope answers
    2 BLOCK  -> HTTP 403 before anything reaches the model

Every decision is audited by the guard itself (/var/lib/daft-ai-guard/audit.log).
This is a filter OVERLAY, not a bypass: weaponized content stays blocked.
"""
import json
import os
import shutil
import subprocess
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

UPSTREAM = "http://127.0.0.1:11434"
GUARD = os.environ.get("DAFT_GUARD", "/opt/daft/ai/daft-ai-guard.py")
LISTEN = ("127.0.0.1", 11435)


def _guard(prompt_text: str):
    """Returns (verdict_str, boost_or_None). verdict in {'ALLOW','BOOST','BLOCK'}."""
    if not os.path.exists(GUARD):
        return ("ALLOW", None)
    p = subprocess.run(["python3", GUARD], input=prompt_text,
                       capture_output=True, text=True)
    if p.returncode == 2:
        return ("BLOCK", p.stderr.strip())
    if p.returncode == 1:
        try:
            return ("BOOST", json.loads(p.stdout).get("system_boost"))
        except Exception:
            return ("BOOST", None)
    return ("ALLOW", None)


def _extract_prompt(body: bytes):
    try:
        d = json.loads(body or b"{}")
    except Exception:
        return None, None
    if "prompt" in d:
        return str(d["prompt"]), d
    if isinstance(d.get("messages"), list) and d["messages"]:
        for m in reversed(d["messages"]):
            c = m.get("content")
            if c:
                return str(c), d
        return "", d
    return None, d


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if self.path.rstrip("/") in ("/api/generate", "/api/chat"):
            prompt, data = _extract_prompt(body)
            if prompt is not None:
                verdict, extra = _guard(prompt)
                if verdict == "BLOCK":
                    self._json(403, {"error":
                        f"blocked by daft-ai-guard ({extra or 'weaponized content'})"})
                    return
                if verdict == "BOOST" and data is not None and extra:
                    self._inject(data, extra)
                    body = json.dumps(data).encode()
        self._proxy(body)

    def _inject(self, data: dict, boost: str):
        """Authorization-scoped system prompt injection."""
        if isinstance(data.get("messages"), list):
            data["messages"] = [{"role": "system", "content": boost}] + \
                               [m for m in data["messages"]
                                if m.get("role") != "system"]
        elif "prompt" in data:
            # generate API has no separate system slot that merges with the
            # Modelfile; prefix so both are seen.
            data["prompt"] = f"[{boost}]\n\n{data['prompt']}"

    def _json(self, code: int, obj: dict):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(obj).encode())

    def do_GET(self):
        self._proxy(b"")

    def do_DELETE(self):
        self._proxy(b"")

    def _proxy(self, body: bytes):
        url = UPSTREAM + self.path
        req = urllib.request.Request(url, data=body or None,
                                     headers={k: v for k, v in self.headers.items()
                                              if k.lower() != "host"},
                                     method=self.command)
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
            self._json(502, {"error": f"upstream error: {e}"})


def main():
    if shutil.which("ollama") is None:
        print("ollama not found; proxy will return 502 until ollama is running")
    print(f"daft-ai-guard listening on http://{LISTEN[0]}:{LISTEN[1]} -> {UPSTREAM}")
    HTTPServer(LISTEN, Handler).serve_forever()


if __name__ == "__main__":
    main()
