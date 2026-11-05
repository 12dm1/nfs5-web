"""ASCII of a part of a raw RGBA frame: shot_crop.py a.rgba x0 y0 x1 y1 [cols]  (1280x960 pixels)"""
import sys
b = open(sys.argv[1], 'rb').read(); W = 1280
x0, y0, x1, y1 = map(int, sys.argv[2:6]); cols = int(sys.argv[6]) if len(sys.argv) > 6 else 150
sx = (x1 - x0) / cols; sy = sx * 2; R = " .:-=+*#%@"
for r in range(int((y1 - y0) / sy)):
    line = ''
    for c in range(cols):
        s = n = 0
        for y in range(int(y0 + r * sy), int(y0 + (r + 1) * sy)):
            for x in range(int(x0 + c * sx), int(x0 + (c + 1) * sx)):
                i = (y * W + x) * 4; s += 0.3 * b[i] + 0.59 * b[i + 1] + 0.11 * b[i + 2]; n += 1
        line += R[min(9, int(s / max(n, 1) / 26))]
    print(line.rstrip())
