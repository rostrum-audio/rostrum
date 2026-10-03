#!/usr/bin/env python3
"""Writes Rostrum's icons into data/icons/.

The mark is an R drawn by three parallel lines, like bus lines on a transit map: Rostrum's
mix buses running side by side. The centre line is a turtle program (forward / turn), and
each line is that path offset sideways, written as exact lines and arcs.
"""
import math
import pathlib

ICONS = pathlib.Path(__file__).resolve().parents[2] / "data" / "icons"
APP_ID = "dev.getrostrum.Rostrum"

INK = "#14151C"
INK_TOP = "#262838"
CORAL = "#FF4F5E"
AMBER = "#FFB43C"
TEAL = "#22D3B6"

LINES = ((-8, TEAL), (0, AMBER), (8, CORAL))
# Up the stem, over the shoulder, round the bowl, and out along the leg.
PROGRAM = (("f", 50), ("t", 90, 13), ("f", 6), ("t", 135, 20), ("t", -90, 12), ("f", 22))


def offset_path(x, y, program, d):
    """The program from (x, y) heading up, offset d to the left of travel."""
    h = -math.pi / 2

    def left(hh):
        return math.sin(hh), -math.cos(hh)

    lx, ly = left(h)
    out = [f"M{x + lx * d:.2f} {y + ly * d:.2f}"]
    for op in program:
        if op[0] == "f":
            x += math.cos(h) * op[1]
            y += math.sin(h) * op[1]
            lx, ly = left(h)
            out.append(f"L{x + lx * d:.2f} {y + ly * d:.2f}")
            continue
        deg, r = op[1], op[2]
        cw = deg > 0
        lx, ly = left(h)
        cx, cy = (x - lx * r, y - ly * r) if cw else (x + lx * r, y + ly * r)
        h += math.radians(deg)
        lx, ly = left(h)
        x, y = (cx + lx * r, cy + ly * r) if cw else (cx - lx * r, cy - ly * r)
        rr = r + d if cw else r - d
        out.append(f"A{rr:.2f} {rr:.2f} 0 0 {1 if cw else 0} {x + lx * d:.2f} {y + ly * d:.2f}")
    return " ".join(out)


def mark(x, y, width, scale=1.0, lines=LINES):
    prog = [(op[0], op[1] * scale) if op[0] == "f" else (op[0], op[1], op[2] * scale) for op in PROGRAM]
    paths = "\n".join(f'    <path d="{offset_path(x, y, prog, d * scale)}" stroke="{col}"/>' for d, col in lines)
    return (f'  <g fill="none" stroke-width="{width:g}" stroke-linecap="round" stroke-linejoin="round">\n'
            f"{paths}\n  </g>\n")


def tile(size, rx, inset):
    w = size - 2 * inset
    return ("  <defs>\n"
            '    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">\n'
            f'      <stop offset="0" stop-color="{INK_TOP}"/>\n'
            f'      <stop offset="1" stop-color="{INK}"/>\n'
            "    </linearGradient>\n"
            "  </defs>\n"
            f'  <rect x="{inset:g}" y="{inset:g}" width="{w:g}" height="{w:g}" rx="{rx:g}" fill="url(#bg)"/>\n')


def svg(size, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
            f'viewBox="0 0 {size} {size}">\n{body}</svg>\n')


def app_icon():
    return svg(128, tile(128, 28, 8) + mark(45, 101, 6))


def small_icon(size):
    """Hand-tuned for menus: the mark fills more of the tile; at 16 px two lines stay apart."""
    s = size / 128
    if size <= 16:
        return svg(size, tile(size, 3.5, 0) + mark(45 * s - 0.5, 101 * s + 0.5, 1.3, s * 1.12,
                                                   ((-7, TEAL), (7, CORAL))))
    return svg(size, tile(size, 3.5 * size / 16, 0) + mark(45 * s, 101 * s + 0.5, 1.4, s * 1.12))


MUTED_BADGE = """  <!-- Muted badge: a slashed mic, so the state is a shape and not only a colour. -->
  <circle cx="98" cy="98" r="28" fill="#da4453" stroke="#fcfcfc" stroke-width="4"/>
  <rect x="91" y="78" width="14" height="24" rx="7" fill="#fcfcfc"/>
  <path d="M84 96 a14 14 0 0 0 28 0" fill="none" stroke="#fcfcfc" stroke-width="4" stroke-linecap="round"/>
  <line x1="98" y1="110" x2="98" y2="117" stroke="#fcfcfc" stroke-width="4" stroke-linecap="round"/>
  <line x1="80" y1="80" x2="116" y2="116" stroke="#fcfcfc" stroke-width="7" stroke-linecap="round"/>
  <line x1="80" y1="80" x2="116" y2="116" stroke="#da4453" stroke-width="3" stroke-linecap="round"/>
"""


def tray_icon(muted):
    """No tile: the coloured lines read on light and dark panels alike."""
    return svg(128, mark(42, 106, 8, 1.12) + (MUTED_BADGE if muted else ""))


def main():
    files = {
        f"{APP_ID}.svg": app_icon(),
        f"16x16/{APP_ID}.svg": small_icon(16),
        f"22x22/{APP_ID}.svg": small_icon(22),
        f"{APP_ID}-tray.svg": tray_icon(False),
        f"{APP_ID}-tray-muted.svg": tray_icon(True),
    }
    for name, text in files.items():
        path = ICONS / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        print(path.relative_to(ICONS.parents[1]))


if __name__ == "__main__":
    main()
