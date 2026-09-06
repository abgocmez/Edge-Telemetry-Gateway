#!/usr/bin/env python3
"""Draw the architecture diagram: the broadcast ring and why it exists.

The README already has a box-and-arrow sketch of the pipeline, and that sketch
says what the parts are called. It does not say what the interesting part
actually does, which is this: several producers claim cells in one ring, every
consumer reads the same cells through a cursor of its own, and a cursor that
falls a full lap behind is overwritten rather than waited for.

That is the whole design argument in one picture -- one copy for N readers, and
a slow consumer that costs the others nothing. A diagram that only showed boxes
would be decoration.

    ./scripts/diagram.py

Shares the SVG primitives and the validated palette with plot.py.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot import OUT, THEME, Svg  # noqa: E402

CELLS = 12
WRITE_AT = 9          # where producers are publishing
CURSORS = [(8, 0), (6, 0), (1, 1)]  # (cell, palette slot): the third is lapped


def box(s, x, y, w, h, t, label, sub=None, stroke=None, fill=None):
    s.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" '
          f'fill="{fill or t["surface"]}" stroke="{stroke or t["axis"]}" '
          f'stroke-width="1.5"/>')
    s.text(x + w / 2, y + (h / 2 + 1 if sub else h / 2 + 4), label, t["ink"], 12,
           anchor="middle", weight=600)
    if sub:
        s.text(x + w / 2, y + h / 2 + 15, sub, t["muted"], 10, anchor="middle")


def arrow(s, x1, y1, x2, y2, colour, width=1.5):
    s.line(x1, y1, x2, y2, colour, width)
    # A plain triangle head; markers would need defs and this needs one shape.
    dx, dy = x2 - x1, y2 - y1
    n = max((dx * dx + dy * dy) ** 0.5, 0.001)
    ux, uy = dx / n, dy / n
    px, py = -uy, ux
    s.add(f'<path d="M{x2:.1f},{y2:.1f} '
          f'L{x2 - ux * 7 + px * 3.5:.1f},{y2 - uy * 7 + py * 3.5:.1f} '
          f'L{x2 - ux * 7 - px * 3.5:.1f},{y2 - uy * 7 - py * 3.5:.1f} Z" '
          f'fill="{colour}"/>')


def diagram(t):
    w, h = 820, 386
    s = Svg(w, h, t)
    blue, orange, aqua = t["series"][0], t["series"][1], t["series"][2]

    s.text(24, 30, "One ring, one copy, a cursor per consumer", t["ink"], 16,
           weight=600)
    s.text(24, 50, "Why the gateway uses a broadcast ring rather than a queue "
                   "per consumer.", t["muted"], 12)

    # --- sources and ingest -------------------------------------------------
    sx, sw, sh = 24, 96, 40
    for i, (name, iface) in enumerate((("bus 0", "vcan0"), ("bus 1", "vcan1"))):
        y = 150 + i * 62
        box(s, sx, y, sw, sh, t, name, iface)
        arrow(s, sx + sw + 4, y + sh / 2, sx + sw + 52, 196, blue)
    s.text(sx + sw + 30, 250, "ingest", t["ink2"], 11, anchor="middle")
    s.text(sx + sw + 30, 264, "threads", t["ink2"], 11, anchor="middle")

    # --- the ring -----------------------------------------------------------
    rx, ry, rh = 186, 176, 42
    cw, gap = 30, 2          # a 2px surface gap keeps adjacent cells separable
    ring_w = CELLS * cw + (CELLS - 1) * gap

    s.text(rx, ry - 46, "broadcast ring", t["ink"], 12, weight=600)
    s.text(rx, ry - 30, "fixed cells, each with its own sequence number",
           t["muted"], 11)

    for i in range(CELLS):
        x = rx + i * (cw + gap)
        written = i <= WRITE_AT
        s.add(f'<rect x="{x}" y="{ry}" width="{cw}" height="{rh}" rx="3" '
              f'fill="{t["surface"]}" stroke="{blue if i == WRITE_AT else t["axis"]}" '
              f'stroke-width="{2 if i == WRITE_AT else 1.2}"/>')
        if written:
            s.add(f'<rect x="{x + 5}" y="{ry + 13}" width="{cw - 10}" '
                  f'height="16" rx="2" fill="{t["grid"]}"/>')

    # Producers claim the next cell with a CAS, which is the part a queue does
    # not need and this does.
    px = rx + WRITE_AT * (cw + gap) + cw / 2
    arrow(s, px, ry - 20, px, ry - 4, blue)
    s.text(px - 10, ry - 18, "producers claim the next cell with a CAS",
           t["ink2"], 11, anchor="end")

    # --- consumer cursors ---------------------------------------------------
    for cell, slot in CURSORS:
        cx = rx + cell * (cw + gap) + cw / 2
        colour = orange if slot else aqua
        arrow(s, cx, ry + rh + 30, cx, ry + rh + 6, colour)
        s.dot(cx, ry + rh + 36, 4, colour)

    s.text(rx, ry + rh + 62, "each consumer holds its own cursor: the frame is "
                             "stored once and read N times", t["ink2"], 11)
    s.text(rx, ry + rh + 80, "a cursor a full lap behind is overwritten — "
                             "drop-oldest, counted, and reported to that "
                             "consumer as a gap marker", t["muted"], 11)

    # --- consumers ----------------------------------------------------------
    ex = rx + ring_w + 52
    names = (("recorder", "appends to disk", 0), ("live view", "serves HTTP", 0),
             ("cadence", "falling behind", 1))
    for i, (name, sub, slot) in enumerate(names):
        y = 118 + i * 62
        colour = orange if slot else aqua
        arrow(s, rx + ring_w + 6, ry + rh / 2, ex - 6, y + 20, colour)
        box(s, ex, y, 118, 40, t, name, sub, stroke=colour)

    s.text(ex, 118 - 18, "separate containers, TCP", t["ink2"], 11)

    s.text(24, h - 18, "A queue per consumer would copy every frame once per "
                       "consumer, and order in a queue is push order — so ingest "
                       "could not be spread across threads at all.",
           t["muted"], 11)
    return s.render(
        "One ring, one copy, a cursor per consumer",
        "Architecture diagram. Two CAN buses feed ingest threads that claim "
        "cells in a shared broadcast ring with a compare-and-swap. Three "
        "consumers in separate containers each read the same cells through "
        "their own cursor over TCP; a cursor that falls a full lap behind is "
        "overwritten, which is counted as drop-oldest and reported to that "
        "consumer as a gap marker.")


def main():
    os.makedirs(OUT, exist_ok=True)
    for mode in ("light", "dark"):
        path = os.path.join(OUT, f"architecture-{mode}.svg")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(diagram(THEME[mode]))
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
