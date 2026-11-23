#!/usr/bin/env python3
"""CLOUD1: make sure the nfs5-reforged nginx site passes what the servers' checks need (run by install-web.sh and
install-online.sh on every deploy; changes nothing when the lines are there already).

  location / (-> 127.0.0.1:9460, the game and /cloud/):  proxy_set_header X-Real-IP $remote_addr;
  location = /lan (-> 127.0.0.1:9450):                     proxy_set_header Host $host;  + X-Real-IP

  python3 nginx_headers.py <site.conf>      prints the new file on stdout, exit 0; exit 3 when nothing changes
"""
import re
import sys

WANT = {
    'proxy_pass http://127.0.0.1:9460;': ['proxy_set_header X-Real-IP $remote_addr;'],
    'proxy_pass http://127.0.0.1:9450/lan;': ['proxy_set_header Host $host;', 'proxy_set_header X-Real-IP $remote_addr;'],
}


def block_of(text, pos):
    """the { ... } block around pos: (start, end) offsets"""
    start = text.rfind('{', 0, pos)
    depth, i = 1, start + 1
    while depth and i < len(text):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return start, i


def main():
    text = open(sys.argv[1]).read()
    changed = False
    for anchor, lines in WANT.items():
        pos = text.find(anchor)
        if pos < 0:
            continue
        s, e = block_of(text, pos)
        block = text[s:e]
        missing = [l for l in lines if not re.search(re.escape(l.split(' $')[0]) + r'\s', block)]
        if not missing:
            continue
        line_start = text.rfind('\n', 0, pos) + 1
        indent = text[line_start:pos]
        eol = text.find('\n', pos) + 1
        text = text[:eol] + ''.join(f'{indent}{l}\n' for l in missing) + text[eol:]
        changed = True
    sys.stdout.write(text)
    sys.exit(0 if changed else 3)


main()
