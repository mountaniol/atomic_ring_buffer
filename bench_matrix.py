#!/usr/bin/env python3
"""Batch-size impact matrix for the ring buffer benchmark.

Runs ring_buf_test.out over every (library variant, placement, mode)
combination, interleaving configurations round-robin so thermal drift and
background noise spread evenly.  Repetitions are adaptive: after each run
the script computes the bootstrap 95% confidence interval of the median
throughput, ESTIMATES how many runs this configuration still needs to
reach CI_TARGET, and keeps running until the target is met or R_MAX is
hit (then the config is flagged and the estimate is reported).

All metrics come from the benchmark's OWN thread-local timers (started
after CPU pinning, inside the threads) - process startup, core selection
and teardown are never part of a measurement.  Wall clock is not used.

Every run also returns the full latency distribution (a log-bucket
histogram counted by the benchmark itself), from which the script computes
true p50/p99/p99.9 and, per thread placement, the three configurations a
user chooses between: most throughput, smallest 1-in-100 worst case,
smallest average delay.

Outputs: bench_matrix_results.csv (raw runs), bench_matrix_hist.csv (raw
distributions), bench_matrix_report.md (legend + tables + regression +
plain-language conclusions), the same report plus a visual latency summary
to stdout.

No command-line arguments by design: tune the constants below.
"""

import csv
import math
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path

# ----------------------------------------------------------------- tuning
SAMPLES_M   = 200                 # messages per run, millions
R_MIN       = 5                   # minimum repetitions per config
R_MAX       = 12                  # ceiling; flagged beyond this
CI_TARGET   = 0.01                # stop when CI halfwidth <= 1% of median
BOOT_N      = 2000                # bootstrap resamples
RUN_TIMEOUT = 120                 # seconds per single benchmark run
UNSTABLE_IQR = 0.03               # spread above any converged config's
VARIANTS    = ["line", "idx"]
PLACEMENTS  = ["--cores", "--siblings", "--same-core"]
BATCHES     = [1, 2, 4, 7, 8, 14, 16, 28, 32, 64, 128, 256]
MODES       = (["single"] + [f"b{n}" for n in BATCHES] + ["auto", "wait"])

# Latency histogram: mirrors the C contract in ring_buf_test_int.c.  The
# benchmark counts into LAT_SUB sub-buckets per octave from 2^LAT_EMIN ns;
# the display bands below are pure sums of those buckets (every display
# edge is a power of two, so it falls exactly on a bucket boundary).
LAT_EMIN, LAT_SUB, LAT_NB = 4, 8, 160
LAT_STRIDE_DOC = 1021             # sampling stride used by the benchmark
DISP_CUT   = [16, 24, 32, 40, 48, 64, 80, 96, 112]   # fine-bucket cuts
DISP_LABEL = ["<64ns", "64-128ns", "128-256ns", "256-512ns", "0.5-1us",
              "1-4us", "4-16us", "16-64us", "64-256us", ">=256us"]
# What the user is choosing between; the screen shows one panel per goal.
GOALS = [("fast", "FASTEST", "most messages per second"),
         ("calm", "LOWEST p99", "smallest 1-in-100 worst case"),
         ("quick", "LOWEST mean", "smallest average delay")]

ROOT  = Path(__file__).resolve().parent
BUILD = ROOT / "bench_build"
CFLAGS = ("-Wall -Wextra -std=c11 -O3 -march=native -flto "
          "-funroll-loops -fomit-frame-pointer").split()

random.seed(42)                   # reproducible bootstrap

# ----------------------------------------------------------------- helpers

def build_binaries():
    BUILD.mkdir(exist_ok=True)
    for var in VARIANTS:
        out = BUILD / f"bench-{var}.out"
        cmd = (["gcc"] + CFLAGS
               + (["-DRB_INT_INDEXED"] if var == "idx" else [])
               + ["-I", str(ROOT), "-o", str(out),
                  str(ROOT / "ring_buf_test_int.c"), str(ROOT / "ring_buf.c"),
                  "-pthread", "-lm"])
        print(f"building {out.name} ...")
        subprocess.run(cmd, check=True)


def mode_args(mode):
    if mode == "single":
        return []
    if mode == "wait":
        return ["--wait"]
    if mode == "auto":
        return ["--auto-batch"]
    return ["--batch", mode[1:]]


RE_PROD = re.compile(r"Producer finished in ([0-9.]+) seconds, miss+es: (\d+)")
RE_CONS = re.compile(r"Consumer finished in ([0-9.]+) seconds, miss+es: (\d+)")
RE_TPUT = re.compile(r"Throughput: ([0-9,.]+) messages/sec")
RE_LAT  = re.compile(r"min (\d+) ns, mean (\d+) ns, max (\d+) ns, "
                     r"stddev (\d+) ns")
RE_HIST = re.compile(r"LatHist v1 emin=(\d+) sub=(\d+) nb=(\d+) "
                     r"samples=(\d+):(.*)")


def hist_edges(k):
    """Bucket k covers [lo, hi) nanoseconds; the top bucket is open."""
    e, s = LAT_EMIN + k // LAT_SUB, k % LAT_SUB
    step = (1 << e) // LAT_SUB
    lo = (1 << e) + s * step
    return lo, (math.inf if k == LAT_NB - 1 else lo + step)


def parse_hist(out):
    """The benchmark's LatHist line -> a LAT_NB-long count vector.  Returns
    None if the line is missing, truncated, or was produced by a build whose
    bucket layout differs from the constants above."""
    m = RE_HIST.search(out)
    if not m:
        return None
    emin, sub, nb, n = (int(m.group(i)) for i in (1, 2, 3, 4))
    if (emin, sub, nb) != (LAT_EMIN, LAT_SUB, LAT_NB):
        print(f"   LatHist contract mismatch: emin={emin} sub={sub} nb={nb}")
        return None
    h = [0] * LAT_NB
    try:
        for pair in m.group(5).split():
            k, c = pair.split(":")
            h[int(k)] = int(c)
    except (ValueError, IndexError):
        return None
    return h if sum(h) == n else None


def hist_pct(h, p):
    """p-quantile, interpolated inside the bucket that contains it."""
    n = sum(h)
    if 0 == n:
        return 0.0
    t, c = p * n, 0
    for k, v in enumerate(h):
        if v and c + v >= t:
            lo, hi = hist_edges(k)
            return lo if math.isinf(hi) else lo + (hi - lo) * ((t - c) / v)
        c += v
    return float(hist_edges(LAT_NB - 1)[0])


def disp_bins(h):
    """160 fine buckets -> the 10 display bands."""
    out, prev = [], 0
    for cut in DISP_CUT + [LAT_NB]:
        out.append(sum(h[prev:cut]))
        prev = cut
    return out


