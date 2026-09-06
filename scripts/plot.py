#!/usr/bin/env python3
"""Draw the charts in the README from the committed measurement summaries.

No plotting library and no dependencies: the input is a handful of numbers and
the output is SVG, so pulling in a toolchain to draw four charts would be a
worse trade than writing the axes. It also means the charts regenerate anywhere
the repository is checked out, which is the same property the measurement
scripts have and for the same reason.

Every value comes from results/<host>-<date>/<experiment>/summary.txt. Nothing
here is typed in by hand, so a re-measurement changes the pictures by re-running
this, and a chart can never drift away from the numbers in the text beside it.

    ./scripts/plot.py

Writes light and dark variants to docs/images/, which the README selects
between with <picture>.
"""

import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BEFORE = os.path.join(ROOT, "results", "ADMIN-20260905")
AFTER = os.path.join(ROOT, "results", "ADMIN-20260906")
OUT = os.path.join(ROOT, "docs", "images")

# From the data-viz reference palette, validated for both surfaces before use.
# Adjacent-pair CVD dE 9.1 light / 8.4 dark against a >=8 target; aqua and
# yellow sit under 3:1 on the light surface, which obliges the direct labels
# every series here carries anyway.
THEME = {
    "light": {
        "surface": "#fcfcfb",
        "ink": "#0b0b0b",
        "ink2": "#52514e",
        "muted": "#898781",
        "grid": "#e1e0d9",
        "axis": "#c3c2b7",
        "series": ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"],
    },
    "dark": {
        "surface": "#1a1a19",
        "ink": "#ffffff",
        "ink2": "#c3c2b7",
        "muted": "#898781",
        "grid": "#2c2c2a",
        "axis": "#383835",
        "series": ["#3987e5", "#d95926", "#199e70", "#c98500"],
    },
}

FONT = ("system-ui,-apple-system,'Segoe UI',Roboto,'Helvetica Neue',"
        "Arial,sans-serif")


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


class Svg:
    """Just enough SVG to draw an axis and some marks."""

    def __init__(self, w, h, t):
        self.w, self.h, self.t = w, h, t
        self.parts = []

    def add(self, s):
        self.parts.append(s)

    def line(self, x1, y1, x2, y2, stroke, width=1, dash=None, cap="round"):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" '
                 f'y2="{y2:.1f}" stroke="{stroke}" stroke-width="{width}" '
                 f'stroke-linecap="{cap}"{d}/>')

    def path(self, pts, stroke, width=2):
        d = " ".join(("M" if i == 0 else "L") + f"{x:.1f},{y:.1f}"
                     for i, (x, y) in enumerate(pts))
        self.add(f'<path d="{d}" fill="none" stroke="{stroke}" '
                 f'stroke-width="{width}" stroke-linejoin="round" '
                 f'stroke-linecap="round"/>')

    def dot(self, x, y, r, fill, ring=None):
        # A 2px surface ring keeps overlapping marks separable, which matters
        # most exactly where lines converge.
        if ring:
            self.add(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r + 2:.1f}" '
                     f'fill="{ring}"/>')
        self.add(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" '
                 f'fill="{fill}"/>')

    def text(self, x, y, s, fill, size=12, anchor="start", weight=400):
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" '
                 f'font-family="{FONT}" font-size="{size}" '
                 f'font-weight="{weight}" text-anchor="{anchor}">{esc(s)}</text>')

    def render(self, title, desc):
        c = self.t
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" '
                f'height="{self.h}" viewBox="0 0 {self.w} {self.h}" '
                f'role="img" aria-label="{esc(desc)}">'
                f"<title>{esc(title)}</title><desc>{esc(desc)}</desc>"
                f'<rect width="{self.w}" height="{self.h}" fill="{c["surface"]}"/>'
                + "".join(self.parts) + "</svg>\n")


def log_scale(lo, hi, px0, px1):
    """Map a value onto pixels logarithmically. Used wherever the data spans
    orders of magnitude, which is most of it: a linear axis showing 73 ms and
    228 us on the same picture shows one of them."""
    a, b = math.log10(lo), math.log10(hi)

    def f(v):
        v = max(v, lo)
        return px0 + (math.log10(v) - a) / (b - a) * (px1 - px0)

    return f


def decades(lo, hi):
    out, d = [], 10 ** math.floor(math.log10(lo))
    while d <= hi * 1.001:
        if d >= lo * 0.999:
            out.append(d)
        d *= 10
    return out


