#!/usr/bin/env python3
"""Summarize step 0: per-form candidates per block and Initialize cost.

Point path: Initialize(false) once per candidate (StringFetchRow).
Scan path estimate: Initialize(true) once per touched block, measured on the
same blocks during the merge (merge_logs_w.bt.txt).
"""
import re
import statistics
import sys
from pathlib import Path

d = Path(sys.argv[1])
rx = re.compile(r'^@(blk|blkns)\[(\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+)\]: (\d+)')


def load(p):
    cnt, ns = {}, {}
    for line in p.read_text().splitlines():
        m = rx.match(line)
        if not m:
            continue
        kind, flag = m.group(1), int(m.group(2))
        key = tuple(int(x) for x in m.groups()[2:8])
        (cnt if kind == 'blk' else ns)[(flag, key)] = int(m.group(9))
    return cnt, ns


mcnt, mns = load(d / 'merge_logs_w.bt.txt')
scan_ns = {k: mns[(f, k)] / mcnt[(f, k)] for (f, k) in mcnt if f == 1}
blocks = sorted(scan_ns)
modes = {}
for k in blocks:
    modes.setdefault(k[0], []).append(k)
print('# terms column blocks (merge, Initialize(true))')
for mode, ks in sorted(modes.items()):
    rows = [k[1] for k in ks]
    dcs = [k[2] for k in ks]
    us = [scan_ns[k] / 1e3 for k in ks]
    print(f'mode={mode} blocks={len(ks)} rows_total={sum(rows)} '
          f'rows/block min={min(rows)} med={statistics.median(rows)} max={max(rows)} '
          f'dict_count med={statistics.median(dcs)} '
          f'init_true_us med={statistics.median(us):.0f} min={min(us):.0f} max={max(us):.0f}')
print()
print('form\tcand\tblocks\tcand/block_med\tcand/block_min\t'
      'init_false_us\tpoint_init_ms\tscan_init_ms\tratio')
for p in sorted(d.glob('like_*.bt.txt')):
    form = p.name[:-len('.bt.txt')]
    cnt, ns = load(p)
    per_block = {k: c for (f, k), c in cnt.items() if f == 0}
    total = sum(per_block.values())
    point_ns = sum(v for (f, k), v in ns.items() if f == 0)
    missing = [k for k in per_block if k not in scan_ns]
    scan = sum(scan_ns[k] for k in per_block if k in scan_ns)
    cpb = sorted(per_block.values())
    print(f'{form}\t{total}\t{len(per_block)}\t{statistics.median(cpb):.0f}\t'
          f'{cpb[0]}\t{point_ns / total / 1e3:.1f}\t{point_ns / 1e6:.1f}\t'
          f'{scan / 1e6:.1f}\t{point_ns / scan:.1f}'
          + (f'\tmissing={len(missing)}' if missing else ''))