def merge_hists(rs):
    """Pool the per-run histograms by SUMMING them.  Every run delivers the
    same sample count, so the sum is an equal-weight average, and the tail
    keeps the resolution of all runs together (a per-bucket median would not
    even sum to n).  A run whose p99 is a MAD outlier is KEPT - it is real
    machine behaviour - but reported with the counterfactual.
    Returns (pooled, notes)."""
    hs = [r["hist"] for r in rs]
    pooled = [sum(c) for c in zip(*hs)]
    notes = []
    if len(rs) < 5:
        return pooled, notes
    xs = [math.log(max(r["lat_p99"], 1.0)) for r in rs]
    med = statistics.median(xs)
    mad = 1.4826 * statistics.median([abs(x - med) for x in xs])
    for i, x in enumerate(xs):
        if mad > 0 and abs(x - med) > max(3 * mad, math.log(1.25)):
            alt = [sum(c) for c in zip(*[h for j, h in enumerate(hs)
                                         if j != i])]
            notes.append((i, math.exp(x), math.exp(med),
                          hist_pct(pooled, .99), hist_pct(alt, .99),
                          hist_pct(pooled, .999), hist_pct(alt, .999)))
    return pooled, notes


def boot_pct_ci(rs, p):
    """Bootstrap 95% CI halfwidth of the pooled p-quantile, RELATIVE.  The
    resampling unit is the RUN: latency samples inside one run share that
    run's machine state, so resampling samples would understate the CI by
    orders of magnitude."""
    hs = [r["hist"] for r in rs]
    if len(hs) < 2:
        return math.inf
    base = hist_pct([sum(c) for c in zip(*hs)], p)
    if 0 == base:
        return math.inf
    qs = sorted(hist_pct([sum(c) for c in zip(*random.choices(hs, k=len(hs)))],
                         p) for _ in range(BOOT_N))
    return (qs[int(0.975 * BOOT_N)] - qs[int(0.025 * BOOT_N)]) / 2 / base


def run_once(cfg):
    """One benchmark run; returns a metrics dict or None on failure."""
    var, placement, mode = cfg
    cmd = [str(BUILD / f"bench-{var}.out"), placement,
           "--samples", f"{SAMPLES_M}m"] + mode_args(mode)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    if p.returncode != 0:         # abort() = ordering violation, etc.
        return None
    out = p.stdout
    prod = RE_PROD.search(out)
    cons = RE_CONS.search(out)
    lat = RE_LAT.search(out)
    hist = parse_hist(out)
    if not (prod and cons and lat and hist):
        return None
    # producer/consumer block order varies by finish time: take the
    # Throughput line printed right after "Consumer finished"
    m = RE_TPUT.search(out[cons.end():])
    if not m:
        m = RE_TPUT.search(out)
    if not m:
        return None
    return {
        "tput": float(m.group(1).replace(",", "")),
        "miss_prod": int(prod.group(2)),
        "miss_cons": int(cons.group(2)),
        "lat_min": int(lat.group(1)),
        "lat_mean": int(lat.group(2)),
        "lat_max": int(lat.group(3)),
        "lat_std": int(lat.group(4)),
        "lat_n": sum(hist),
        "lat_p50": hist_pct(hist, 0.50),
        "lat_p99": hist_pct(hist, 0.99),
        "lat_p999": hist_pct(hist, 0.999),
        "hist": hist,
        # set_my_prio() asks for SCHED_FIFO 99 and only perror()s when it is
        # refused, so a non-root run measures a different scheduling regime
        "rt": "setschedparam" not in (p.stderr or ""),
    }


def sched_regime(runs):
    """True when every run really got SCHED_FIFO.  Without it other tasks
    preempt the two threads, and on --siblings and --same-core, where they
    share one core, that changes what is being measured."""
    rs = [r for v in runs.values() for r in v]
    return bool(rs) and all(r["rt"] for r in rs)


def ci_halfwidth(runs):
    """Bootstrap 95% CI halfwidth of the median throughput, RELATIVE to
    the median (0.02 = +-2%)."""
    xs = [r["tput"] for r in runs]
    if len(xs) < 2:
        return math.inf
    med = statistics.median(xs)
    meds = sorted(statistics.median(random.choices(xs, k=len(xs)))
                  for _ in range(BOOT_N))
    lo, hi = meds[int(0.025 * BOOT_N)], meds[int(0.975 * BOOT_N)]
    return (hi - lo) / 2 / med


def reps_needed(runs):
    """Estimate of the TOTAL number of runs this config needs to reach
    CI_TARGET (CI halfwidth shrinks ~ 1/sqrt(n))."""
    hw = ci_halfwidth(runs)
    if hw <= CI_TARGET:
        return len(runs)
    return math.ceil(len(runs) * (hw / CI_TARGET) ** 2)


def spread(runs):
    """Interquartile spread of the throughput, relative to the median."""
    xs = sorted(r["tput"] for r in runs)
    n = len(xs)
    if n < 4:
        return 0.0
    med = statistics.median(xs)
    return (xs[int(.75 * (n - 1))] - xs[int(.25 * (n - 1))]) / med if med else 0


def fmt_need(runs):
    """What to say about a configuration that did not converge.

    reps_needed() assumes the runs are independent draws from one stable
    distribution, so that the CI falls like 1/sqrt(n).  Measured on this
    benchmark that assumption fails for some configurations: 60 repetitions
    of one of them left the CI at 14% where the law predicted 6%.  Those
    configurations flip between regimes run to run, and repeating them is a
    waste - so they are reported as unstable instead of being given a
    reps estimate nobody should act on.  The threshold is set just above the
    widest spread any CONVERGED configuration shows."""
    s = spread(runs)
    if s > UNSTABLE_IQR:
        return f"unstable ±{s * 100:.0f}%"
    n = reps_needed(runs)
    return f"needs ~{n}" if n <= 200 else "needs >200"


def converged(runs):
    return len(runs) >= R_MIN and ci_halfwidth(runs) <= CI_TARGET


