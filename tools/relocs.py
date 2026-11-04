#!/usr/bin/env python3
"""PORSCHE.EXE keeps a normal .reloc directory: write its HIGHLOW fixup slots to out/relocs.txt."""
import sys, os, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
IB = pe.OPTIONAL_HEADER.ImageBase
addrs = sorted(IB + e.rva for b in pe.DIRECTORY_ENTRY_BASERELOC for e in b.entries if e.type == 3)
os.makedirs('out', exist_ok=True)
open('out/relocs.txt', 'w').write('\n'.join(f'{a:08x}' for a in addrs))
print(f"-> out/relocs.txt ({len(addrs)} HIGHLOW fixups)")
