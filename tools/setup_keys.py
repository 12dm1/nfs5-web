#!/usr/bin/env python3
"""CARS1: the setup key table (fe.txt / argv KEY=VALUE tokens) as the start-up code fills it.

  python tools/setup_keys.py out/PORSCHE.EXE [lo hi]
      prints  var  id  name  for every key whose variable lies in [lo, hi) (hex; default: all)

The table at 005D1E40 holds {id, variable, name} triples; 004B1000..004B4C00 fills it with
`mov dword ptr [slot], imm`.  Per-car keys (id >= 0x21E) are stored at var + car*0x580.
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

pe = pefile.PE(sys.argv[1]); IB = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
mem = {}
lo, hi = 0x4B1000, 0x4B4C00
for i in md.disasm(img[lo - IB:hi - IB], lo):
    if i.mnemonic == 'mov' and len(i.operands) == 2 and i.operands[0].type == X86_OP_MEM and i.operands[1].type == X86_OP_IMM:
        m = i.operands[0].mem
        if m.base == 0 and m.index == 0: mem[m.disp & 0xffffffff] = i.operands[1].imm & 0xffffffff


def cstr(a):
    if not (IB <= a < IB + len(img)): return '?'
    return img[a - IB:a - IB + 64].split(b'\0')[0].decode('latin1')


keys = []
for slot in sorted(mem):
    if slot % 12 == 0x5D1E40 % 12 and slot + 8 in mem and slot + 4 in mem:
        keys.append((mem[slot + 4], mem[slot], cstr(mem[slot + 8])))
flo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
fhi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 1 << 32
for var, kid, name in sorted(keys):
    if flo <= var < fhi: print(f'{var:08x}  {kid:#05x}  {name}')
print(f'# {len(keys)} keys', file=sys.stderr)
