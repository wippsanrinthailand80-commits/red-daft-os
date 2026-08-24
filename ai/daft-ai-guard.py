#!/usr/bin/env python3
"""
daft-ai-guard — DEFENSIVE content filter for the local AI proxy.

This is the *opposite* of a guardrail bypass. It inspects prompts/responses
and blocks clearly weaponized or illegal content (ready-to-run malware,
mass-exploitation, ransomware, non-disclosed 0day weaponization against
unpatched targets) while allowing standard security-research discussion.

Run as a reverse proxy in front of Ollama's API (port 11434).
"""
import re
import sys

# Pattern categories we refuse to forward (operational weaponization).
BLOCK_PATTERNS = [
    r"\b(ransomware|C2 ?beacon|botnet\s*loader)\b",
    r"\b(undisclosed|unpatched)\s+zero[- ]day\s+(exploit|weapon)",
    r"\b(mass\s*(scan|exploit)\s*of\s*(random|internet))\b",
    r"\b(obfuscated\s+malware\s+loader\s+for\s+deployment)\b",
]

ALLOW_CONTEXT = re.compile(r"\b(ctf|authorized|lab|own\s+software|reverse\s+engineer\s+(my|the\s+target))\b", re.I)


def is_blocked(text: str) -> bool:
    low = text.lower()
    for pat in BLOCK_PATTERNS:
        if re.search(pat, low, re.I):
            # Allow if explicitly framed as authorized/lab research.
            if ALLOW_CONTEXT.search(text):
                continue
            return True
    return False


def main() -> int:
    data = sys.stdin.read()
    if is_blocked(data):
        sys.stderr.write("[daft-ai-guard] blocked: weaponized/illegal content\n")
        return 2
    sys.stdout.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
