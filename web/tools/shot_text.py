"""Text views of frames: ASCII art of a raw RGBA or a BMP; with two files, also their difference.
usage: shot_text.py a.rgba [b.bmp]   (rgba = 1280x960)"""
import struct, sys
def load(p, W=1280, H=960):
    b = open(p, 'rb').read()
    if b[:2] == b'BM':
        off, _, w, h, _, bpp = struct.unpack('<IIiiHH', b[10:30])
        flip = h > 0; h = abs(h); stride = (w * bpp // 8 + 3) & ~3; n = bpp // 8
        px = []
        for y in range(h):
            r = off + ((h - 1 - y) if flip else y) * stride
            px.append([(b[r + x * n + 2], b[r + x * n + 1], b[r + x * n]) for x in range(w)])
        return w, h, px
    return W, H, [[(b[(y * W + x) * 4], b[(y * W + x) * 4 + 1], b[(y * W + x) * 4 + 2]) for x in range(W)] for y in range(H)]
RAMP = " .:-=+*#%@"
def grid(img, cols=100, rows=40):
    w, h, px = img; out = []
    for r in range(rows):
        row = []
        for c in range(cols):
            x0, x1 = c * w // cols, (c + 1) * w // cols; y0, y1 = r * h // rows, (r + 1) * h // rows
            s = [0, 0, 0]; n = 0
            for y in range(y0, y1, max(1, (y1 - y0) // 6)):
                for x in range(x0, x1, max(1, (x1 - x0) // 6)):
                    p = px[y][x]; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n += 1
            row.append((s[0] / n, s[1] / n, s[2] / n))
        out.append(row)
    return out
def show(g):
    for row in g:
        print(''.join(RAMP[min(9, int((0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2]) / 256 * 10))] for p in row))
a = grid(load(sys.argv[1])); show(a)
if len(sys.argv) > 2:
    b = grid(load(sys.argv[2])); print('---'); show(b)
    d = sum(abs(pa[i] - pb[i]) for ra, rb in zip(a, b) for pa, pb in zip(ra, rb) for i in range(3)) / (100 * 40 * 3)
    print('mean abs difference (0..255) of the 100x40 cells: %.2f' % d)
    print('--- cells off by more than 24:')
    for ra, rb in zip(a, b):
        print(''.join('X' if max(abs(pa[i] - pb[i]) for i in range(3)) > 24 else '.' for pa, pb in zip(ra, rb)))
