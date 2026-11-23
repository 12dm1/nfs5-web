#!/usr/bin/env python3
"""CARS1: a test fe.txt with N racers (the player + N-1 AI) for a direct race (fe=0).

  python tools/cars_fe.py <game dir>/fe.txt <out dir> --cars 12 [--track coastal] [--laps 2]
                          [--models 993,959,911] [--traffic 0] [--seed 300]

Writes <out dir>/fe.txt (put it in $NFS5_SAVEDIR: the save overlay wins over the game folder).
Cars 1..N-1 copy the stock car[1] block (class=ai) with their own starting position, colour and model.
The file is generated from the player's own install; nothing of it goes into git.
"""
import argparse, os, re

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('out')
ap.add_argument('--cars', type=int, default=8)
ap.add_argument('--track', default=None)
ap.add_argument('--laps', type=int, default=None)
ap.add_argument('--models', default='993')
ap.add_argument('--traffic', type=int, default=None)
ap.add_argument('--traffic-cars', type=int, default=0, help='extra car blocks of class Traffic (models eas1..eas11)')
ap.add_argument('--seed', type=int, default=None)
a = ap.parse_args()

lines = open(a.src, encoding='latin1').read().splitlines()
glob, car0, tmpl = [], [], []
for ln in lines:
    m = re.match(r'\s*car\[(\d+)\]\.(.*)$', ln)
    if not m: glob.append(ln); continue
    i = int(m.group(1))
    if i == 0: car0.append(ln)
    elif i == 1: tmpl.append(m.group(2))

N = a.cars
models = a.models.split(',')
T = a.traffic_cars
sets = {'numopponentracecars': N - 1, 'numcars': N + T, 'numplayerracecars': 1}
if a.track: sets['track_name'] = a.track
if a.laps: sets['num_laps'] = a.laps
if a.traffic is not None: sets['traffic_density'] = a.traffic
if a.seed is not None: sets['rand_seed'] = a.seed
outg = []
for ln in glob:
    k = ln.split('=')[0].strip().lower()
    if k in sets: ln = f'{k} = {sets.pop(k)}'
    outg.append(ln)
outg += [f'{k} = {v}' for k, v in sets.items()]

out = outg + car0
for i in range(1, N + T):
    traffic = i >= N
    for t in tmpl:
        k = t.split('=')[0].strip().lower()
        v = t.split('=', 1)[1].strip() if '=' in t else ''
        if k == 'startingpos': v = str(i)
        elif k == 'personality': v = str(i % 8)
        elif k == 'filename': v = f'eas{(i - N) % 11 + 1}' if traffic else models[(i - 1) % len(models)]
        elif k == 'class' and traffic: v = 'Traffic'
        elif k == 'colorconfig':
            c = -64501 - 997 * i
            v = f'{c}, {c}, {c}, -3757195'
        elif k == 'drivername': v = f'traffic{i}' if traffic else f'bot{i}'
        out.append(f'car[{i}].{k} = {v}')
os.makedirs(a.out, exist_ok=True)
open(os.path.join(a.out, 'fe.txt'), 'w', encoding='latin1', newline='\r\n').write('\n'.join(out) + '\n')
print(f'{N} racers + {T} traffic -> {os.path.join(a.out, "fe.txt")}')
