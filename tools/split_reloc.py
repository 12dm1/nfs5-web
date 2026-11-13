"""Lift-time operand relocation (nfsu2-web relocates split-screen operands here).

PU (CARS1): the per-car tables listed in runtime/pu_cars.h (CARS_MOVES) move to new guest memory with more
slots.  An operand is moved only when it is an address, that is when one of the EXE's own HIGHLOW fixups
(out/relocs.txt) covers it, and when it points into an old table; it then names the same offset in the new
table.  CARS=0 in the environment turns the moves off (the stock layout, for A/B runs).
"""
import os, re, struct

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOVES = []          # (old, old_len, new, new_len, name)
_RELOC = None       # fixup slot -> the 32-bit value it holds
_IP_EXCEPT = set()  # instructions whose operands stay
_END_IPS = set()    # instructions whose operand equal to an old table's end means the new table's end
_FIX = {}           # (ip, operand) -> (new operand, move name): cross-table indexing
STATS = {}          # name -> operands moved (for the lift log)
CUR_SIZE = 15       # size of the instruction being lifted (set by lift.py)


def _load():
    global _RELOC
    if _RELOC is not None: return
    _RELOC = {}
    if os.environ.get('CARS', '1') == '0': return
    hdr = open(os.path.join(_ROOT, 'runtime', 'pu_cars.h')).read()
    for m in re.finditer(r'X\((\w+),\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u\)', hdr):
        MOVES.append((int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16), m.group(1)))
    _IP_EXCEPT.update(int(x, 16) for x in re.findall(r'K\(0x([0-9A-Fa-f]+)u\)', hdr))
    _END_IPS.update(int(x, 16) for x in re.findall(r'E\(0x([0-9A-Fa-f]+)u\)', hdr))
    by_name = {m[4]: m for m in MOVES}
    for m in re.finditer(r'F\(0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,\s*(\w+),\s*(-?0x[0-9A-Fa-f]+|-?\d+)\)', hdr):
        _FIX[(int(m.group(1), 16), int(m.group(2), 16))] = (by_name[m.group(3)][2] + int(m.group(4), 0), m.group(3))
    exe = os.environ.get('CARS_EXE', os.path.join(_ROOT, 'out', 'PORSCHE.EXE'))
    rel = os.environ.get('RELOCS', os.path.join(_ROOT, 'out', 'relocs.txt'))
    import pefile
    pe = pefile.PE(exe, fast_load=True); ib = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    for ln in open(rel):
        if not ln.strip(): continue
        a = int(ln, 16)
        _RELOC[a] = struct.unpack_from('<I', img, a - ib)[0]


def moved(value):
    """(new address, move name) of an address inside a moved table, else None"""
    _load()
    for old, olen, new, nlen, name in MOVES:
        if old <= value < old + olen: return new + (value - old), name
    return None


def is_address(value, ip):
    """does a fixup slot inside the instruction at ip hold this value?"""
    _load()
    for s in range(ip, ip + CUR_SIZE - 3):
        if _RELOC.get(s) == value: return True
    return False


def operand(value, ip=0, immediate=False):
    _load()
    v = value & 0xffffffff
    f = _FIX.get((ip, v))
    if f is not None:
        STATS[f[1]] = STATS.get(f[1], 0) + 1
        return f"0x{f[0] & 0xffffffff:x}u"
    if ip in _END_IPS:
        for old, olen, new, nlen, name in MOVES:
            if v == old + olen and is_address(v, ip):
                STATS[name] = STATS.get(name, 0) + 1
                return f"0x{new + nlen:x}u"
    if ip and ip not in _IP_EXCEPT:
        n = moved(v)
        if n is not None and is_address(v, ip):
            v = n[0]; STATS[n[1]] = STATS.get(n[1], 0) + 1
    return f"0x{v:x}u"


# CARCHK=1: every memory operand with a register part and a displacement inside the game's data goes through
# chk_ea(address, base, extent, ip) (runtime/pu_cars.c): the extent is the distance from the displacement to the
# next data address the code names (out/car_census.tsv), or to the end of a moved table.
CHECK = os.environ.get('CARCHK', '') == '1'
_CENSUS = None


def check_gap(d, ip):
    """(base, extent) to check an indexed access with displacement d at ip, or None"""
    global _CENSUS
    import bisect
    if _CENSUS is None:
        vals = set()
        for ln in open(os.path.join(_ROOT, 'out', 'car_census.tsv')):
            vals.add(int(ln.split('\t')[3], 16))
        _CENSUS = sorted(vals)
    n = moved(d)
    if n is not None and is_address(d, ip):
        for old, olen, new, nlen, name in MOVES:
            if name == n[1]: return n[0], new + nlen - n[0]
    if not (0x5CB000 <= d < 0x6C2000): return None
    k = bisect.bisect_right(_CENSUS, d)
    nxt = _CENSUS[k] if k < len(_CENSUS) else 0x6C2000
    return d, min(nxt, 0x6C2000) - d


if __name__ == '__main__':
    # python tools/split_reloc.py --check: for every move, the instructions that name the old table's end (an end
    # pointer or the next variable?) and the indexed ones just below its start (a base of index - 1?)
    import sys, collections
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from car_census import load
    _load()
    rows = list(load(os.path.join(_ROOT, 'out', 'car_census.tsv')))
    keep = _IP_EXCEPT
    for old, olen, new, nlen, name in MOVES:
        starts = {m[0] for m in MOVES}
        end = [r for r in rows if r[3] == old + olen and old + olen not in starts and (r[2] == 'imm' or r[4] != '-+-')]
        low = [r for r in rows if old - 0x10 <= r[3] < old and r[4] != '-+-']
        inside = sum(1 for r in rows if old <= r[3] < old + olen)
        print(f'{name:10s} {old:08x}+{olen:x} -> {new:08x}+{nlen:x}: {inside} refs inside')
        for r in end: print(f'    END {r[0]:08x} {r[7]}{"   (kept)" if r[0] in keep else ""}')
        for r in low: print(f'    LOW {r[0]:08x} {r[7]}')
