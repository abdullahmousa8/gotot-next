"""make_miptex.py - diagnostic mip-colored texture (mip colorization technique).

Level 0-2: exact box-filtered checkerboard (white top-left, 32px cells).
Level 3-8: solid RED (255,0,0) - reveals which mip level the GPU samples:
  near geometry (levels 0-2) stays white/black, far geometry (level 3+)
  turns red. Deterministic, stdlib only.

Writes miptex_L0.rgba .. miptex_L8.rgba (consumed by tex_import --levels).
"""
import struct


def checker(x, y, cell=32):
    return (255, 255, 255) if ((x // cell) + (y // cell)) % 2 == 0 else (0, 0, 0)


def box_down(src, w, h):
    nw, nh = max(w // 2, 1), max(h // 2, 1)
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        for x in range(nw):
            acc = [0, 0, 0, 0]
            n = 0
            for dy in range(2):
                for dx in range(2):
                    sx, sy = x * 2 + dx, y * 2 + dy
                    if sx < w and sy < h:
                        o = (sy * w + sx) * 4
                        for c in range(4):
                            acc[c] += src[o + c]
                        n += 1
            o = (y * nw + x) * 4
            for c in range(4):
                out[o + c] = (acc[c] + n // 2) // n
    return bytes(out), nw, nh


def main():
    w = h = 256
    l0 = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = checker(x, y)
            l0 += bytes((r, g, b, 255))
    levels = [(bytes(l0), w, h)]
    for _ in range(8):
        blob, w, h = box_down(levels[-1][0], levels[-1][1], levels[-1][2])
        levels.append((blob, w, h))
    # Overwrite levels 3..8 with solid red (diagnostic mip color).
    out_levels = []
    for i, (blob, w, h) in enumerate(levels):
        if i >= 3:
            blob = bytes((255, 0, 0, 255)) * (w * h)
        out_levels.append((blob, w, h))
    for i, (blob, w, h) in enumerate(out_levels):
        with open(f"miptex_L{i}.rgba", "wb") as f:
            f.write(blob)
    # Sanity: L0 first texel white, L3 first texel red.
    assert out_levels[0][0][:4] == bytes((255, 255, 255, 255))
    assert out_levels[3][0][:4] == bytes((255, 0, 0, 255))
    print("wrote miptex_L0..L8.rgba (L0-L2 checker, L3+ solid red)")


if __name__ == "__main__":
    main()
