#!/usr/bin/env python3
"""CARS1: compare two CARCHK reports (runtime/pu_cars.c): the instructions whose indexed access reaches further in
the run with more cars (a table sized for fewer cars), and every access to an old (moved) table.

  python tools/carchk_diff.py <baseline carchk.txt> <more-cars carchk.txt>
"""
import sys, collections


def load(p):
    d = {}
    for ln in open(p):
        if ln.startswith('#'): continue
        k, ip, _b, base, _g, gap, _m, maxoff, _n, n = ln.split()
        d[(k, int(ip, 16))] = (int(base, 16), int(gap, 16), int(maxoff, 16), int(n))
    return d


a, b = load(sys.argv[1]), load(sys.argv[2])
fn = {}
try:
    import re
    addrs = sorted(int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})u', open('gen/all/fntab.c').read().split('g_fn_addr[]')[1].split('};')[0]))
    import bisect
    def func(ip):
        k = bisect.bisect_right(addrs, ip) - 1
        return addrs[k] if k >= 0 else 0
except Exception:
    def func(ip): return 0
rows = []
for key, (base, gap, mo, n) in b.items():
    old = a.get(key)
    if key[0] == 'OLD': rows.append(('OLD', key[1], base, gap, mo, n, None)); continue
    if old is None or mo > old[2]: rows.append(('NEW' if old is None else 'MORE', key[1], base, gap, mo, n, old[2] if old else None))
by = collections.defaultdict(list)
for r in rows: by[r[2]].append(r)
for base in sorted(by):
    for k, ip, base, gap, mo, n, omo in sorted(by[base], key=lambda r: r[1]):
        print(f'{k:4s} {ip:08x} (in {func(ip):08x}) base {base:08x} extent {gap:#x} max offset {mo:#x}'
              + (f' (was {omo:#x})' if omo is not None else '') + f' n {n}')
