// nfs5-web ON1: the online server alone (no game files served): the virtual network's switch at /lan and the NetGather
// lobby, for a public host behind nginx (wss://<host>/lan -> http://127.0.0.1:<port>/lan).
//   node web/online/standalone.mjs [--port 9450] [--host 127.0.0.1] [--data <dir with FEData/Data/nfs5.car + nfs5.trn>]
//                                  [--cars cars.json] [--udp <port for PC players>] [--logs <dir>]
//                                  [--accounts http://127.0.0.1:<web server's --internal-port>] [--origin https://<site>]
// --accounts (CLOUD1): a signed-in player plays under the account's name, and a guest may not use an account's name
// (vnet.mjs); --origin: the site whose pages may connect (behind nginx the Host header is the loopback's).
// The page connects to it with ?lan=wss://<host>/lan (web/src/net/lan.ts). GET /status: players and games as JSON.
// nfs5.car / nfs5.trn are the player's own game data: copy them to the host, never into the repository.
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createOnline } from './vnet.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 9450), HOST = arg('host', '127.0.0.1');
const ACCOUNTS = arg('accounts', '');
// the web server's answers, a name's for a minute (a lookup that fails: a guest, and the name stays as typed)
const takenCache = new Map();
async function taken(name) {
  const k = name.toLowerCase(), c = takenCache.get(k);
  if (c && Date.now() - c.t < 60e3) return c.v;
  const r = await fetch(`${ACCOUNTS}/taken?u=${encodeURIComponent(name)}`, { signal: AbortSignal.timeout(1500) });
  const v = r.ok && (await r.json()).taken === true;
  if (takenCache.size > 5000) takenCache.clear();
  takenCache.set(k, { v, t: Date.now() });
  return v;
}
async function identify(req) {
  if (!req.headers.cookie) return null;
  const r = await fetch(`${ACCOUNTS}/whoami`, { headers: { cookie: req.headers.cookie }, signal: AbortSignal.timeout(1500) });
  return r.ok ? (await r.json()).user ?? null : null;
}
const online = createOnline({ dataDir: arg('data', ''), carsJson: arg('cars', ''), udpPort: +arg('udp', 0),
  logDir: arg('logs', path.join(HERE, '../../out/online')), verbose: process.argv.includes('--net-verbose'),
  identify: ACCOUNTS ? identify : null, taken: ACCOUNTS ? taken : null,
  origins: process.argv.flatMap((a, i) => a === '--origin' ? [process.argv[i + 1]] : []) });

http.createServer((req, res) => {
  const p = new URL(req.url, 'http://x').pathname;
  if (p === '/status' && online.status) {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-cache', 'Access-Control-Allow-Origin': '*' });
    res.end(JSON.stringify(online.status()));
    return;
  }
  res.writeHead(404); res.end();
}).on('upgrade', (req, socket) => {
  socket.on('error', () => socket.destroy());
  let p;
  try { p = new URL(req.url, 'http://x').pathname; } catch { socket.destroy(); return; }
  if (p !== '/lan') { socket.destroy(); return; }
  online.upgrade(req, socket);
}).listen(PORT, HOST, () => console.log(`[online] standalone server on http://${HOST}:${PORT} (/lan, /status)`));