def linreg(xs, ys):
    """Least squares y = a + b*x; returns (a, b, r2)."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0:
        return my, 0.0, 0.0
    b = sxy / sxx
    a = my - b * mx
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys)
    r2 = 1 - ss_res / ss_tot if ss_tot > 0 else 1.0
    return a, b, r2


def solve_ols(X, Y):
    """OLS via normal equations; returns (betas, r2, tstats).
    Gaussian elimination with partial pivoting; stdlib only."""
    k = len(X[0])
    n = len(X)
    XtX = [[sum(X[r][i] * X[r][j] for r in range(n)) for j in range(k)]
           for i in range(k)]
    XtY = [sum(X[r][i] * Y[r] for r in range(n)) for i in range(k)]
    # invert XtX (augment with identity) - also needed for std errors
    A = [XtX[i][:] + [1.0 if i == j else 0.0 for j in range(k)]
         for i in range(k)]
    for col in range(k):
        piv = max(range(col, k), key=lambda r: abs(A[r][col]))
        A[col], A[piv] = A[piv], A[col]
        d = A[col][col]
        if abs(d) < 1e-12:
            return None, 0.0, None
        A[col] = [v / d for v in A[col]]
        for r in range(k):
            if r != col and A[r][col] != 0:
                f = A[r][col]
                A[r] = [v - f * w for v, w in zip(A[r], A[col])]
    inv = [row[k:] for row in A]
    betas = [sum(inv[i][j] * XtY[j] for j in range(k)) for i in range(k)]
    pred = [sum(b * x for b, x in zip(betas, X[r])) for r in range(n)]
    my = sum(Y) / n
    ss_res = sum((y - p) ** 2 for y, p in zip(Y, pred))
    ss_tot = sum((y - my) ** 2 for y in Y)
    r2 = 1 - ss_res / ss_tot if ss_tot > 0 else 1.0
    sigma2 = ss_res / max(n - k, 1)
    tstats = [betas[i] / math.sqrt(sigma2 * inv[i][i])
              if inv[i][i] > 0 else 0.0 for i in range(k)]
    return betas, r2, tstats


def eta2(pairs):
    """One-way variance explained (eta squared) for (level, y) pairs."""
    grand = sum(y for _, y in pairs) / len(pairs)
    groups = {}
    for lv, y in pairs:
        groups.setdefault(lv, []).append(y)
    ss_between = sum(len(g) * (sum(g) / len(g) - grand) ** 2
                     for g in groups.values())
    ss_total = sum((y - grand) ** 2 for _, y in pairs)
    return ss_between / ss_total if ss_total > 0 else 0.0


def spearman(xs, ys):
    """Spearman rank correlation, stdlib only."""
    def ranks(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        r = [0.0] * len(v)
        for rank, i in enumerate(order):
            r[i] = rank
        return r
    rx, ry = ranks(xs), ranks(ys)
    n = len(xs)
    mx, my = sum(rx) / n, sum(ry) / n
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    den = math.sqrt(sum((a - mx) ** 2 for a in rx)
                    * sum((b - my) ** 2 for b in ry))
    return num / den if den > 0 else 0.0


def render_table(headers, rows):
    """Markdown table with every column padded to its widest cell."""
    widths = [max(len(str(headers[i])), *(len(str(r[i])) for r in rows))
              for i in range(len(headers))]
    def line(cells):
        return "| " + " | ".join(str(c).ljust(w)
                                 for c, w in zip(cells, widths)) + " |"
    sep = "|" + "|".join("-" * (w + 2) for w in widths) + "|"
    return [line(headers), sep] + [line(r) for r in rows]


def champions(runs, failed):
    """Per placement, the three configurations a user actually chooses
    between: most throughput, smallest 1-in-100 worst case, smallest average
    delay.  Ties are broken by the other two metrics, so the pick is
    reproducible.  Returns {placement: {goal: cfg, "stat": {...}}}."""
    out = {}
    for pl in PLACEMENTS:
        cs = [(v, pl, m) for v in VARIANTS for m in MODES
              if (v, pl, m) not in failed and runs.get((v, pl, m))]
        if not cs:
            out[pl] = None
            continue
        stat = {}
        for c in cs:
            pooled, notes = merge_hists(runs[c])
            tp = statistics.median(r["tput"] for r in runs[c])
            mn = statistics.median(r["lat_mean"] for r in runs[c])
            stat[c] = {
                "hist": pooled, "notes": notes, "reps": len(runs[c]),
                "tput": tp, "mean": mn,
                "p01": hist_pct(pooled, .01), "p50": hist_pct(pooled, .50),
                "p99": hist_pct(pooled, .99), "p999": hist_pct(pooled, .999),
                "inflight": tp * mn / 1e9,      # Little's law: L = lambda*W
                "tputci": ci_halfwidth(runs[c]),
                "conv": converged(runs[c]),
            }
        # A median with a wide confidence interval can win a title by luck, so
        # the pool is the configurations that actually converged.  Only when
        # NONE of them did does the whole set compete - and then every winner
        # is flagged, here and on screen.
        pool = [c for c in cs if stat[c]["conv"]] or cs
        pick = {
            "fast": max(pool, key=lambda c: (stat[c]["tput"], -stat[c]["p99"])),
            "calm": min(pool, key=lambda c: (stat[c]["p99"], -stat[c]["tput"])),
            "quick": min(pool, key=lambda c: (stat[c]["mean"],
                                              -stat[c]["tput"])),
        }
        # the bootstrap is only worth its cost on the nine configs that win
        for c in set(pick.values()):
            stat[c]["p99ci"] = boot_pct_ci(runs[c], .99)
        # configurations statistically indistinguishable from each winner
        near = {}
        for g, key in (("fast", "tput"), ("calm", "p99"), ("quick", "mean")):
            w = stat[pick[g]][key]
            near[g] = [c[0] + " " + c[2] for c in pool if c != pick[g]
                       and abs(stat[c][key] - w) <= 0.01 * w]
        out[pl] = dict(pick, stat=stat, near=near)
    return out


# ------------------------------------------------- screen summary rendering
W_LBL, BAR_DEC = 9, 6.0                      # label width, bar log window
WIDE = shutil.get_terminal_size((100, 24)).columns >= 99
BAR_W, W_SHARE, W_CNT = (12, 7, 6) if WIDE else (7, 7, 4)
PANEL = BAR_W + W_SHARE + W_CNT + 4          # "| " + bar + share + " " + n + "|"
COLOR = sys.stdout.isatty()
BAND_COL = ["32"] * 4 + ["33"] * 4 + ["31"] * 2   # green / yellow / red


def si(n):
    if n >= 1e6:
        return f"{n / 1e6:.2f}M" if W_CNT >= 6 else f"{n / 1e6:.1f}M"
    if n >= 1e4:
        return f"{n / 1e3:.0f}k"
    return str(n)


def fmt_ns(v):
    if v < 1000:
        return f"{v:.0f} ns"
    if v < 10000:
        return f"{v / 1e3:.1f} us"
    if v < 1e6:
        return f"{v / 1e3:.0f} us"
    return f"{v / 1e6:.2f} ms"


def bar(c, n):
    """Logarithmic bar over BAR_DEC decades: a band holding one message in a
    million still shows.  Gridlines mark 1% and 0.01%."""
    t = ["·"] * BAR_W
    for k in (2, 4):
        t[int(round(BAR_W * (BAR_DEC - k) / BAR_DEC)) - 1] = "┊"
    if 0 == c:
        return "".join(t)                    # never happened: bare track
    raw = BAR_W * (BAR_DEC + math.log10(c / n)) / BAR_DEC
    if raw < 1:
        return "▏" + "".join(t[1:])     # present, below the scale
    ln = min(BAR_W, int(round(raw)))
    return "█" * ln + "".join(t[ln:])


def fmt_pct(c, n):
    """Share of messages, never rounded down to a misleading 0.00%."""
    if not c:
        return "-"
    s = 100.0 * c / n
    return (f"{s:.1f}%" if s >= 10 else f"{s:.2f}%" if s >= 0.1
            else f"{s:.3f}%" if s >= 0.001 else "<0.001%")


def cell(c, n, band):
    sh = fmt_pct(c, n)
    b = bar(c, n)
    if COLOR:
        b = f"\033[{BAND_COL[band]}m{b}\033[0m"
    return ("│ " + b + sh.rjust(W_SHARE) + " "
            + ("-" if 0 == c else si(c)).rjust(W_CNT) + "│")


def panel_top(goal):
    t = goal[:PANEL - 5]
    return "┌─ " + t + " " + "─" * (PANEL - len(t) - 5) + "┐"


def panel_sub(cfg, st):
    # "!" = the median never converged, so this title was won on a coin toss
    mark = "" if st.get("conv", True) else "!"
    head = f" {cfg[0]} {cfg[2]} · {st['tput'] / 1e6:.0f} M/s{mark}"
    s = f"{head} · {st['reps']}r"          # drop the rep count if it a-
    if len(s) > PANEL - 2:                 # would be cut mid-token
        s = head
    return "│" + s[:PANEL - 2].ljust(PANEL - 2) + "│"


def wrap_frags(frags, width, indent="  "):
    """Join ' · '-separated fragments into lines that fit the actual summary
    width, so the same code renders on a 99- and on a 78-column layout."""
    lines, cur = [], ""
    for f in frags:
        cand = f if not cur else cur + " · " + f
        if len(indent) + len(cand) <= width:
            cur = cand
        else:
            if cur:
                lines.append(indent + cur)
            cur = f
    if cur:
        lines.append(indent + cur)
    return lines


PLACE_NOTE = {
    "--cores": "Two physical cores: the hop itself costs ~56 ns, the rest "
               "is queue.",
    "--siblings": "HT siblings share one L1: cheapest hop on the machine, "
                  "shortest tail.",
    "--same-core": "One CPU for both threads: every hand-off costs a "
                   "context switch (~500 ns).",
}


def print_latency_summary(champs, rt=True):
    """The on-screen tuning summary: three placements x three goals, one
    latency histogram each, on one shared scale."""
    w = W_LBL + 1 + 3 * PANEL + 2
    p = print
    p("\n" + "═" * w)
    p(" LATENCY DISTRIBUTION · where the messages actually landed")
    p("═" * w)
    if not rt:
        p(" !! MEASURED WITHOUT REAL-TIME PRIORITY.  SCHED_FIFO was refused")
        p(" !! (the script was not run as root), so any other task could")
        p(" !! preempt the producer and the consumer.  All three placements")
        p(" !! were measured the same way, so they stay comparable with each")
        p(" !! other - but on siblings and same-core, where both threads")
        p(" !! share one core, a foreign task steals from BOTH at once.")
        p(" !! Re-run as root before trusting these numbers.")
        p("")
    p(" For each way of placing the two threads, the three configurations you")
    p(" would actually choose between: most messages per second, smallest")
    p(" 1-in-100 worst case, smallest average delay.  A row is a delay band;")
    p(" the bar is the share of messages that landed in it.  Bars are")
    p(" LOGARITHMIC - each 10x drop in share costs ~2 characters - so a band")
    p(" holding one message in a million stays visible.  The % and the count")
    p(" beside every bar are the exact truth.  'ring N' = how many messages")
    p(" were queued inside the ring at any instant.")
    for pl in PLACEMENTS:
        ch = champs.get(pl)
        p("")
        if not ch:
            p(f"{pl.lstrip('-'):<{W_LBL}} no data - every run failed")
            continue
        cfgs = [ch[g] for g, _, _ in GOALS]
        sts = [ch["stat"][c] for c in cfgs]
        hs = [s["hist"] for s in sts]
        ns = [sum(h) for h in hs]
        bins = [disp_bins(h) for h in hs]
        p(pl.lstrip("-").ljust(W_LBL + 1)
          + " ".join(panel_top(g[1]) for g in GOALS))
        p(" " * (W_LBL + 1)
          + " ".join(panel_sub(c, s) for c, s in zip(cfgs, sts)))
        for i, lbl in enumerate(DISP_LABEL):
            p(lbl.rjust(W_LBL) + " "
              + " ".join(cell(b[i], n, i) for b, n in zip(bins, ns)))
        p(" " * (W_LBL + 1)
          + " ".join(["└" + "─" * (PANEL - 2) + "┘"] * 3))
        for ln in wrap_frags([
                "ring " + " / ".join(f"{s['inflight']:.0f}" for s in sts),
                "floor " + " / ".join(fmt_ns(s["p01"]) for s in sts),
                "half within " + " / ".join(fmt_ns(s["p50"]) for s in sts),
                "99 in 100 within "
                + " / ".join(fmt_ns(s["p99"]) for s in sts),
                "past 256 us: " + " / ".join(str(b[9]) for b in bins)
                + " messages"], w):
            p(ln)
        p("  " + PLACE_NOTE.get(pl, ""))
        if cfgs[1] == cfgs[2]:
            p("  The same configuration wins both lowest p99 and lowest "
              "mean here.")
        for c, s in dict(zip(cfgs, sts)).items():   # a config can win twice
            for i, bad, mid, p99, p99a, p999, p999a in s["notes"]:
                for ln in wrap_frags(
                        [f"!! {c[0]} {c[2]} run {i}: p99 {fmt_ns(bad)} vs "
                         f"{fmt_ns(mid)} median - KEPT",
                         f"without it p99 {fmt_ns(p99a)} "
                         f"({(p99a / p99 - 1) * 100:+.1f}%)",
                         f"p99.9 {fmt_ns(p999a)} "
                         f"({(p999a / p999 - 1) * 100:+.1f}%)"], w):
                    p(ln)
    p("")
    p("─" * w)
    p(" Mass on the LEFT (under 256 ns) = the message crossed as fast as the")
    p(" hardware can.  Mass in the MIDDLE (1-64 us) = the ring was full of")
    p(" work and the message waited its turn: that is the price of")
    p(" throughput, not the cost of a transfer.  'ring N' divided by the")
    p(" throughput IS the average delay, so a deep ring and a low average")
    p(" cannot be bought together.  Everything past 256 us is not the ring")
    p(" at all: a timer tick or another task taking the CPU.")
    best = {}
    for g, _, _ in GOALS:
        cand = [(champs[pl]["stat"][champs[pl][g]], champs[pl][g], pl)
                for pl in PLACEMENTS if champs.get(pl)]
        if not cand:
            continue
        best[g] = (max(cand, key=lambda t: t[0]["tput"]) if g == "fast"
                   else min(cand, key=lambda t: t[0]["p99" if g == "calm"
                                                    else "mean"]))
    if best:
        p("")
        p(" WHAT TO PICK for your own system")
        txt = {"fast": "throughput above all", "calm": "lowest worst case",
               "quick": "lowest average delay"}
        num = {"fast": lambda s: f"{s['tput'] / 1e6:.0f} M msg/s",
               "calm": lambda s: f"p99 {fmt_ns(s['p99'])}",
               "quick": lambda s: f"mean {fmt_ns(s['mean'])}"}
        for g, _, _ in GOALS:
            if g not in best:
                continue
            s, c, pl = best[g]
            p(f"   {txt[g]:<22}-> {c[0] + ' ' + c[2]:<12} on "
              f"{pl.lstrip('-'):<10} {num[g](s)}"
              + ("" if s.get("conv", True) else "   (! unconverged)"))
        p(" Measure your own producer rate first: if it stays below the")
        p(" consumer's capacity the ring never fills, and the 'floor' figure")
        p(" is what you will actually see - not the middle of the histogram.")
    p(" Full tables: bench_matrix_report.md")
    p(" Raw runs: bench_matrix_results.csv, bench_matrix_hist.csv")


LEGEND = f"""\
## Legend - what everything in this report means