def spread(ys, gap=15):
    """Push direct labels apart so they stay readable where the series they
    name have converged. Convergence is usually the finding -- two arms landing
    on the same number is the whole point of one chart here -- so the labels
    have to survive it."""
    order = sorted(range(len(ys)), key=lambda i: ys[i])
    out = list(ys)
    for k in range(1, len(order)):
        a, b = order[k - 1], order[k]
        if out[b] - out[a] < gap:
            out[b] = out[a] + gap
    return out


def fmt_us(v):
    if v >= 1000:
        return f"{v / 1000:.0f} ms" if v >= 10000 else f"{v / 1000:.1f} ms"
    return f"{v:.0f} µs" if v >= 10 else f"{v:.1f} µs"


# ------------------------------------------------------------------ parsing ---

def read_latency(path):
    """topology -> rate -> [p50, p90, p99, p99.9, max], microseconds."""
    out = {}
    with open(os.path.join(path, "latency", "summary.txt")) as f:
        for line in f:
            m = re.match(r"^(ring|queue)\s+(\d+)\s+(.*)$", line.strip())
            if m:
                vals = [float(x) for x in re.findall(r"([\d.]+)us", m.group(3))]
                out.setdefault(m.group(1), {})[int(m.group(2))] = vals
    return out


def read_linger(path):
    """[(linger_us, p50, p99, writes_per_s)]."""
    rows = []
    with open(os.path.join(path, "linger", "summary.txt")) as f:
        for line in f:
            m = re.match(r"^(\d+)us\s+([\d.]+)\s+([\d.]+)us\s+([\d.]+)us\s+(\d+)",
                         line.strip())
            if m:
                rows.append((int(m.group(1)), float(m.group(3)),
                             float(m.group(4)), int(m.group(5))))
    return rows


def read_sched(path):
    """arm -> [p50, p90, p99, p99.9]."""
    out = {}
    with open(os.path.join(path, "sched", "summary.txt")) as f:
        for line in f:
            m = re.match(r"^(idle-other|loaded-other|loaded-fifo|loaded-fifo-mlock)"
                         r"\s+(.*)$", line.strip())
            if m:
                out[m.group(1)] = [float(x) for x in m.group(2).split()][:4]
    return out


# ------------------------------------------------------------------- charts ---

def chart_correction(t):
    """Before and after the instrument fix, per measurement. A dumbbell,
    because the reader's job is to see the distance one item moved -- and the
    emphasis pattern, grey for the discredited numbers, colour for the ones
    that stand."""
    before, after = read_latency(BEFORE)["ring"], read_latency(AFTER)["ring"]
    idx = {"p99": 2, "p99.9": 3, "max": 4}
    rows = [(f"{r:,}/s  {p}".replace(",", " "), before[r][i], after[r][i])
            for r in (2000, 20000, 100000) for p, i in idx.items()]

    w, h = 720, 78 + len(rows) * 30 + 44
    s = Svg(w, h, t)
    left, right, top = 168, w - 92, 76
    lo = min(min(b, a) for _, b, a in rows) * 0.7
    hi = max(max(b, a) for _, b, a in rows) * 1.4
    x = log_scale(lo, hi, left, right)

    s.text(20, 26, "Every tail figure, re-measured", t["ink"], 15, weight=600)
    s.text(20, 44, "Ring topology, same board and load. Log scale.",
           t["muted"], 12)
    # The 2 000/s rows do not fit the headline and are not hidden for it.
    s.text(20, 61, "The 2 000/s rows barely move, and p99 moves the wrong way: "
                   "too few samples there for the bug to bite.", t["muted"], 11)

    ly = 40
    s.dot(right - 96, ly, 5, t["muted"])
    s.text(right - 86, ly + 4, "before", t["ink2"], 11)
    s.dot(right - 34, ly, 5, t["series"][0])
    s.text(right - 24, ly + 4, "after", t["ink2"], 11)

    for d in decades(lo, hi):
        s.line(x(d), top - 8, x(d), h - 34, t["grid"], 1)
        s.text(x(d), h - 18, fmt_us(d), t["muted"], 11, anchor="middle")

    for i, (label, b, a) in enumerate(rows):
        y = top + 12 + i * 30
        s.text(left - 14, y + 4, label, t["ink2"], 12, anchor="end")
        s.line(x(min(b, a)), y, x(max(b, a)), y, t["axis"], 2)
        s.dot(x(b), y, 5, t["muted"], ring=t["surface"])
        s.dot(x(a), y, 5, t["series"][0], ring=t["surface"])
        s.text(right + 12, y + 4, fmt_us(a), t["ink"], 11, weight=600)

    return s.render(
        "Every tail figure, re-measured",
        "Dumbbell chart. Every tail figure fell once the probe stopped sorting "
        "its own sample buffer inside the loop it was measuring.")


