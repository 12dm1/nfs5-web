#!/usr/bin/env python3
"""The game's EXE: find it, check it, patch it.

    python tools/exe.py prepare <Porsche.exe | nfspu_v35.zip> [out/PORSCHE.EXE]

Accepts the official v3.5 EXE itself or the patch archive that holds it
(nfspu_v35.zip, entry Porsche.exe). Checks the exact build (other versions are
not supported: every address in this project is from this one), applies
patches/exe_v35.txt (each run's old bytes are checked first) and writes the
patched copy that gen.sh lifts. The source file is never modified.
"""
import hashlib
import sys
import zipfile
from pathlib import Path

import pefile

V35_SIZE = 2_129_965
V35_SHA256 = 'e4684948ca9bfb552a0fc0eeb7024a13bfe67f2e40ebb6ba121ae6a81dd166bb'
ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'patches' / 'exe_v35.txt'


def load(src: Path) -> bytes:
    if src.suffix.lower() == '.zip':
        with zipfile.ZipFile(src) as z:
            name = next((n for n in z.namelist() if n.lower() == 'porsche.exe'), None)
            if not name:
                sys.exit(f'{src}: no Porsche.exe inside')
            data = z.read(name)
    else:
        data = src.read_bytes()
    if len(data) != V35_SIZE or hashlib.sha256(data).hexdigest() != V35_SHA256:
        sys.exit(f'{src}: not the English Porsche.exe v3.5 '
                 f'(need {V35_SIZE} bytes, SHA-256 {V35_SHA256}; got {len(data)} bytes, '
                 f'{hashlib.sha256(data).hexdigest()})')
    return data


def patches():
    for n, line in enumerate(PATCHES.read_text().splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        va, old, new = line.split()
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        if len(old) != len(new):
            sys.exit(f'{PATCHES.name}:{n}: old and new differ in length')
        yield n, int(va, 16), old, new


def apply(data: bytes) -> bytes:
    pe = pefile.PE(data=data, fast_load=True)
    ib = pe.OPTIONAL_HEADER.ImageBase
    out = bytearray(data)
    count = 0
    for n, va, old, new in patches():
        off = pe.get_offset_from_rva(va - ib)
        if out[off:off + len(old)] != old:
            sys.exit(f'{PATCHES.name}:{n}: {va:08X} holds {out[off:off + len(old)].hex()}, expected {old.hex()}')
        out[off:off + len(new)] = new
        count += 1
    print(f'{count} patch runs from {PATCHES.relative_to(ROOT)}')
    return bytes(out)


def main():
    if len(sys.argv) < 3 or sys.argv[1] != 'prepare':
        sys.exit(__doc__)
    dst = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / 'out' / 'PORSCHE.EXE'
    data = apply(load(Path(sys.argv[2])))
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    print(f'-> {dst} (SHA-256 {hashlib.sha256(data).hexdigest()[:16]}...)')


if __name__ == '__main__':
    main()