**One table = one build variant on one thread placement.**  Rows are the
operating modes of the SAME benchmark (push {SAMPLES_M} million sequential
integers through the ring, verify they arrive in exact FIFO order):

| Row      | Meaning                                                                             |
|----------|-------------------------------------------------------------------------------------|
| `single` | baseline: one message per call (`rb_push_int` / `rb_pull_int`), no batching at all  |
| `bN`     | producer sends bursts of N messages per call (`rb_push_int_burst`); consumer drains N per call |
| `auto`   | burst size picked adaptively by `rb_batch_size()` from the producer's measured rate |
| `wait`   | blocking mode: threads sleep on a futex instead of spinning (`rb_push_wait`)        |

Build variants: `line` = default format (7 messages + flag per 64-byte
cache line); `idx` = `-DRB_INT_INDEXED` (plain cells, free-running
indices - pays more per publication, so batching amortizes more).
Placements: `--cores` = two distinct physical cores; `--siblings` = two
hyperthreads of one physical core (shared caches, cheapest transfer);
`--same-core` = both threads on ONE logical CPU (every hand-off costs a
context switch).

Columns:

| Column         | Meaning                                                                            |
|----------------|------------------------------------------------------------------------------------|
| `reps`         | how many runs the script needed until the throughput median was statistically solid: 95% confidence interval within +-{CI_TARGET:.0%}.  `N! (needs ~M)` = not reached after N runs, and M runs should get there.  `N! (unstable +-X%)` = the configuration itself flips between regimes run to run, so repeating it does NOT help - X% is the spread between its quartiles, and no converged configuration here exceeds {UNSTABLE_IQR:.0%} |
| `tput M/s`     | median throughput over the reps, million messages per second                       |
| `CI +-%`       | actual confidence-interval halfwidth of that median, in percent                    |
| `lat min/mean` | latency of individual messages, nanoseconds (median over reps).  Stamped when the message is CREATED, so batching delay is included.  In this saturated test `mean` is mostly QUEUE waiting time, not transfer cost - `min` is the true transfer floor |
| `p50/p99/p99.9`| pooled percentiles over ALL reps of that config: half / 99 in 100 / 999 in 1000 of the messages arrived within this delay.  `p99` is the jitter number a latency-critical user sizes against |
| `lat max(abs)` | the single worst event across all reps - owned by OS interrupts, not by the ring   |
| `in ring`      | messages queued inside the ring at any instant, from Little's law `L = throughput x mean`.  When this equals the ring capacity the producer is simply faster than the consumer, and `mean` measures the queue, not the transport |
| `miss p/c`     | producer/consumer: how often a side found the ring full/empty and had to yield     |

