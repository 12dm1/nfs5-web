// nfs5-web ON1: the virtual network's switch and the online service in one place.
//
// Every browser tab with the game opens one WebSocket to /lan (web/src/net/lan.ts) and gets a virtual address
// 10.77.1.x. Its frames (big-endian, as nfsu2-web's switch):
//   [0] kind  [1..3] 0  [4..7] source vip  [8..11] destination vip  [12..13] source port  [14..15] destination port
//   [16..] payload
//   kind 1 (NET_KIND_UDP): a datagram. For the entry port (9442) or the server's address it goes to the NetGather
//          server (netgather.mjs, the Internet mode's lobby and races), else to the tab with that address.
//   kind 5 (NET_KIND_LAN_ICMP): an ICMP message; an echo request is answered here, from the address it was sent to.
//   kind 0x40 (to the page only): hello, [4..7] the tab's address, [8..11] the netmask.
// PC players (the PC mod's dx11z with net_entry_host pointing here) reach the same NetGather server over real UDP
// (udpPort), so browsers and PCs share one lobby.
//
// CLOUD1 (docs/tasks/CLOUD1-cloud-saves.md "Online"): a browser signed in to the site plays under its account name, whatever the game's
// profile is called (identify(req): the account of the upgrade's cookie); anyone else plays under the name typed,
// unless that name is an account's (taken(name)), then as "<name>-guest". A join is held (with what follows it from the
// same client) until that is decided. The WebSocket upgrade must come from the site's own pages (Origin), and one
// address may hold at most MAX_TABS_PER_IP connections (MAX_TABS in all).
import crypto from 'node:crypto';
import dgram from 'node:dgram';
import fs from 'node:fs';
import path from 'node:path';
import { createNetGather } from './netgather.mjs';

export const SERVER_VIP = 0x0A4D0001;          // 10.77.0.1 (winsock_web.c NET_SERVER_VIP)
const NETMASK = 0xFFFF0000;
const ENTRY_PORT = 9442;
const CTL_PORT = 9443;              // ON2: text lines between the server and the game's runtime (winsock_web.c online_ctl_*)
const K_UDP = 1, K_ICMP = 5, K_HELLO = 0x40;
const MAX_FRAME = 8192;                // the game's datagrams are at most 1472 bytes + the 16-byte header
const MAX_TABS = 500, MAX_TABS_PER_IP = 16;
const NAME_MAX = 15;                   // the join's info field: "<persona>~<version>~<version>" in 24 bytes
const ipText = v => `${v >>> 24}.${(v >>> 16) & 255}.${(v >>> 8) & 255}.${v & 255}`;
const ipNum = t => t.split('.').reduce((a, b) => ((a << 8) | (+b & 255)) >>> 0, 0);

/** the typed name as the lobby shows it: printable, no '~' (the info field's separator), NAME_MAX at most */
export const cleanName = s => String(s).replace(/[\x00-\x1f\x7f~]/g, '').slice(0, NAME_MAX).trim() || 'Player';
/** the address a connection came from: nginx's X-Real-IP on the loopback, else the socket's; null for a loopback
 *  connection nginx did not name (a test, or an nginx without the header: then no per-address limit, never one
 *  limit for everybody) */
function peerAddress(req) {
  const a = (req.socket && req.socket.remoteAddress || '').replace(/^::ffff:/, '');
  const loop = a === '127.0.0.1' || a === '::1';
  const real = req.headers['x-real-ip'];
  if (loop) return typeof real === 'string' && real ? real : null;
  return a || null;
}

