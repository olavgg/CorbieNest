#!/usr/bin/env python3
"""Turn a captured terminal transcript into an SVG, for the README.

GitHub's markdown renders neither ANSI escapes nor CSS (its sanitiser drops
`style`), so the coloured examples in README.md are SVGs: an image carries its
own fills and survives the sanitiser. The .ansi files next to the .svg ones are
the source — `cat docs/demo.ansi` shows the same thing in a terminal.

    tools/ansi2svg.py docs/demo.ansi docs/demo.svg

Understands the escapes corbienest itself emits (see the C_* macros in
src/common.h): reset, bold, dim, the 30-37/90-97 colours and 38;5;N.
"""
import io
import re
import sys

# xterm's 16 base colours, in the shades GitHub's dark theme uses, so the image
# sits naturally in a README rather than glowing at it.
BASE = {
    30: "#484f58", 31: "#ff7b72", 32: "#3fb950", 33: "#d29922",
    34: "#58a6ff", 35: "#bc8cff", 36: "#39c5cf", 37: "#b1bac4",
    90: "#8b949e", 91: "#ffa198", 92: "#56d364", 93: "#e3b341",
    94: "#79c0ff", 95: "#d2a8ff", 96: "#56d4dd", 97: "#f0f6fc",
}
FG, BG, BORDER = "#c9d1d9", "#0d1117", "#30363d"

FONT = ("ui-monospace, SFMono-Regular, 'SF Mono', Menlo, Consolas, "
        "'DejaVu Sans Mono', 'Liberation Mono', monospace")
SIZE, CELL, LINE, PAD = 14.0, 8.4, 20.0, 18.0   # px: font, advance, leading, padding


def cube(n):
    """xterm 256-colour index -> #rrggbb."""
    if n < 16:
        return BASE.get(n if n < 8 else n + 82, FG)
    if n < 232:
        n -= 16
        steps = [0, 95, 135, 175, 215, 255]
        return "#%02x%02x%02x" % (steps[n // 36], steps[n // 6 % 6], steps[n % 6])
    v = 8 + (n - 232) * 10
    return "#%02x%02x%02x" % (v, v, v)


def runs(line):
    """Split one line into (text, colour, bold, dim) runs."""
    out, col, bold, dim, pos = [], None, False, False, 0
    for m in re.finditer(r"\x1b\[([0-9;]*)m", line):
        if m.start() > pos:
            out.append((line[pos:m.start()], col, bold, dim))
        params = [int(p) for p in m.group(1).split(";") if p != ""] or [0]
        i = 0
        while i < len(params):
            p = params[i]
            if p == 0:
                col, bold, dim = None, False, False
            elif p == 1:
                bold = True
            elif p == 2:
                dim = True
            elif p in BASE:
                col = BASE[p]
            elif p == 38 and params[i + 1:i + 2] == [5]:
                col = cube(params[i + 2])
                i += 2
            i += 1
        pos = m.end()
    if pos < len(line):
        out.append((line[pos:], col, bold, dim))
    return out


def width(text):
    """Display cells, near enough for sizing the canvas."""
    w = 0
    for ch in text:
        o = ord(ch)
        if o in (0x200d, 0xfe0f) or 0x300 <= o <= 0x36f:
            continue          # ZWJ / variation selector / combining marks
        w += 2 if o >= 0x1f300 else 1
    return w


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def render(text):
    lines = text.rstrip("\n").split("\n")
    parsed = [runs(l) for l in lines]
    cols = max((sum(width(t) for t, _, _, _ in r) for r in parsed), default=0)
    w = cols * CELL + 2 * PAD
    h = len(lines) * LINE + 2 * PAD

    o = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%.0f" height="%.0f" '
        'viewBox="0 0 %.0f %.0f" font-family="%s" font-size="%g">'
        % (w, h, w, h, FONT, SIZE),
        '<rect width="%.0f" height="%.0f" rx="8" fill="%s" stroke="%s"/>' % (w, h, BG, BORDER),
    ]
    for i, r in enumerate(parsed):
        y = PAD + SIZE + i * LINE
        o.append('<text y="%g" xml:space="preserve" fill="%s">' % (y, FG))
        # Symbols like ● ⎿ ↳ ⏵ are drawn wider than a monospace cell by whatever
        # font the viewer falls back to, which would shift the rest of the line.
        # Pinning every run to its own column keeps the grid honest instead.
        cell = 0
        for txt, col, bold, dim in r:
            if not txt:
                continue
            a = ' x="%g"' % (PAD + cell * CELL)
            if col:
                a += ' fill="%s"' % col
            if bold:
                a += ' font-weight="600"'
            if dim:
                a += ' fill-opacity="0.62"'
            o.append("<tspan%s>%s</tspan>" % (a, esc(txt)))
            cell += width(txt)
        o.append("</text>")
    o.append("</svg>")
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = io.open(sys.argv[1], encoding="utf-8").read()
    io.open(sys.argv[2], "w", encoding="utf-8").write(render(src))
