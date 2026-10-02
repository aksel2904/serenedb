#!/usr/bin/env python3
"""Summarize an ab_run.sh output directory.

warm: per (threads, bin, query) take the min over reps in each round, then
report the median of the round minima and their min..max.
cold: median over rounds.
Deltas are against A. Also lists the rows where a variant is slower than A
and the queries whose first result differs from A's.
"""

import collections
import statistics
import sys


def main():
    out = sys.argv[1]
    runs = collections.defaultdict(list)
    with open(f"{out}/raw.tsv") as f:
        next(f)
        for line in f:
            mode, rnd, t, b, q, _, ms = line.rstrip("\n").split("\t")
            runs[(mode, t, q, b, rnd)].append(float(ms))

    per = collections.defaultdict(list)
    for (mode, t, q, b, _), v in runs.items():
        per[(mode, t, q, b)].append(min(v) if mode == "warm" else v[0])

    labels = sorted({b for _, _, _, b in per})
    keys = sorted({(m, t, q) for m, t, q, _ in per})
    head = ["mode", "threads", "qid"]
    for b in labels:
        head += [f"{b}_ms", f"{b}_range"]
    head += [f"{b}_vs_A_pct" for b in labels if b != "A"]
    print("\t".join(head))
    for m, t, q in keys:
        cells = []
        med = {}
        for b in labels:
            v = per.get((m, t, q, b))
            if not v:
                cells += ["", ""]
                continue
            med[b] = statistics.median(v)
            cells += [f"{med[b]:.3f}", f"{min(v):.3f}..{max(v):.3f}"]
        for b in labels:
            if b == "A":
                continue
            ok = "A" in med and b in med and med["A"] > 0
            cells.append(f"{(med[b] / med['A'] - 1) * 100:+.1f}" if ok else "")
        print("\t".join([m, t, q] + cells))

    slower = []
    for m, t, q in keys:
        a_v = per.get((m, t, q, "A"))
        if not a_v:
            continue
        a_med = statistics.median(a_v)
        for lab in labels:
            v = per.get((m, t, q, lab))
            if lab == "A" or not v:
                continue
            med_v = statistics.median(v)
            if med_v > a_med:
                apart = min(v) > max(a_v)
                slower.append(((med_v / a_med - 1) * 100, m, t, q, lab,
                               a_med, med_v, apart))
    slower.sort(reverse=True)
    print(f"\n# rows where a variant is slower than A: {len(slower)}"
          " (apart = every value of the variant above every value of A)")
    for d, m, t, q, lab, a_med, med_v, apart in slower:
        print(f"# {m}\tt{t}\t{q}\t{lab}\tA={a_med:.3f}\t{lab}={med_v:.3f}"
              f"\t{d:+.1f}%\t{'apart' if apart else 'overlap'}")

    res = collections.defaultdict(dict)
    try:
        with open(f"{out}/results.tsv") as f:
            for line in f:
                b, t, q, val = line.rstrip("\n").split("\t", 3)
                res[(t, q)].setdefault(b, val)
    except FileNotFoundError:
        return
    diff = [(t, q, b, r["A"], r[b]) for (t, q), r in sorted(res.items())
            for b in labels if b != "A" and "A" in r and b in r
            and r["A"] != r[b] and not q.endswith("_topk")]
    print(f"\n# results differing from A: {len(diff)}")
    for t, q, b, a, v in diff:
        print(f"# t{t} {q}: A={a} {b}={v}")


if __name__ == "__main__":
    main()