def chart_latency(t):
    """Latency against offered rate. Four percentiles, so categorical, and
    every one direct-labelled."""
    data = read_latency(AFTER)["ring"]
    rates = sorted(data)
    names = ["p50", "p90", "p99", "p99.9"]

    w, h = 660, 400
    s = Svg(w, h, t)
    left, right, top, bot = 76, w - 96, 74, h - 58
    x = log_scale(rates[0], rates[-1], left, right)
    lo, hi = 40, 2000
    y = log_scale(lo, hi, bot, top)

    s.text(20, 28, "Latency against offered rate", t["ink"], 15, weight=600)
    s.text(20, 46, "Raspberry Pi 3 B+, ring topology, one consumer. Both axes "
                   "logarithmic.", t["muted"], 12)

    for d in decades(lo, hi):
        s.line(left, y(d), right, y(d), t["grid"], 1)
        s.text(left - 12, y(d) + 4, fmt_us(d), t["muted"], 11, anchor="end")
    s.line(left, bot, right, bot, t["axis"], 1)
    for r in rates:
        s.text(x(r), bot + 22, f"{r:,}".replace(",", " ") + "/s",
               t["ink2"], 12, anchor="middle")

    ends = spread([y(data[rates[-1]][i]) for i in range(len(names))])
    for i, name in enumerate(names):
        col = t["series"][i]
        pts = [(x(r), y(data[r][i])) for r in rates]
        s.path(pts, col, 2)
        for px, py in pts:
            s.dot(px, py, 4.5, col, ring=t["surface"])
        s.dot(right + 14, ends[i], 4.5, col)
        s.text(right + 24, ends[i] + 4, name, t["ink2"], 12)

    s.text(20, h - 18,
           "p50 and p90 fall as the board wakes up, then hold. Only p99.9 lifts "
           "at 100 000/s, and there it is queueing.", t["muted"], 11)
    return s.render(
        "Latency against offered rate",
        "Log-log line chart of p50, p90, p99 and p99.9 latency at 2 000, "
        "20 000 and 100 000 frames per second.")


def chart_batching(t):
    """Two panels rather than two y-axes. Latency and syscall rate are
    different units, and putting them on one frame with two scales is the
    fastest way to make a trade-off look like whatever the author wanted."""
    rows = read_linger(AFTER)
    xs = [r[0] for r in rows]

    w, h = 660, 430
    s = Svg(w, h, t)
    left, right = 82, w - 96
    x = log_scale(max(xs[0], 50), xs[-1], left, right)

    def xpos(v):
        return x(max(v, 50))

    s.text(20, 28, "What waiting to coalesce buys, and what it costs",
           t["ink"], 15, weight=600)
    s.text(20, 46, "20 000 frames/s. The linger axis is logarithmic; 0 is drawn "
                   "at the left edge.", t["muted"], 12)

    # Panel 1: latency, two series.
    t1, b1 = 76, 222
    lo1, hi1 = 50, 10000
    y1 = log_scale(lo1, hi1, b1, t1)
    lab = []
    for d in decades(lo1, hi1):
        s.line(left, y1(d), right, y1(d), t["grid"], 1)
        s.text(left - 12, y1(d) + 4, fmt_us(d), t["muted"], 11, anchor="end")
    for i, (name, col_i) in enumerate((("p50", 0), ("p99", 1))):
        col = t["series"][col_i]
        pts = [(xpos(r[0]), y1(r[1 + i])) for r in rows]
        s.path(pts, col, 2)
        for px, py in pts:
            s.dot(px, py, 4, col, ring=t["surface"])
        lab.append((pts[-1][1], col, name))
    for ly, col, name in zip(spread([v[0] for v in lab]), *zip(*[(c, n) for _, c, n in lab])):
        s.dot(right + 14, ly, 4.5, col)
        s.text(right + 24, ly + 4, name, t["ink2"], 12)
    s.text(left, t1 - 14, "consumer latency", t["ink2"], 12, weight=600)

    # Panel 2: write syscalls, one series, so no legend is owed.
    t2, b2 = 274, 366
    lo2, hi2 = 100, 20000
    y2 = log_scale(lo2, hi2, b2, t2)
    for d in decades(lo2, hi2):
        s.line(left, y2(d), right, y2(d), t["grid"], 1)
        s.text(left - 12, y2(d) + 4, f"{d:,}".replace(",", " "),
               t["muted"], 11, anchor="end")
    s.line(left, b2, right, b2, t["axis"], 1)
    pts = [(xpos(r[0]), y2(r[3])) for r in rows]
    s.path(pts, t["series"][2], 2)
    for px, py in pts:
        s.dot(px, py, 4, t["series"][2], ring=t["surface"])
    s.text(left, t2 - 14, "write syscalls per second", t["ink2"], 12,
           weight=600)

    for r in rows:
        s.text(xpos(r[0]), b2 + 22, "0" if r[0] == 0 else f"{r[0]}",
               t["ink2"], 11, anchor="middle")
    s.text((left + right) / 2, b2 + 40, "linger, microseconds", t["muted"], 11,
           anchor="middle")
    s.text(20, h - 12, "A straight trade. An earlier version of this chart "
                       "turned over, and that was the instrument, not the board.",
           t["muted"], 11)
    return s.render(
        "What waiting to coalesce buys, and what it costs",
        "Two stacked panels sharing a logarithmic linger axis: consumer "
        "latency p50 and p99 above, write syscalls per second below.")


