#!/usr/bin/env python3
"""The data layer the server puts over the game folder (web/server.mjs --mod, default out/mod): the converted High Stakes
tracks of a PC mod install. Hard links where possible, so it takes no space. Stays local: nothing here goes into git.

    python tools/make_mod_layer.py --pc M:/Games/nfspu_nglide [--out out/mod]

The PC wrapper repoints the game at nfs5ext.trk, trackext.viv and zzzwzzz_mod.viv; here they simply take the stock names."""
import argparse, os, re, shutil

KEEP = {'crp', 'dtx', 'edg', 'env', 'fsh', 'jnc', 'map', 'tan', 'hsw', 'aispeed', 'aiprofile', 'scn', 'key', 'cop', 'bin', 'lsp', 'txt'}
ALIASES = {
    'FEData/Data/nfs5ext.trk': 'FEData/Data/nfs5.trk',
    'FEData/Data/nfs5ext.names': 'FEData/Data/nfs5ext.names',
    'GameData/Track/aiprofile_cars.txt': 'GameData/Track/aiprofile_cars.txt',
    'GameData/Sounds/trackext.viv': 'GameData/Sounds/track.viv',
    'GameData/Sounds/zzzwzzz_mod.viv': 'GameData/Sounds/zzzwzzz.viv',
}

def put(src, out, rel):
    dst = os.path.join(out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst): os.remove(dst)
    try: os.link(src, dst)
    except OSError: shutil.copy2(src, dst)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', required=True, help='the PC mod install')
    ap.add_argument('--out', default='out/mod')
    a = ap.parse_args()
    track = os.path.join(a.pc, 'GameData/Track')
    keys = sorted(f[:-4].lower() for f in os.listdir(track) if f.lower().endswith('.hsw'))
    # a track's own names: "aquatica.crp", "aquatica_audio.scn", "aquatica0.lsp"; backups have more dots
    own = re.compile('^(' + '|'.join(map(re.escape, keys)) + r')([a-z0-9_]*)\.([a-z0-9]+)$', re.I)
    n = size = 0
    for d, _, files in [w for r in (track, os.path.join(a.pc, 'FEData/TrackArt')) for w in os.walk(r)]:
        rel = os.path.relpath(d, a.pc).replace(chr(92), '/')
        for f in files:
            m = own.match(f)
            if not m or m.group(3).lower() not in KEEP: continue
            src = os.path.join(d, f)
            put(src, a.out, rel + '/' + f); n += 1; size += os.path.getsize(src)
    for s, t in ALIASES.items():
        src = os.path.join(a.pc, s)
        if os.path.exists(src): put(src, a.out, t); n += 1; size += os.path.getsize(src)
        else: print('missing', s)
    print(f'{len(keys)} tracks, {n} files, {size / 1e6:.0f} MB -> {a.out}')
    print(' '.join(keys))

main()
