#!/usr/bin/env python3
"""A/B comparison: library single-push (v2, no batch) vs burst policies (v3).
Usage: v3_ab_compare.py v3_ab_results.csv"""
import csv, sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1])))
for r in rows:
    for k in r:
        if k not in ("build", "policy"):
            r[k] = float(r[k])

def med(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])

cells = defaultdict(list)               # (build, policy, lam) -> rows
for r in rows:
    cells[(r["build"], r["policy"], int(r["rate_M"]))].append(r)

builds = ["line", "idx"]
lams = sorted({int(r["rate_M"]) for r in rows})
fixed = ["b8", "b32", "b128"]

def cell(build, pol, lam):
    rs = cells.get((build, pol, lam))
    if not rs:
        return None
    return {
        "p50":  med([r["p50"] for r in rs]),
        "p90":  med([r["p90"] for r in rs]),
        "drop": med([r["drop_pct"] for r in rs]),
        "thr":  med([r["thr_M"] for r in rs]),
        "chunk": med([r["chunk"] for r in rs]),
    }

for build in builds:
    name = "line (default)" if build == "line" else "indexed"
    print(f"## Build: {name}")
    print()
    print("| lam M/s | single p50 | single drop% | adapt p50 | adapt p90 |"
          " adapt drop% | adapt B | best fixed     | verdict          |")
    print("|---------|------------|--------------|-----------|-----------|"
          "-------------|---------|----------------|------------------|")
    for lam in lams:
        s = cell(build, "single", lam)
        a = cell(build, "adapt", lam)
        if not s or not a:
            continue
        s_ok = s["drop"] <= 1.0
        a_ok = a["drop"] <= 1.0
        s_p50 = f"{s['p50']:.0f}" if s_ok else "UNSTBL"
        # best stable fixed policy
        bf, bfv = "-", None
        for f in fixed:
            c = cell(build, f, lam)
            if c and c["drop"] <= 1.0 and (bfv is None or c["p50"] < bfv):
                bf, bfv = f"{f}: {c['p50']:.0f}", c["p50"]
        if s_ok and a_ok:
            d = a["p50"] - s["p50"]
            verdict = ("equal" if abs(d) <= max(30, 0.3 * s["p50"])
                       else ("batch +%.0fns" % d if d > 0
                             else "batch -%.0fns" % -d))
        elif a_ok and not s_ok:
            verdict = "BATCH RESCUES"
        elif s_ok and not a_ok:
            verdict = "batch FAILS"
        else:
            verdict = "both unstable"
        print(f"| {lam:<7d} | {s_p50:>10s} | {s['drop']:12.2f} |"
              f" {a['p50']:9.0f} | {a['p90']:9.0f} | {a['drop']:11.2f} |"
              f" {a['chunk']:7.1f} | {bf:<14s} | {verdict:<16s} |")
    print()

# stability frontier summary
print("## Stability frontier (max lam with drops <= 1%)")
print()
print("| build | single (no batch) | adapt (dynamic B) | best fixed |")
print("|-------|-------------------|-------------------|------------|")
for build in builds:
    def frontier(pols):
        best = 0
        for lam in lams:
            for p in pols:
                c = cell(build, p, lam)
                if c and c["drop"] <= 1.0:
                    best = max(best, lam)
        return best
    fs = frontier(["single"])
    fa = frontier(["adapt"])
    ff = frontier(fixed)
    print(f"| {build:<5s} | {fs:>17d} | {fa:>17d} | {ff:>10d} |")
