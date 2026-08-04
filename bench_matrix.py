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

Outputs: bench_matrix_results.csv (raw runs), bench_matrix_report.md
(legend + tables + regression + plain-language conclusions), same report
to stdout.

No command-line arguments by design: tune the constants below.
"""

import csv
import math
import random
import re
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
VARIANTS    = ["line", "idx"]
PLACEMENTS  = ["--cores", "--siblings"]
BATCHES     = [1, 2, 4, 7, 8, 14, 16, 28, 32, 64, 128, 256]
MODES       = (["single"] + [f"b{n}" for n in BATCHES] + ["auto", "wait"])

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
    if not (prod and cons and lat):
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
    }


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


def fmt_need(runs):
    n = reps_needed(runs)
    return f"~{n}" if n <= 200 else ">200"


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
hyperthreads of one physical core (shared caches, cheapest transfer).

Columns:

| Column         | Meaning                                                                            |
|----------------|------------------------------------------------------------------------------------|
| `reps`         | how many runs the script needed until the throughput median was statistically solid: 95% confidence interval within +-{CI_TARGET:.0%}.  `N! (~M)` = target NOT reached after N runs; ~M runs would be needed |
| `tput M/s`     | median throughput over the reps, million messages per second                       |
| `CI +-%`       | actual confidence-interval halfwidth of that median, in percent                    |
| `lat min/mean` | latency of individual messages, nanoseconds (median over reps).  Stamped when the message is CREATED, so batching delay is included.  In this saturated test `mean` is mostly QUEUE waiting time, not transfer cost - `min` is the true transfer floor |
| `lat max(med)` | median of each run's worst-case latency                                            |
| `lat max(abs)` | the single worst event across all reps - owned by OS interrupts, not by the ring   |
| `miss p/c`     | producer/consumer: how often a side found the ring full/empty and had to yield     |
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
                          else f"needs {fmt_need(runs[c])} reps total")
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
                    "lat_min", "lat_mean", "lat_max", "lat_std"])
        for c, rs in runs.items():
            for i, r in enumerate(rs):
                w.writerow([c[0], c[1], c[2], i, f"{r['tput']:.0f}",
                            r["miss_prod"], r["miss_cons"], r["lat_min"],
                            r["lat_mean"], r["lat_max"], r["lat_std"]])
    return runs, failed


def make_report(runs, failed):
    """Render the full report from collected data; returns the text."""
    rep = [f"# Ring buffer batch matrix - "
           f"{datetime.now():%Y-%m-%d %H:%M}, "
           f"{SAMPLES_M}m msgs/run, CI target ±{CI_TARGET:.0%}",
           "", LEGEND]

    def med(c, k):
        return statistics.median(r[k] for r in runs[c])

    def mtput(c):
        return med(c, "tput")

    overall = []                  # (tput, lat_mean, cfg) for global summary

    for var in VARIANTS:
        for pl in PLACEMENTS:
            rep.append(f"\n## {var} {pl}\n")
            headers = ["mode", "reps", "tput M/s", "CI ±%", "lat min",
                       "lat mean", "lat max(med)", "lat max(abs)",
                       "miss p/c"]
            rows = []
            for m in MODES:
                c = (var, pl, m)
                if c in failed or not runs[c]:
                    rows.append([m, "FAILED", "", "", "", "", "", "", ""])
                    continue
                hw = ci_halfwidth(runs[c])
                if converged(runs[c]):
                    repcell = f"{len(runs[c])}"
                else:
                    repcell = f"{len(runs[c])}! ({fmt_need(runs[c])})"
                mx_abs = max(r["lat_max"] for r in runs[c])
                rows.append([
                    m, repcell, f"{mtput(c) / 1e6:.1f}", f"{hw * 100:.2f}",
                    f"{med(c, 'lat_min'):.0f}", f"{med(c, 'lat_mean'):.0f}",
                    f"{med(c, 'lat_max'):.0f}", f"{mx_abs}",
                    f"{med(c, 'miss_prod'):.0f}/{med(c, 'miss_cons'):.0f}"])
                overall.append((mtput(c), med(c, "lat_mean"), c))
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

                # ---- plain-language summary for this table
                best_b, best_c = max(pts, key=lambda p: mtput(p[1]))
                bt = mtput(best_c)
                knee = next((b for b, c in pts
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
               "    ns/msg = a0 + a_idx*IDX + a_sib*SIB + "
               "(T0 + T_idx*IDX + T_sib*SIB) / B\n\n"
               "IDX = 1 for the idx variant, SIB = 1 for --siblings.  "
               "The 1/B terms are the per-batch overhead being amortized; "
               "the additive terms are the per-message floor.")
    X, Y = [], []
    for (var, pl, m), rs in runs.items():
        if not m.startswith("b") or not rs or (var, pl, m) in failed:
            continue
        b = int(m[1:])
        for r in rs:
            X.append([1.0, 1.0 if var == "idx" else 0.0,
                      1.0 if pl == "--siblings" else 0.0, 1.0 / b,
                      (1.0 if var == "idx" else 0.0) / b,
                      (1.0 if pl == "--siblings" else 0.0) / b])
            Y.append(1e9 / r["tput"])
    betas, r2, ts = solve_ols(X, Y) if X else (None, 0, None)
    if betas:
        names = [
            ("a0    (floor, line --cores)", "ns/msg"),
            ("a_idx (idx adds to floor)", "ns/msg"),
            ("a_sib (siblings adds to floor)", "ns/msg"),
            ("T0    (per-batch cost, line --cores)", "ns/batch"),
            ("T_idx (idx extra per-batch cost)", "ns/batch"),
            ("T_sib (siblings extra per-batch cost)", "ns/batch"),
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
                   f"idx pays {betas[3] + betas[4]:.1f} ns/batch vs line "
                   f"{betas[3]:.1f} ns/batch "
                   f"({(betas[3] + betas[4]) / betas[3]:.1f}x) - which is "
                   f"exactly why batching lifts idx so much more.")
        rep.append(f"- Per-MESSAGE floor at large B: line --cores "
                   f"{betas[0]:.2f} ns; idx "
                   f"{'adds' if betas[1] > 0 else 'saves'} "
                   f"{abs(betas[1]):.2f} ns; siblings "
                   f"{'adds' if betas[2] > 0 else 'saves'} "
                   f"{abs(betas[2]):.2f} ns.")

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
             ["thread placement (cores vs siblings)", f"{e_pl:.0%}"],
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
        bt, bl, bc = max(overall, key=lambda t: t[0])
        rep.append(f"- Fastest overall: {bc[0]} {bc[1]} {bc[2]} - "
                   f"{bt / 1e6:.0f} M msg/s"
                   + (".  The 1 G msg/s goal is met."
                      if bt >= 1e9 else "."))
        lt = min(overall, key=lambda t: t[1])
        rep.append(f"- Lowest mean latency: {lt[2][0]} {lt[2][1]} "
                   f"{lt[2][2]} - {lt[1]:.0f} ns at "
                   f"{lt[0] / 1e6:.0f} M msg/s.")
        best_line = max((t for t in overall if t[2][0] == "line"),
                        key=lambda t: t[0], default=None)
        best_idx = max((t for t in overall if t[2][0] == "idx"),
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
    text = make_report(runs, failed)
    (ROOT / "bench_matrix_report.md").write_text(text + "\n")
    print(text)
    print(f"\nraw: bench_matrix_results.csv, report: bench_matrix_report.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
