#!/usr/bin/env python3
"""Compare two check_binary.sh output directories, A (reference) and B.

Answers:
  - B against A on every query (top-k: same number of rows);
  - in each directory, target w (logs_w) and k (logs_k) against t (the table
    with LIKE / regexp_full_match), for like_*, rx_* and o_* forms;
  - in each directory, s_rx_* against s_like_* (same semantics).
Plans: B against A with box characters and `Verify:` lines removed; then the
`Verify:` values of B and A per query group.
Exit code 1 if any answer or plan differs; plans of queries listed in
--allow-plan-diff may differ (reported, not counted). --show-plan prints the
raw EXPLAIN of the listed queries from both directories.

Usage: compare_checks.py <dir A> <dir B> [--allow-plan-diff=q1,q2] [--show-plan=q1,q2]
"""

import collections
import glob
import os
import re
import sys

VERIFY = re.compile(r"Verify:\s*([a-z]+(?: [a-z]+)*)")
BOX = re.compile(r"[\u2502\u256d\u256e\u2570\u256f\u2500\u252c\u2534\u2524\u251c]")


def blocks(path):
    out = {}
    cur = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("@@ "):
                cur = line[3:]
                out[cur] = []
            elif cur is not None:
                out[cur].append(line)
    return out


def load(d):
    ans, plan = {}, {}
    for p in sorted(glob.glob(os.path.join(d, "*.answers.log"))):
        for q, lines in blocks(p).items():
            ans[q] = [l for l in lines if l and not l.startswith("Time: ") and l != "Timing is on."]
    for p in sorted(glob.glob(os.path.join(d, "*.explain.log"))):
        for q, lines in blocks(p).items():
            plan[q] = [BOX.sub("", l).strip() for l in lines]
            plan[q] = [l for l in plan[q] if l]
    return ans, plan


def same(q, x, y):
    if q.endswith("_topk"):
        return len(x) == len(y)
    return x == y


def references(label, ans, bad):
    for q, v in sorted(ans.items()):
        m = re.match(r"^((like|rx|o)_.+)_(w|k)$", q)
        if m and (m.group(1) + "_t") in ans and v != ans[m.group(1) + "_t"]:
            bad.append(f"{label}: {q}={v} vs {m.group(1)}_t={ans[m.group(1) + '_t']}")
        if q.startswith("s_like_"):
            r = "s_rx_" + q[len("s_like_"):]
            if r in ans and not same(q, v, ans[r]):
                bad.append(f"{label}: {q}={v} vs {r}={ans[r]}")


def group(q):
    if q.startswith("o_"):
        return "o_" + q.split("_")[2] + "_" + q.rsplit("_", 1)[1]
    if q.startswith("s_"):
        return "s_" + q.split("_")[1] + "_" + q.rsplit("_", 1)[1]
    return q.split("_")[0] + "_" + q.rsplit("_", 1)[1]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    allow = set(filter(None, opts.get("allow-plan-diff", "").split(",")))
    show = [q for q in opts.get("show-plan", "").split(",") if q]
    da, db = args[0], args[1]
    ans_a, plan_a = load(da)
    ans_b, plan_b = load(db)
    bad = []

    missing = sorted(set(ans_a) ^ set(ans_b))
    if missing:
        bad.append(f"queries present in one directory only: {missing}")
    diff = [q for q in sorted(set(ans_a) & set(ans_b)) if not same(q, ans_a[q], ans_b[q])]
    print(f"answers B vs A: {len(set(ans_a) & set(ans_b))} queries, {len(diff)} differ")
    for q in diff:
        bad.append(f"B vs A: {q}: A={ans_a[q]} B={ans_b[q]}")

    before = len(bad)
    references("A", ans_a, bad)
    references("B", ans_b, bad)
    print(f"answers against references (t, s_like/s_rx): {len(bad) - before} mismatches")

    def strip(p):
        out = []
        for l in p:
            l = " ".join(VERIFY.sub("", l).split())
            if l:
                out.append(l)
        return out
    pdiff = [q for q in sorted(set(plan_a) & set(plan_b)) if strip(plan_a[q]) != strip(plan_b[q])]
    print(f"plans B vs A without Verify lines: {len(set(plan_a) & set(plan_b))} queries, {len(pdiff)} differ")
    for q in pdiff:
        if q in allow:
            print(f"plan differs (allowed): {q}")
        else:
            bad.append(f"plan differs: {q}")

    for q in show:
        for label, d in (("A", da), ("B", db)):
            print(f"\nEXPLAIN {q} in {label} ({d}):")
            for p in sorted(glob.glob(os.path.join(d, "*.explain.log"))):
                for l in blocks(p).get(q, []):
                    print("  " + l.rstrip())

    for label, plan in (("A", plan_a), ("B", plan_b)):
        cnt = collections.Counter()
        for q, p in plan.items():
            v = tuple(m.group(1) for l in p for m in VERIFY.finditer(l))
            cnt[(group(q), v)] += 1
        print(f"\nVerify values in {label} (group, values per node): queries")
        for (g, v), c in sorted(cnt.items()):
            print(f"  {g}\t{' / '.join(v) if v else '-'}\t{c}")

    if bad:
        print(f"\n{len(bad)} problems:")
        for b in bad:
            print("  " + b)
        sys.exit(1)
    print("\nno problems")


if __name__ == "__main__":
    main()