export function createOnline({ dataDir, carsJson, udpPort = 0, logDir, verbose = false, identify = null, taken = null, origins = [] } = {}) {
  fs.mkdirSync(logDir, { recursive: true });
  const logFile = path.join(logDir, `online-${new Date().toISOString().replace(/[:.]/g, '-')}.log`);
  const logStream = fs.createWriteStream(logFile, { flags: 'a' });
  const t0 = Date.now();
  const log = line => {
    const text = `[${((Date.now() - t0) / 1000).toFixed(3).padStart(9)}] ${line}`;
    if (!verbose && /^\s{4}/.test(line)) { logStream.write(text + '\n'); return; }
    console.log('[online] ' + text); logStream.write(text + '\n');
  };
  const tabs = new Map();          // vip -> connection
  const perIp = new Map();         // address -> open connections
  let nextHost = 1;
  let udp = null;

  function frame(kind, src, dst, sport, dport, payload) {
    const f = Buffer.alloc(16 + payload.length);
    f[0] = kind; f.writeUInt32BE(src >>> 0, 4); f.writeUInt32BE(dst >>> 0, 8);
    f.writeUInt16BE(sport, 12); f.writeUInt16BE(dport, 14); payload.copy(f, 16);
    return f;
  }
  // ---- CLOUD1 names: the decision per client key ("ip:port"), and the datagrams held while it is made
  const decided = new Map(), held = new Map();
  function joinTyped(msg) {
    if (msg[0] !== 0x6a || msg.length < 1 + 0x88) return null;
    const b = msg.subarray(1 + 0x70, 1 + 0x70 + 0x18), z = b.indexOf(0);
    return (z < 0 ? b : b.subarray(0, z)).toString('latin1').split('~')[0];
  }
  async function decide(rinfo, typed) {
    const name = cleanName(typed);
    if (rinfo.tab && rinfo.tab.account) return rinfo.tab.account;
    if (taken && await taken(name)) return `${name.slice(0, NAME_MAX - 6)}-guest`;
    return name;
  }
  function toLobby(key, rinfo, msg) {
    const q = held.get(key);
    if (q) { if (q.length < 64) q.push([rinfo, msg]); return; }
    const typed = identify || taken ? joinTyped(msg) : null;
    if (typed !== null) {
      const d = decided.get(key);
      if (!d || d.typed !== typed || d.tab !== rinfo.tab) {
        held.set(key, [[rinfo, msg]]);
        void decide(rinfo, typed).catch(() => cleanName(typed)).then(name => {
          if (decided.size > 20000) decided.clear();
          decided.set(key, { typed, tab: rinfo.tab, name });
          if (name !== cleanName(typed)) log(`  name: "${cleanName(typed)}" plays as "${name}"${rinfo.tab && rinfo.tab.account ? ' (signed in)' : ' (a guest under an account\'s name)'}`);
          const list = held.get(key) || [];
          held.delete(key);
          for (const [r, m] of list) toLobby(key, r, m);
        });
        return;
      }
    }
    ng.datagram(key, rinfo, msg);
  }

  const ng = createNetGather({
    log, verbose,
    personaFor(key, typed) { const d = decided.get(key); return d && d.typed === typed ? d.name : cleanName(typed); },
    carFile: dataDir ? path.join(dataDir, 'FEData', 'Data', 'nfs5.car') : '',
    carsJson: carsJson || '',
    trnFile: dataDir ? path.join(dataDir, 'FEData', 'Data', 'nfs5.trn') : '',
    ctlSend(rinfo, text) { if (rinfo && rinfo.tab && rinfo.tab.open) rinfo.tab.send(frame(K_UDP, SERVER_VIP, rinfo.tab.vip, CTL_PORT, CTL_PORT, Buffer.from(text, 'latin1'))); },
    transmit(rinfo, pkt) {
      if (rinfo.tab) { if (rinfo.tab.open) rinfo.tab.send(frame(K_UDP, rinfo.serverIp, rinfo.tab.vip, rinfo.serverPort, rinfo.port, pkt)); }
      else if (udp) udp.send(pkt, rinfo.port, rinfo.address);
    },
  });
  const timer = setInterval(() => ng.tick(), 1000);

  function onFrame(tab, b) {
    if (b.length < 16) return;
    const kind = b[0], dst = b.readUInt32BE(8), sport = b.readUInt16BE(12), dport = b.readUInt16BE(14);
    const payload = b.subarray(16);
    if (kind === K_UDP) {
      if (dport === CTL_PORT) {
        const c = [...ng.clients.values()].find(x => x.rinfo && x.rinfo.tab === tab);
        if (c) ng.control(c, payload.toString('latin1'));
        return;
      }
      if (dport === ENTRY_PORT || dst === SERVER_VIP) {
        toLobby(`${ipText(tab.vip)}:${sport}`, { address: ipText(tab.vip), port: sport, tab, serverIp: dst, serverPort: dport }, payload);
        return;
      }
      const to = tabs.get(dst);
      if (to && to.open) to.send(frame(K_UDP, tab.vip, dst, sport, dport, payload));
      return;
    }
    if (kind === K_ICMP && payload.length >= 8 && payload[0] === 8) {     // echo request -> echo reply
      const r = Buffer.from(payload);
      r[0] = 0; r[2] = 0; r[3] = 0;
      let sum = 0;
      for (let i = 0; i + 1 < r.length; i += 2) sum += r.readUInt16BE(i);
      if (r.length & 1) sum += r[r.length - 1] << 8;
      while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
      r.writeUInt16BE(~sum & 0xffff, 2);
      tab.send(frame(K_ICMP, dst, tab.vip, 0, 0, r));
    }
  }

  // ---- a minimal WebSocket server (RFC 6455: binary frames, ping, close) ----
  /** the page's origin: none (not a browser), this host's, or one of `origins` (the site's, behind nginx) */
  function originOk(req) {
    const o = req.headers.origin;
    if (o === undefined) return true;
    let host;
    try { host = new URL(o).host; } catch { return false; }
    return host === req.headers.host || origins.some(x => { try { return new URL(x).host === host; } catch { return false; } });
  }
  function upgrade(req, socket) {
    if (!req.headers['sec-websocket-key'] || !originOk(req)) { socket.destroy(); return; }
    const addr = peerAddress(req);
    if (tabs.size >= MAX_TABS || (addr && (perIp.get(addr) || 0) >= MAX_TABS_PER_IP)) {
      socket.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n');
      return;
    }
    if (addr) perIp.set(addr, (perIp.get(addr) || 0) + 1);
    let released = false;
    const release = () => {
      if (released || !addr) return;
      released = true;
      const n = (perIp.get(addr) || 1) - 1;
      if (n > 0) perIp.set(addr, n); else perIp.delete(addr);
    };
    socket.on('close', release);
    if (!identify) { accept(req, socket, null, release); return; }
    let done = false;
    const go = account => { if (done) return; done = true; if (socket.destroyed) { release(); return; } accept(req, socket, account, release); };
    setTimeout(() => go(null), 2000);
    Promise.resolve().then(() => identify(req)).then(a => go(typeof a === 'string' && a ? a : null), () => go(null));
  }
  function accept(req, socket, account, release) {
    const key = req.headers['sec-websocket-key'];
    const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
                 `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    socket.setNoDelay(true);
    let host = nextHost++;
    while (tabs.has((0x0A4D0100 + host) >>> 0)) host = nextHost++;
    const tab = {
      vip: (0x0A4D0000 + 0x100 + (host % 0xFEFF)) >>> 0, open: true, account,
      send(data) {
        if (!this.open) return;
        const n = data.length;
        const h = n < 126 ? Buffer.from([0x82, n]) : n < 65536 ? Buffer.from([0x82, 126, n >> 8, n & 255]) : null;
        if (!h) return;
        socket.write(Buffer.concat([h, data]));
      },
    };
    tabs.set(tab.vip, tab);
    log(`tab ${ipText(tab.vip)} connected${account ? ` (signed in as ${account})` : ''} (${tabs.size} tabs)`);
    tab.send(frame(K_HELLO, tab.vip, NETMASK, 0, 0, Buffer.alloc(0)));
    let buf = Buffer.alloc(0);
    const close = why => {
      if (!tab.open) return;
      tab.open = false; tabs.delete(tab.vip); release();
      for (const key of [...ng.clients.keys()]) if (key.startsWith(ipText(tab.vip) + ':')) ng.drop(key);
      for (const key of [...decided.keys()]) if (key.startsWith(ipText(tab.vip) + ':')) decided.delete(key);
      log(`tab ${ipText(tab.vip)} gone (${why}); ${tabs.size} tabs`);
      socket.destroy();
    };
    socket.on('data', d => {
      buf = buf.length ? Buffer.concat([buf, d]) : d;
      for (;;) {
        if (buf.length < 2) return;
        const op = buf[0] & 15, masked = buf[1] & 128;
        let n = buf[1] & 127, off = 2;
        if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); off = 4; }
        else if (n === 127) { if (buf.length < 10) return; n = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (n > MAX_FRAME) { close('frame too big'); return; }
        const mask = masked ? buf.subarray(off, off + 4) : null;
        if (masked) off += 4;
        if (buf.length < off + n) return;
        const p = Buffer.from(buf.subarray(off, off + n));
        if (mask) for (let i = 0; i < n; i++) p[i] ^= mask[i & 3];
        buf = buf.subarray(off + n);
        if (op === 2) onFrame(tab, p);
        else if (op === 8) { socket.write(Buffer.from([0x88, 0])); close('closed'); return; }
        else if (op === 9 && p.length <= 125) socket.write(Buffer.concat([Buffer.from([0x8a, p.length]), p]));
      }
    });
    socket.on('close', () => close('socket closed'));
    socket.on('error', e => close(e.code || e.message));
  }

  if (udpPort) {
    udp = dgram.createSocket('udp4');
    udp.on('message', (msg, rinfo) => toLobby(`${rinfo.address}:${rinfo.port}`, { address: rinfo.address, port: rinfo.port }, msg));
    udp.on('error', e => log(`UDP error: ${e.message}`));
    udp.bind(udpPort, '0.0.0.0', () => log(`PC players: UDP on 0.0.0.0:${udpPort}`));
  }
  log(`online service up, log ${logFile}`);
  return { upgrade, netgather: ng, status: () => ({ tabs: tabs.size, ...ng.status() }), tabs, close() { clearInterval(timer); if (udp) udp.close(); }, log };
}
export { ipText, ipNum };
