"""Pixel numbers of test shots (web/tools/race_look.mjs --keys "<s>:SHOT", raw RGBA) over a mask: the pixels a debug shot paints
magenta (?glassdbg=1: the intact car glass). No pictures, only counts and means.
usage: shot_mask.py <mask.rgba> [<shot.rgba> ...] [--size 1280x960]
  per shot: mean RGB and luminance over the mask's pixels, the share of bright pixels (luminance > 0.7) and the same over the
  mask's bounding box outside the mask (the car around the glass) for scale"""
import sys

def load(p, w, h):
    b = open(p, 'rb').read()
    if len(b) != w * h * 4: sys.exit(f'{p}: {len(b)} bytes, not {w}x{h} RGBA')
    return b

def main():
    args = [a for a in sys.argv[1:]]
    w, h = 1280, 960
    if '--size' in args:
        i = args.index('--size'); w, h = map(int, args[i + 1].split('x')); del args[i:i + 2]
    if not args: sys.exit(__doc__)
    m = load(args[0], w, h)
    mask = [i for i in range(w * h) if m[i * 4] > 240 and m[i * 4 + 1] < 16 and m[i * 4 + 2] > 240]
    if not mask: print(f'{args[0]}: no magenta pixels'); return
    xs = [i % w for i in mask]; ys = [i // w for i in mask]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    print(f'mask {args[0]}: {len(mask)} magenta pixels, box x {x0}..{x1} y {y0}..{y1}')
    inside = set(mask)
    ring = [y * w + x for y in range(y0, y1 + 1) for x in range(x0, x1 + 1) if y * w + x not in inside]
    def stats(b, idx):
        n = len(idx) or 1
        r = sum(b[i * 4] for i in idx) / n / 255; g = sum(b[i * 4 + 1] for i in idx) / n / 255; bl = sum(b[i * 4 + 2] for i in idx) / n / 255
        lum = [(0.2126 * b[i * 4] + 0.7152 * b[i * 4 + 1] + 0.0722 * b[i * 4 + 2]) / 255 for i in idx]
        lum.sort()
        bright = sum(1 for v in lum if v > 0.7) / n
        med = lum[len(lum) // 2] if lum else 0
        p95 = lum[int(len(lum) * 0.95)] if lum else 0
        return f'mean rgb {r:.3f} {g:.3f} {bl:.3f} lum {sum(lum) / n:.3f} median {med:.3f} p95 {p95:.3f} bright {bright * 100:.1f} %'
    for p in args[1:]:
        b = load(p, w, h)
        print(f'{p}:\n  glass  {stats(b, mask)}\n  around {stats(b, ring)}')

if __name__ == '__main__':
    main()