def chart_sched(t):
    """Four arms across four percentiles. The shape is the argument: three
    lines stay flat and one lifts away."""
    data = read_sched(AFTER)
    arms = [("loaded-other", "contended"), ("idle-other", "idle"),
            ("loaded-fifo", "contended, SCHED_FIFO"),
            ("loaded-fifo-mlock", "+ mlockall")]
    names = ["p50", "p90", "p99", "p99.9"]

    w, h = 660, 400
    s = Svg(w, h, t)
    left, right, top, bot = 76, w - 210, 78, h - 58
    lo, hi = 100, 10000
    y = log_scale(lo, hi, bot, top)
    xs = [left + i * (right - left) / (len(names) - 1) for i in range(len(names))]

    s.text(20, 28, "One busy loop per core, and what priority does about it",
           t["ink"], 15, weight=600)
    s.text(20, 46, "20 000 frames/s on the ring. No frames were lost in any arm.",
           t["muted"], 12)

    for d in decades(lo, hi):
        s.line(left, y(d), right, y(d), t["grid"], 1)
        s.text(left - 12, y(d) + 4, fmt_us(d), t["muted"], 11, anchor="end")
    s.line(left, bot, right, bot, t["axis"], 1)
    for i, n in enumerate(names):
        s.text(xs[i], bot + 22, n, t["ink2"], 12, anchor="middle")

    ends = spread([y(data[k][-1]) for k, _ in arms], gap=17)
    for i, (key, label) in enumerate(arms):
        col = t["series"][i]
        pts = [(xs[j], y(data[key][j])) for j in range(len(names))]
        s.path(pts, col, 2)
        for px, py in pts:
            s.dot(px, py, 4.5, col, ring=t["surface"])
        # A leader from the mark to a nudged label, so moving the text does not
        # break which line it belongs to.
        s.line(pts[-1][0] + 6, pts[-1][1], right + 12, ends[i], t["axis"], 1)
        s.dot(right + 16, ends[i], 4.5, col)
        s.text(right + 26, ends[i] + 4, label, t["ink2"], 12)

    s.text(20, h - 18, "Contention costs 20x at p99. Real-time priority gives "
                       "all of it back; locking memory adds nothing.",
           t["muted"], 11)
    return s.render(
        "One busy loop per core, and what priority does about it",
        "Line chart across p50, p90, p99 and p99.9 for four scheduling arms: "
        "contended, idle, contended under SCHED_FIFO, and the same with "
        "mlockall.")


CHARTS = {
    "correction": chart_correction,
    "latency-vs-rate": chart_latency,
    "batching": chart_batching,
    "scheduling": chart_sched,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    written = 0
    for name, fn in CHARTS.items():
        for mode in ("light", "dark"):
            path = os.path.join(OUT, f"{name}-{mode}.svg")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(fn(THEME[mode]))
            written += 1
            print(f"wrote {os.path.relpath(path, ROOT)}")
    print(f"{written} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
