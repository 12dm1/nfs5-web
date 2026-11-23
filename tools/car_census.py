#!/usr/bin/env python3
"""CARS1: every instruction that names a guest data address, for the car-limit census.

  python tools/car_census.py out/PORSCHE.EXE out/car_census.tsv
      writes  ip  func  kind  value  index  scale  text
      kind = mem (memory operand displacement) or imm (immediate operand)
  python tools/car_census.py --range 5e8a00 5e8f00 out/car_census.tsv
      prints the references to a data range, grouped by address

Uses the lifter's own function discovery (gen/all/fntab.c must exist).
"""
import sys, os, re, collections
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

DATA_LO, DATA_HI = 0x5B2000, 0x6E3000


def collect(exe, out):
    from lift import Lifter
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_INVALID
    L = Lifter(exe, comments=False)
    fns = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})u', open('gen/all/fntab.c').read().split('g_fn_addr[]')[1].split('};')[0])]
    rows = {}
    for f in fns:
        try:
            blocks, _ = L.discover(f)
        except Exception as e:
            print('skip', hex(f), e, file=sys.stderr); continue
        for ins in blocks.values():
            for i in ins:
                if i.address in rows: continue
                for o in i.operands:
                    if o.type == X86_OP_MEM:
                        d = o.mem.disp & 0xffffffff
                        if DATA_LO <= d < DATA_HI:
                            idx = i.reg_name(o.mem.index) if o.mem.index != X86_REG_INVALID else '-'
                            base = i.reg_name(o.mem.base) if o.mem.base != X86_REG_INVALID else '-'
                            rows.setdefault(i.address, []).append((f, 'mem', d, base + '+' + idx, o.mem.scale, f'{i.mnemonic} {i.op_str}', o.size))
                    elif o.type == X86_OP_IMM:
                        v = o.imm & 0xffffffff
                        if DATA_LO <= v < DATA_HI and i.mnemonic not in ('call',) and not i.mnemonic.startswith('j'):
                            rows.setdefault(i.address, []).append((f, 'imm', v, '-+-', 0, f'{i.mnemonic} {i.op_str}', o.size))
    with open(out, 'w') as fo:
        for ip in sorted(rows):
            for f, k, v, reg, sc, txt, sz in rows[ip]:
                fo.write(f'{ip:08x}\t{f:08x}\t{k}\t{v:08x}\t{reg}\t{sc}\t{sz}\t{txt}\n')
    print(f'{sum(len(r) for r in rows.values())} references in {len(rows)} instructions -> {out}')


def load(tsv):
    for ln in open(tsv):
        ip, f, k, v, reg, sc, sz, txt = ln.rstrip('\n').split('\t')
        yield int(ip, 16), int(f, 16), k, int(v, 16), reg, int(sc), int(sz), txt


def show_range(lo, hi, tsv):
    by = collections.defaultdict(list)
    for r in load(tsv):
        if lo <= r[3] < hi: by[r[3]].append(r)
    for v in sorted(by):
        rs = by[v]
        fn = sorted({r[1] for r in rs})
        idx = sorted({r[4] for r in rs if r[4].split('+')[1] != '-'})
        print(f'{v:08x}  refs {len(rs):4d}  funcs {len(fn):3d}  idx {",".join(idx)[:40]:40s}  e.g. {rs[0][0]:08x} {rs[0][7]}')


if __name__ == '__main__':
    if sys.argv[1] == '--range':
        show_range(int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4])
    else:
        collect(sys.argv[1], sys.argv[2])