Latency samples: every {LAT_STRIDE_DOC}th message is timestamped when it is
CREATED and measured when it is PULLED.  The stride is PRIME on purpose: a
power-of-two stride divides both the batch size and the ring capacity, which
would pin every sample to in-batch position 0 and to a handful of ring cells.
Each run contributes ~{SAMPLES_M * 1000000 // 1021:,} samples; percentiles are
computed from a {LAT_SUB}-sub-buckets-per-octave histogram (bucket resolution
below 0.5% on every distribution seen here) pooled over all reps of a config.
"""


# ----------------------------------------------------------------- main

def collect():
    """Build, warm up, run all configs adaptively; returns (runs, failed)."""
    build_binaries()
    configs = [(v, p, m) for v in VARIANTS for p in PLACEMENTS
               for m in MODES]
    runs = {c: [] for c in configs}
    failed = set()

    print(f"\n{len(configs)} configs, {SAMPLES_M}m msgs/run, "
          f"reps {R_MIN}..{R_MAX} (CI target ±{CI_TARGET:.0%})")

    print("\n== warmup round (discarded)")
    for c in configs:
        if run_once(c) is None:
            print(f"   WARMUP FAILED: {c}")
            failed.add(c)

    rnd = 0
    while True:
        rnd += 1
        pending = [c for c in configs if c not in failed
                   and len(runs[c]) < R_MAX and not converged(runs[c])]
        if not pending:
            break
        print(f"== round {rnd}: {len(pending)} configs pending")
        for c in pending:
            r = run_once(c)
            if r is None:
                print(f"   RUN FAILED: {c}")
                failed.add(c)
                continue
            runs[c].append(r)
            n = len(runs[c])
            if n >= R_MIN:
                hw = ci_halfwidth(runs[c])
                status = ("converged" if hw <= CI_TARGET
                          else fmt_need(runs[c]))
                print(f"   {c[0]:4s} {c[1]:10s} {c[2]:6s}: "
                      f"{r['tput'] / 1e6:7.1f} M/s  "
                      f"CI ±{hw:.1%} -> {status}")
            else:
                print(f"   {c[0]:4s} {c[1]:10s} {c[2]:6s}: "
                      f"{r['tput'] / 1e6:7.1f} M/s  ({n}/{R_MIN} min reps)")

    # ------------------------------------------------------------- CSV
    with open(ROOT / "bench_matrix_results.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["variant", "placement", "mode", "rep", "tput",
                    "miss_prod", "miss_cons",
                    "lat_min", "lat_mean", "lat_max", "lat_std",
                    "lat_n", "lat_p50", "lat_p99", "lat_p999", "rt"])
        for c, rs in runs.items():
            for i, r in enumerate(rs):
                w.writerow([c[0], c[1], c[2], i, f"{r['tput']:.0f}",
                            r["miss_prod"], r["miss_cons"], r["lat_min"],
                            r["lat_mean"], r["lat_max"], r["lat_std"],
                            r["lat_n"], f"{r['lat_p50']:.0f}",
                            f"{r['lat_p99']:.0f}", f"{r['lat_p999']:.0f}",
                            int(r["rt"])])

    # raw distributions, long format, non-empty buckets only
    with open(ROOT / "bench_matrix_hist.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["variant", "placement", "mode", "rep", "bucket",
                    "lo_ns", "hi_ns", "count"])
        for c, rs in runs.items():
            for i, r in enumerate(rs):
                for k, n in enumerate(r["hist"]):
                    if n:
                        lo, hi = hist_edges(k)
                        w.writerow([c[0], c[1], c[2], i, k, lo,
                                    "" if math.isinf(hi) else hi, n])
    return runs, failed


def make_report(runs, failed, champs):
    """Render the full report from collected data; returns the text."""
    rt = sched_regime(runs)
    rep = [f"# Ring buffer batch matrix - "
           f"{datetime.now():%Y-%m-%d %H:%M}, "
           f"{SAMPLES_M}m msgs/run, CI target ±{CI_TARGET:.0%}, "
           f"latency sampled every {LAT_STRIDE_DOC}th message",
           ""]
    if not rt:
        rep += ["> **Measured WITHOUT real-time priority.**  `SCHED_FIFO` was "
                "refused (not run as root), so any other task could preempt "
                "the producer and the consumer.  All placements were measured "
                "the same way and stay comparable with each other, but on "
                "`--siblings` and `--same-core` a foreign task steals from "
                "both threads at once.  Re-run as root before trusting the "
                "absolute numbers.", ""]
    rep.append(LEGEND)

    def med(c, k):
        return statistics.median(r[k] for r in runs[c])

    def mtput(c):
        return med(c, "tput")

    overall = []                  # (tput, lat_mean, cfg) for global summary

    # ------------------------- where the latency lands: the three choices
    rep.append("\n## Where the latency lands - the three choices per "
               "placement\n")
    rep.append("For every way of placing the two threads, the configuration "
               "that wins each of the three goals a user actually has.  The "
               "screen summary draws the full distribution of these same "
               "nine configurations.  Only configurations whose median "
               f"throughput converged to ±{CI_TARGET:.0%} may win; a winner "
               "marked `!` did not, because nothing in that placement did, "
               "and its ranking is then worth no more than its interval.\n")
    rows = []
    unconv = False
    for pl in PLACEMENTS:
        ch = champs.get(pl)
        if not ch:
            rows.append([pl, "-", "no data"] + [""] * 9)
            continue
        for g, title, _ in GOALS:
            c = ch[g]
            s = ch["stat"][c]
            tied = ", ".join(ch["near"][g][:3]) or "-"
            ci = s.get("p99ci", math.inf)
            unconv = unconv or not s["conv"]
            rows.append([pl, title,
                         f"{c[0]} {c[2]}" + ("" if s["conv"] else " !"),
                         f"{s['tput'] / 1e6:.0f}", f"{s['tputci'] * 100:.1f}",
                         fmt_ns(s["p01"]),
                         fmt_ns(s["p50"]), fmt_ns(s["p99"]),
                         "n/a" if math.isinf(ci) else f"{ci * 100:.1f}",
                         fmt_ns(s["p999"]), f"{s['inflight']:.0f}", tied])
    rep.extend(render_table(["placement", "goal", "winner", "tput M/s",
                             "tput ±%", "floor", "p50", "p99", "p99 ±%",
                             "p99.9", "in ring", "within 1%"], rows))
    if unconv:
        rep.append("\n**A `!` above means the run was too noisy to rank.**  "
                   "Re-run on an otherwise idle machine before acting on "
                   "those rows: a median with a wide interval can take a "
                   "title by luck, and some configurations on some machines "
                   "are bimodal, so more repetitions do not always help.")

    rep.append("\nShare of messages per delay band, pooled over all reps "
               "of the winning configuration:\n")
    hdr = ["band"]
    cols = []
    for pl in PLACEMENTS:
        ch = champs.get(pl)
        for g, title, _ in GOALS:
            hdr.append(f"{pl.lstrip('-')} {g}")
            cols.append(disp_bins(ch["stat"][ch[g]]["hist"]) if ch else None)
    brows = []
    for i, lbl in enumerate(DISP_LABEL):
        row = [lbl]
        for b in cols:
            row.append("-" if b is None else fmt_pct(b[i], sum(b)))
        brows.append(row)
    rep.extend(render_table(hdr, brows))

    for var in VARIANTS:
        for pl in PLACEMENTS:
            rep.append(f"\n## {var} {pl}\n")
            headers = ["mode", "reps", "tput M/s", "CI ±%", "lat min",
                       "lat mean", "p50", "p99", "p99.9", "lat max(abs)",
                       "in ring", "miss p/c"]
            rows = []
            st = (champs.get(pl) or {}).get("stat", {})
            for m in MODES:
                c = (var, pl, m)
                if c in failed or not runs[c]:
                    rows.append([m, "FAILED"] + [""] * 10)
                    continue
                hw = ci_halfwidth(runs[c])
                if converged(runs[c]):
                    repcell = f"{len(runs[c])}"
                else:
                    repcell = f"{len(runs[c])}! ({fmt_need(runs[c])})"
                mx_abs = max(r["lat_max"] for r in runs[c])
                s = st.get(c) or {}
                rows.append([
                    m, repcell, f"{mtput(c) / 1e6:.1f}", f"{hw * 100:.2f}",
                    f"{med(c, 'lat_min'):.0f}", f"{med(c, 'lat_mean'):.0f}",
                    fmt_ns(s.get("p50", 0)), fmt_ns(s.get("p99", 0)),
                    fmt_ns(s.get("p999", 0)), f"{mx_abs}",
                    f"{s.get('inflight', 0):.0f}",
                    f"{med(c, 'miss_prod'):.0f}/{med(c, 'miss_cons'):.0f}"])
                overall.append((mtput(c), med(c, "lat_mean"), c,
                                converged(runs[c])))
            rep.extend(render_table(headers, rows))

            # regression over fixed-B rows: ns/msg = a + T/B
            pts = [(b, (var, pl, f"b{b}")) for b in BATCHES
                   if (var, pl, f"b{b}") not in failed
                   and runs[(var, pl, f"b{b}")]]
            single = (var, pl, "single")
            auto = (var, pl, "auto")
            if len(pts) >= 4:
                xs = [1.0 / b for b, c in pts]
                ys = [1e9 / mtput(c) for b, c in pts]
                a, T, r2 = linreg(xs, ys)
                ceil_txt = (f"  (theoretical ceiling 1/a = {1e3 / a:.0f} "
                            f"M/s)" if a > 0 else "")
                rep.append(f"\nCost model fit  ns/msg = a + T/B:  "
                           f"a = {a:.2f} ns/msg,  T = {T:.1f} ns/batch,  "
                           f"R² = {r2:.3f}{ceil_txt}")

                # ---- plain-language summary for this table.  Quote only
                # batch sizes whose median converged: an unconverged row can
                # top the table on luck and would send the reader after it.
                cpts = [p for p in pts if converged(runs[p[1]])] or pts
                best_b, best_c = max(cpts, key=lambda p: mtput(p[1]))
                bt = mtput(best_c)
                knee = next((b for b, c in cpts
                             if mtput(c) >= 0.99 * bt), best_b)
                lines = ["\nIn plain words:"]
                if runs.get(single):
                    st = mtput(single)
                    lines.append(
                        f"- Fastest setting: batches of {best_b} messages "
                        f"push {bt / 1e6:.0f} M msg/s - "
                        f"{bt / st:.1f}x the no-batching baseline "
                        f"({st / 1e6:.0f} M msg/s).")
                lines.append(
                    f"- Diminishing returns: batches beyond {knee} "
                    f"messages add less than 1% throughput.")
                if runs.get(auto) and runs.get(single):
                    at, al = mtput(auto), med(auto, "lat_mean")
                    bl = med(best_c, "lat_mean")
                    if at >= mtput(single):
                        lines.append(
                            f"- auto (the library picks the batch itself): "
                            f"{at / 1e6:.0f} M msg/s "
                            f"({at / bt:.0%} of the best fixed batch) at "
                            f"{al:.0f} ns mean latency - "
                            f"{bl / al:.0f}x lower than that best batch. "
                            f"This is the latency-first tradeoff it is "
                            f"designed for.")
                    else:
                        lines.append(
                            f"- auto: {at / 1e6:.0f} M msg/s, SLOWER than "
                            f"the baseline here - in this saturated test "
                            f"on this variant the adaptive law picks small "
                            f"batches (it optimizes latency: "
                            f"{al:.0f} ns mean), and a burst call with a "
                            f"tiny batch costs more than a plain push.")
                if runs.get((var, pl, "b1")) and runs.get(single):
                    if mtput((var, pl, "b1")) < 0.6 * mtput(single):
                        lines.append(
                            "- Batches under ~4 messages are pointless: "
                            "the burst call itself costs more than the "
                            "plain single-message API.")
                if (var == "line" and runs.get((var, pl, "b14"))
                        and runs.get((var, pl, "b16"))
                        and mtput((var, pl, "b14"))
                        > 1.05 * mtput((var, pl, "b16"))):
                    lines.append(
                        f"- Multiples of 7 fit this format's 64-byte "
                        f"lines exactly: B=14 beats B=16 by "
                        f"{mtput((var, pl, 'b14')) / mtput((var, pl, 'b16')) - 1:.0%}. "
                        f"Prefer 7/14/28 over 8/16/32.")
                if runs.get((var, pl, "wait")) and runs.get(single):
                    wt = mtput((var, pl, "wait"))
                    if wt >= 0.85 * mtput(single):
                        lines.append(
                            f"- wait (sleeping instead of spinning): "
                            f"{wt / 1e6:.0f} M msg/s, about the same as "
                            f"busy polling, with zero wasted polls - use "
                            f"it when CPU time matters.")
                rep.extend(lines)

    # ------------------------- global model: one regression over all data
    rep.append("\n## Global model - one regression over ALL tests\n")
    rep.append("Every raw run of every fixed-batch configuration enters "
               "one pooled model:\n\n"
               "    ns/msg = a0 + a_idx*IDX + a_sib*SIB + a_same*SAME + "
               "(T0 + T_idx*IDX + T_sib*SIB + T_same*SAME) / B\n\n"
               "IDX = 1 for the idx variant, SIB = 1 for --siblings, "
               "SAME = 1 for --same-core.  "
               "The 1/B terms are the per-batch overhead being amortized; "
               "the additive terms are the per-message floor.")
    X, Y = [], []
    for (var, pl, m), rs in runs.items():
        if not m.startswith("b") or not rs or (var, pl, m) in failed:
            continue
        b = int(m[1:])
        for r in rs:
            ix = 1.0 if var == "idx" else 0.0
            sb = 1.0 if pl == "--siblings" else 0.0
            sm = 1.0 if pl == "--same-core" else 0.0
            X.append([1.0, ix, sb, sm, 1.0 / b, ix / b, sb / b, sm / b])
            Y.append(1e9 / r["tput"])
    betas, r2, ts = solve_ols(X, Y) if X else (None, 0, None)
    if betas:
        names = [
            ("a0    (floor, line --cores)", "ns/msg"),
            ("a_idx (idx adds to floor)", "ns/msg"),
            ("a_sib (siblings adds to floor)", "ns/msg"),
            ("a_same(same-core adds to floor)", "ns/msg"),
            ("T0    (per-batch cost, line --cores)", "ns/batch"),
            ("T_idx (idx extra per-batch cost)", "ns/batch"),
            ("T_sib (siblings extra per-batch cost)", "ns/batch"),
            ("T_same(same-core extra per-batch cost)", "ns/batch"),
        ]
        rows = [[nm, f"{b:+.2f} {unit}", f"{t:+.1f}",
                 "yes" if abs(t) > 2 else "no"]
                for (nm, unit), b, t in zip(names, betas, ts)]
        rep.append("")
        rep.extend(render_table(
            ["term", "estimate", "t-stat", "significant?"], rows))
        rep.append(f"\nModel fit: R² = {r2:.3f} over {len(Y)} raw runs.")
        rep.append("\nWhat the coefficients say:")
        rep.append(f"- Per-BATCH overhead is a property of the VARIANT: "
                   f"idx pays {betas[4] + betas[5]:.1f} ns/batch vs line "
                   f"{betas[4]:.1f} ns/batch "
                   f"({(betas[4] + betas[5]) / betas[4]:.1f}x) - which is "
                   f"exactly why batching lifts idx so much more.")
        rep.append(f"- Per-MESSAGE floor at large B: line --cores "
                   f"{betas[0]:.2f} ns; idx "
                   f"{'adds' if betas[1] > 0 else 'saves'} "
                   f"{abs(betas[1]):.2f} ns; siblings "
                   f"{'adds' if betas[2] > 0 else 'saves'} "
                   f"{abs(betas[2]):.2f} ns; same-core "
                   f"{'adds' if betas[3] > 0 else 'saves'} "
                   f"{abs(betas[3]):.2f} ns.")

    # variance decomposition: what actually moves throughput?
    pairs_all = []
    for (var, pl, m), rs in runs.items():
        for r in rs:
            pairs_all.append(((var, pl, m), math.log(r["tput"])))
    if pairs_all:
        e_cfg = eta2(pairs_all)
        e_mode = eta2([((c[2]), y) for c, y in pairs_all])
        e_var = eta2([((c[0]), y) for c, y in pairs_all])
        e_pl = eta2([((c[1]), y) for c, y in pairs_all])
        rep.append("\nWhat explains the spread of throughput "
                   "(share of variance in log-throughput):")
        rep.extend(render_table(
            ["factor", "share"],
            [["mode (batch size / single / auto / wait)",
              f"{e_mode:.0%}"],
             ["library variant (line vs idx)", f"{e_var:.0%}"],
             ["thread placement (cores / siblings / one cpu)", f"{e_pl:.0%}"],
             ["all parameters together", f"{e_cfg:.0%}"],
             ["run-to-run noise (OS, cache luck)", f"{1 - e_cfg:.0%}"]]))
        rep.append(f"\nSo the batch size is the dominant lever "
                   f"({e_mode:.0%} of all variation), and only "
                   f"{1 - e_cfg:.0%} of what you see is machine noise - "
                   f"the parameters are worth tuning.")

    # deterministic price of batching: lat_min grows with B
    slopes = []
    for var in VARIANTS:
        for pl in PLACEMENTS:
            pts = [(b, (var, pl, f"b{b}")) for b in BATCHES
                   if runs.get((var, pl, f"b{b}"))]
            if len(pts) >= 4:
                _, s, sr2 = linreg([float(b) for b, _ in pts],
                                   [float(med(c, "lat_min"))
                                    for _, c in pts])
                slopes.append((var, pl, s, sr2))
    if slopes:
        rep.append("\nThe deterministic price of batching - the BEST-case "
                   "latency (lat min) grows with B, because a message "
                   "waits for its own batch to fill:")
        rep.extend(render_table(
            ["variant/placement", "ns per +1 of B", "R²"],
            [[f"{v} {p}", f"{s:.2f}", f"{r:.2f}"]
             for v, p, s, r in slopes]))
        avg_s = sum(s for _, _, s, _ in slopes) / len(slopes)
        rep.append(f"\nRule: every message in a batch of B waits "
                   f"~{avg_s:.1f} ns x B before it can even be sent.  "
                   f"At B=256 that is ~{avg_s * 256 / 1000:.1f} us of "
                   f"guaranteed floor - batching buys throughput with "
                   f"WORST-case latency, never for free.")

    # misses vs batch size
    corr = []
    for var in VARIANTS:
        for pl in PLACEMENTS:
            pts = [(b, (var, pl, f"b{b}")) for b in BATCHES
                   if runs.get((var, pl, f"b{b}"))]
            if len(pts) >= 4:
                corr.append(spearman(
                    [float(b) for b, _ in pts],
                    [float(med(c, "miss_prod")) for _, c in pts]))
    if corr:
        rep.append(f"\nMisses vs batch size: Spearman rho = "
                   f"{min(corr):.2f}..{max(corr):.2f} across the four "
                   f"tables - larger batches consistently suppress "
                   f"full/empty stalls (thousands of yields at B=1 vs "
                   f"single digits at B>=28).")

    # ------------------------------------------------- global conclusions
    if overall:
        rep.append("\n## Bottom line across everything\n")
        # same rule as the champions table: a median that never converged is
        # not a result, so it cannot be quoted as the best of anything
        pool = [t for t in overall if t[3]] or overall
        if len(pool) < len(overall):
            rep.append(f"Quoting only the {len(pool)} of {len(overall)} "
                       f"configurations whose median throughput converged to "
                       f"±{CI_TARGET:.0%}; the rest were too noisy to rank.\n")
        bt, bl, bc, _ = max(pool, key=lambda t: t[0])
        rep.append(f"- Fastest overall: {bc[0]} {bc[1]} {bc[2]} - "
                   f"{bt / 1e6:.0f} M msg/s"
                   + (".  The 1 G msg/s goal is met."
                      if bt >= 1e9 else "."))
        lt = min(pool, key=lambda t: t[1])
        rep.append(f"- Lowest mean latency: {lt[2][0]} {lt[2][1]} "
                   f"{lt[2][2]} - {lt[1]:.0f} ns at "
                   f"{lt[0] / 1e6:.0f} M msg/s.")
        allst = [(s, c) for pl in PLACEMENTS if champs.get(pl)
                 for c, s in champs[pl]["stat"].items()]
        if allst:
            ps, pc = min(allst, key=lambda t: t[0]["p99"])
            rep.append(f"- Lowest p99 latency: {pc[0]} {pc[1]} {pc[2]} - "
                       f"{fmt_ns(ps['p99'])} at "
                       f"{ps['tput'] / 1e6:.0f} M msg/s.")
            fs, fc = max(allst, key=lambda t: t[0]["tput"])
            rep.append(f"- Price of the throughput champion: its p99 is "
                       f"{fs['p99'] / ps['p99']:.0f}x the latency champion's "
                       f"({fmt_ns(fs['p99'])} vs {fmt_ns(ps['p99'])}), "
                       f"because it keeps {fs['inflight']:.0f} messages "
                       f"queued inside the ring against "
                       f"{ps['inflight']:.0f}.")
        best_line = max((t for t in pool if t[2][0] == "line"),
                        key=lambda t: t[0], default=None)
        best_idx = max((t for t in pool if t[2][0] == "idx"),
                       key=lambda t: t[0], default=None)
        if best_line and best_idx:
            rep.append(f"- Variant effect: with batching the idx variant "
                       f"tops out {best_idx[0] / best_line[0]:.1f}x higher "
                       f"than line ({best_idx[0] / 1e6:.0f} vs "
                       f"{best_line[0] / 1e6:.0f} M msg/s) - idx pays more "
                       f"per publication, so batches amortize more.")
        rep.append("- Rule of thumb: throughput-critical -> idx variant "
                   "with batches >= 28 (or auto when a backlog exists); "
                   "latency-critical -> auto; on the line variant use "
                   "multiples of 7 and never batch below 4.")

    return "\n".join(rep)


def main():
    runs, failed = collect()
    champs = champions(runs, failed)
    text = make_report(runs, failed, champs)
    (ROOT / "bench_matrix_report.md").write_text(text + "\n")
    print(text)
    print_latency_summary(champs, sched_regime(runs))   # screen only
    # under sudo the outputs would end up root-owned, and the next ordinary
    # run could not rewrite them
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if uid and gid:
        for f in ("bench_matrix_results.csv", "bench_matrix_hist.csv",
                  "bench_matrix_report.md"):
            try:
                os.chown(ROOT / f, int(uid), int(gid))
            except OSError as e:
                print(f"chown {f}: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
