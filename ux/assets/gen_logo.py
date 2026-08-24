#!/usr/bin/env python3
"""Generate the Red Daft OS block ASCII banner (plain text, no color codes).
Color is applied at render time by the consuming scripts."""
import sys

# 5-row x 5-col glyphs for the banner font.
G = {
    'R': ["█████", "█   █", "█████", "█   █", "█   █"],
    'E': ["█████", "█    ", "████ ", "█    ", "█████"],
    'D': ["█████", "█   █", "█   █", "█   █", "█████"],
    'A': [" ███ ", "█   █", "█████", "█   █", "█   █"],
    'F': ["█████", "█    ", "████ ", "█    ", "█    "],
    'T': ["█████", "  █  ", "  █  ", "  █  ", "  █  "],
    'O': [" ███ ", "█   █", "█   █", "█   █", " ███ "],
    'S': [" ████", "█    ", " ███ ", "    █", "████"],
    ' ': ["     ", "     ", "     ", "     ", "     "],
}

TEXT = "RED DAFT OS"
rows = [""] * 5
for ch in TEXT:
    g = G[ch]
    for i in range(5):
        rows[i] += g[i] + " "

out = "\n".join(r.rstrip() for r in rows)
print(out)
if __name__ == "__main__":
    sys.exit(0)
