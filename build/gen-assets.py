#!/usr/bin/env python3
"""Generate Red Daft OS background images (crimson gradient) as PNGs.
Pure stdlib (no PIL/ImageMagick needed). Run at ISO build time on the host."""
import zlib, struct, os, sys


def _chunk(f, tag, data):
    f.write(struct.pack(">I", len(data)))
    f.write(tag)
    f.write(data)
    f.write(struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, w, h, row_fn):
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        _chunk(f, b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        raw = bytearray()
        for y in range(h):
            raw.append(0)  # filter: none
            for x in range(w):
                raw += bytes(row_fn(x, y, w, h))
        _chunk(f, b"IDAT", zlib.compress(bytes(raw), 9))
        _chunk(f, b"IEND", b"")


def _gradient(x, y, w, h):
    t = y / max(1, h - 1)
    r = int(0xFF * (1 - t) + 0x0C * t)
    g = int(0x00 * (1 - t) + 0x0C * t)
    b = int(0x33 * (1 - t) + 0x0C * t)
    cx, cy = w / 2.0, h / 2.0
    d = (((x - cx) / w) ** 2 + ((y - cy) / h) ** 2) ** 0.5
    glow = max(0.0, 1 - d * 1.6) * 36
    r = min(255, r + int(glow * 0.85))
    g = min(255, g + int(glow * 0.12))
    b = min(255, b + int(glow))
    return (r, g, b)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/assets"
    os.makedirs(out, exist_ok=True)
    write_png(os.path.join(out, "reddaft-bg.png"), 640, 480, _gradient)
    write_png(os.path.join(out, "reddaft-wallpaper.png"), 1280, 720, _gradient)
    print("generated:", os.path.join(out, "reddaft-bg.png"),
          os.path.join(out, "reddaft-wallpaper.png"))


if __name__ == "__main__":
    main()
