// nfs5-web WEB3 development server: page + module + game files, with the COOP/COEP headers SharedArrayBuffer needs.
//   node web/server.mjs [--port 8077] [--data M:/Games/nfs5_clean] [--exe out/PORSCHE.EXE] [--password x]
// /data-manifest + /data/<path>?b=<first block>&n=<blocks> are what web/src/host/io-worker.ts reads. The game files
// come from the player's own folder and are never part of this repository. The patched EXE (./gen.sh) is served
// as /data/PORSCHE.EXE in place of the folder's own.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url)), ROOT = path.dirname(HERE);
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8077), DATA = arg('data', 'M:/Games/nfs5_clean'), EXE = arg('exe', path.join(ROOT, 'out/PORSCHE.EXE'));
const PASSWORD = arg('password', '');
const BLOCK = 1 << 20, MAX_RUN = 16;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.json': 'application/json', '.map': 'application/json' };

function listFiles() {
  const out = [];
  const walk = (dir, rel) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = rel ? rel + '/' + e.name : e.name, full = path.join(dir, e.name);
      if (e.isDirectory()) { out.push({ p, d: 1 }); walk(full, p); }
      else if (e.isFile()) {
        if (!rel && e.name.toLowerCase() === 'porsche.exe') continue;
        const st = fs.statSync(full);
        out.push({ p, s: st.size, m: Math.floor(st.mtimeMs / 1000) });
      }
    }
  };
  walk(DATA, '');
  const st = fs.statSync(EXE);
  out.push({ p: 'PORSCHE.EXE', s: st.size, m: Math.floor(st.mtimeMs / 1000) });
  return out;
}
const manifest = () => JSON.stringify({ files: listFiles(), blocks: { url: '/data/', size: BLOCK, max: MAX_RUN } });

function sendFile(res, file, type, extra = {}) {
  fs.readFile(file, (err, b) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': b.length, ...extra });
    res.end(b);
  });
}
function dataPath(rel) {
  if (rel.toLowerCase() === 'porsche.exe') return EXE;
  const full = path.resolve(DATA, rel);
  return full.startsWith(path.resolve(DATA)) ? full : null;
}
function authorized(req) {
  if (!PASSWORD) return true;
  const a = req.headers.authorization || '';
  return a.startsWith('Basic ') && Buffer.from(a.slice(6), 'base64').toString().split(':').slice(1).join(':') === PASSWORD;
}

http.createServer((req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  if (!authorized(req)) { res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="nfs5"' }); res.end(); return; }
  const u = new URL(req.url, 'http://x');
  const p = decodeURIComponent(u.pathname);
  if (p === '/data-manifest') { res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-cache' }); res.end(manifest()); return; }
  if (p.startsWith('/data/')) {
    const full = dataPath(p.slice(6));
    if (!full) { res.writeHead(403); res.end(); return; }
    let st; try { st = fs.statSync(full); } catch { res.writeHead(404); res.end(); return; }
    const b = u.searchParams.get('b');
    if (b !== null) {
      const first = +b, n = Math.min(+u.searchParams.get('n') || 1, MAX_RUN);
      const start = first * BLOCK, end = Math.min(st.size, start + n * BLOCK);
      res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': Math.max(0, end - start) });
      if (end > start) fs.createReadStream(full, { start, end: end - 1 }).pipe(res); else res.end();
      return;
    }
    const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
    if (range) {
      const start = +range[1], end = range[2] ? Math.min(+range[2], st.size - 1) : st.size - 1;
      res.writeHead(206, { 'Content-Type': 'application/octet-stream', 'Content-Length': end - start + 1, 'Content-Range': `bytes ${start}-${end}/${st.size}` });
      fs.createReadStream(full, { start, end }).pipe(res);
      return;
    }
    res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': st.size });
    fs.createReadStream(full).pipe(res);
    return;
  }
  if (p === '/nfs5.wasm' || p === '/nfs5.json') { const f = path.join(ROOT, 'out/web', p.slice(1)); sendFile(res, f, TYPES[path.extname(f)], { 'Cache-Control': 'no-cache' }); return; }
  // POST /test-log: the autotest page's lines (tools/ drive headless Chrome)
  if (p === '/test-log' && req.method === 'POST') { let s = ''; req.on('data', d => s += d); req.on('end', () => { process.stdout.write(s); res.end(); }); return; }
  const rel = p === '/' ? 'index.html' : p.slice(1);
  const full = path.resolve(HERE, 'dist', rel);
  if (!full.startsWith(path.resolve(HERE, 'dist'))) { res.writeHead(403); res.end(); return; }
  sendFile(res, full, TYPES[path.extname(full)] || 'application/octet-stream', { 'Cache-Control': 'no-cache' });
}).listen(PORT, '127.0.0.1', () => console.log(`nfs5-web on http://127.0.0.1:${PORT}/ (data: ${DATA})`));
