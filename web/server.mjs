// nfs5-web WEB3 development server: page + module + game files, with the COOP/COEP headers SharedArrayBuffer needs.
//   node web/server.mjs [--port 8077] [--data M:/Games/nfs5_clean] [--exe out/PORSCHE.EXE] [--mod out/mod] [--password x]
// /data-manifest + /data/<path>?b=<first block>&n=<blocks> are what web/src/host/io-worker.ts reads. The game files
// come from the player's own folder and are never part of this repository. The patched EXE (./gen.sh) is served
// as /data/PORSCHE.EXE in place of the folder's own.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createOnline } from './online/vnet.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url)), ROOT = path.dirname(HERE);
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8077), DATA = arg('data', 'M:/Games/nfs5_clean'), EXE = arg('exe', path.join(ROOT, 'out/PORSCHE.EXE'));
const PASSWORD = arg('password', '');
const HOST = arg('host', '127.0.0.1');
// ON1 online: the virtual network's switch at /lan + the NetGather lobby (web/online); --udp <port> also lets PC
// players in over real UDP (dx11z net_entry_host/net_entry_port), --cars the OS1 car ladder for the bots' cars
const online = createOnline({ dataDir: DATA, carsJson: arg('cars', 'M:/Games/nfspu_nglide/_mod/netserver/cars.json'),
  udpPort: +arg('udp', 0), logDir: path.join(ROOT, 'out/online'), verbose: process.argv.includes('--net-verbose') });
// A copy of the game without the menu music (GameData/Music is optional at install time): this folder takes its place.
const MUSIC = arg('music', path.join(ROOT, 'out/save/gamedata/music'));
const MOD = arg('mod', path.join(ROOT, 'out/mod'));
const BLOCK = 1 << 20, MAX_RUN = 16;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.json': 'application/json', '.map': 'application/json' };

// The game's file list: the folder, the optional music, the mod layer (tools/make_mod_layer.py) over them, in any letter case.
let files = new Map();
function listFiles() {
  files = new Map();
  const walk = (dir, rel, top) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = rel ? rel + '/' + e.name : e.name, full = path.join(dir, e.name);
      if (e.isDirectory()) { if (!files.has(p.toLowerCase())) files.set(p.toLowerCase(), { p, d: 1 }); walk(full, files.get(p.toLowerCase()).p, top); }
      else if (e.isFile()) {
        if (!rel && top && e.name.toLowerCase() === 'porsche.exe') continue;
        const old = files.get(p.toLowerCase()), st = fs.statSync(full);
        files.set(p.toLowerCase(), { p: old ? old.p : p, s: st.size, m: Math.floor(st.mtimeMs / 1000), full });
      }
    }
  };
  walk(DATA, '', true);
  if (fs.existsSync(MUSIC) && !files.has('gamedata/music')) { files.set('gamedata/music', { p: 'GameData/Music', d: 1 }); walk(MUSIC, 'GameData/Music'); }
  if (fs.existsSync(MOD)) walk(MOD, '');
  const st = fs.statSync(EXE);
  files.set('porsche.exe', { p: 'PORSCHE.EXE', s: st.size, m: Math.floor(st.mtimeMs / 1000), full: EXE });
  return [...files.values()].map(({ p, d, s, m }) => d ? { p, d } : { p, s, m });
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
  if (!files.size) listFiles();
  return files.get(rel.toLowerCase())?.full ?? null;
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
  // GET /test-seed/<name>: a save file of the native test folder (out/save/SaveData), for a test's first start
  if (p.startsWith('/test-seed/')) { sendFile(res, path.join(ROOT, 'out/save/SaveData', path.basename(p)), 'application/octet-stream'); return; }
  // POST /test-shot/<frame>: a frame's raw RGBA, for the text comparisons (no screenshots to look at)
  if (p.startsWith('/test-shot/') && req.method === 'POST') { const f = fs.createWriteStream(path.join(ROOT, 'out/web', 'shot_' + (+p.slice(11)) + '.rgba')); req.pipe(f); req.on('end', () => res.end()); return; }
  const rel = p === '/' ? 'index.html' : p.slice(1);
  const full = path.resolve(HERE, 'dist', rel);
  if (!full.startsWith(path.resolve(HERE, 'dist'))) { res.writeHead(403); res.end(); return; }
  sendFile(res, full, TYPES[path.extname(full)] || 'application/octet-stream', { 'Cache-Control': 'no-cache' });
}).on('upgrade', (req, socket) => {
  if (new URL(req.url, 'http://x').pathname !== '/lan' || !authorized(req)) { socket.destroy(); return; }
  online.upgrade(req, socket);
}).listen(PORT, HOST, () => console.log(`nfs5-web on http://${HOST}:${PORT}/ (data: ${DATA})`));
