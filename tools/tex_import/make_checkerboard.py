"""make_checkerboard.py - procedural deterministic checkerboard (stdlib only).

Writes BOTH:
  checkerboard.png  - human-readable source (RGB, 256x256, 32px cells)
  checkerboard.rgba - exact raw bytes consumed by tex_import (no PNG decoder
                      needed in the offline tool: zero new dependencies).

Top-left cell is WHITE so first texel == (255,255,255,255).
"""
import struct
import zlib


def checker_pixel(x, y, cell=32):
    return (255, 255, 255) if ((x // cell) + (y // cell)) % 2 == 0 else (0, 0, 0)


def png_chunk(ctype, data):
    c = struct.pack(">I", len(data)) + ctype + data
    c += struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)
    return c


def write_png(path, w, h):
    raw = b"".join(
        b"\x00" + b"".join(bytes(checker_pixel(x, y)) for x in range(w))
        for y in range(h)
    )
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def write_rgba(path, w, h):
    with open(path, "wb") as f:
        for y in range(h):
            for x in range(w):
                r, g, b = checker_pixel(x, y)
                f.write(bytes((r, g, b, 255)))


if __name__ == "__main__":
    write_png("checkerboard.png", 256, 256)
    write_rgba("checkerboard.rgba", 256, 256)
    print("wrote checkerboard.png + checkerboard.rgba (256x256, top-left WHITE)")
