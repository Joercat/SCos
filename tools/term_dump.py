#!/usr/bin/env python3
"""Decode SCos terminal screenshots back into text.

The kernel renders with the generated 8x16 bitmap font (kernel/src/font_data.c)
at a 18px line pitch, so every 8x16 cell of a terminal screenshot can be
matched against the font table exactly. Usage:

    python3 tools/term_dump.py <ppm> <content_x> <content_y> [cols] [rows]

content_x/content_y is the screen position of the terminal content origin
(window x+1+6, window y+TITLEBAR+4 in wm coordinates).
"""
import re
import sys


def load_font():
    src = open("kernel/src/font_data.c").read()
    pat = re.compile(r"(?:0x[0-9a-fA-F]{2},\s*){15}0x[0-9a-fA-F]{2}")
    glyphs = []
    for m in pat.finditer(src):
        vals = [int(v, 16) for v in m.group(0).split(",")]
        glyphs.append(vals)
    return glyphs


def load_ppm(path):
    data = open(path, "rb").read()
    # P6 header: magic, dims, maxval
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    return w, h, px


def main():
    path = sys.argv[1]
    ox, oy = int(sys.argv[2]), int(sys.argv[3])
    cols = int(sys.argv[4]) if len(sys.argv) > 4 else 86
    rows = int(sys.argv[5]) if len(sys.argv) > 5 else 23
    font = load_font()
    w, h, px = load_ppm(path)

    def lum(x, y):
        if x < 0 or y < 0 or x >= w or y >= h:
            return 0
        i = (y * w + x) * 3
        return px[i] * 3 + px[i + 1] * 6 + px[i + 2]

    out = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            cx, cy = ox + c * 8, oy + r * 18
            cell = []
            for row in range(16):
                bits = 0
                for bit in range(8):
                    if lum(cx + bit, cy + row) > 240:
                        bits |= 0x80 >> bit
                cell.append(bits)
            ch = None
            if not any(cell):
                ch = " "
            else:
                for cand in range(33, 127):
                    if font[cand] == cell:
                        ch = chr(cand)
                        break
            if ch is None:
                bestd, ch = 10 ** 9, "?"
                for cand in range(33, 127):
                    d = 0
                    g = font[cand]
                    for row in range(16):
                        d += bin(g[row] ^ cell[row]).count("1")
                    if d < bestd:
                        bestd, ch = d, chr(cand)
                if bestd > 20:
                    ch = "?"
            line += ch
        out.append(line.rstrip())
    print("\n".join(out))


main()
