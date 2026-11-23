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
const ipText = v => `${v >>> 24}.${(v >>> 16) & 255}.${(v >>> 8) & 255}.${v & 255}`;
const ipNum = t => t.split('.').reduce((a, b) => ((a << 8) | (+b & 255)) >>> 0, 0);

export function createOnline({ dataDir, carsJson, udpPort = 0, logDir, verbose = false } = {}) {
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
  let nextHost = 1;
  let udp = null;

  function frame(kind, src, dst, sport, dport, payload) {
    const f = Buffer.alloc(16 + payload.length);
    f[0] = kind; f.writeUInt32BE(src >>> 0, 4); f.writeUInt32BE(dst >>> 0, 8);
    f.writeUInt16BE(sport, 12); f.writeUInt16BE(dport, 14); payload.copy(f, 16);
    return f;
  }
  const ng = createNetGather({
    log, verbose,
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
        ng.datagram(`${ipText(tab.vip)}:${sport}`, { address: ipText(tab.vip), port: sport, tab, serverIp: dst, serverPort: dport }, payload);
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
  function upgrade(req, socket) {
    const key = req.headers['sec-websocket-key'];
    if (!key) { socket.destroy(); return; }
    const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
                 `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    socket.setNoDelay(true);
    let host = nextHost++;
    while (tabs.has((0x0A4D0100 + host) >>> 0)) host = nextHost++;
    const tab = {
      vip: (0x0A4D0000 + 0x100 + (host % 0xFEFF)) >>> 0, open: true,
      send(data) {
        if (!this.open) return;
        const n = data.length;
        const h = n < 126 ? Buffer.from([0x82, n]) : n < 65536 ? Buffer.from([0x82, 126, n >> 8, n & 255]) : null;
        if (!h) return;
        socket.write(Buffer.concat([h, data]));
      },
    };
    tabs.set(tab.vip, tab);
    log(`tab ${ipText(tab.vip)} connected from ${req.socket.remoteAddress} (${tabs.size} tabs)`);
    tab.send(frame(K_HELLO, tab.vip, NETMASK, 0, 0, Buffer.alloc(0)));
    let buf = Buffer.alloc(0);
    const close = why => {
      if (!tab.open) return;
      tab.open = false; tabs.delete(tab.vip);
      for (const key of [...ng.clients.keys()]) if (key.startsWith(ipText(tab.vip) + ':')) ng.drop(key);
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
        if (n > 1 << 20) { close('frame too big'); return; }
        const mask = masked ? buf.subarray(off, off + 4) : null;
        if (masked) off += 4;
        if (buf.length < off + n) return;
        const p = Buffer.from(buf.subarray(off, off + n));
        if (mask) for (let i = 0; i < n; i++) p[i] ^= mask[i & 3];
        buf = buf.subarray(off + n);
        if (op === 2) onFrame(tab, p);
        else if (op === 8) { socket.write(Buffer.from([0x88, 0])); close('closed'); return; }
        else if (op === 9) socket.write(Buffer.concat([Buffer.from([0x8a, p.length]), p]));
      }
    });
    socket.on('close', () => close('socket closed'));
    socket.on('error', e => close(e.code || e.message));
  }

  if (udpPort) {
    udp = dgram.createSocket('udp4');
    udp.on('message', (msg, rinfo) => ng.datagram(`${rinfo.address}:${rinfo.port}`, { address: rinfo.address, port: rinfo.port }, msg));
    udp.on('error', e => log(`UDP error: ${e.message}`));
    udp.bind(udpPort, '0.0.0.0', () => log(`PC players: UDP on 0.0.0.0:${udpPort}`));
  }
  log(`online service up, log ${logFile}`);
  return { upgrade, netgather: ng, status: () => ({ tabs: tabs.size, ...ng.status() }), tabs, close() { clearInterval(timer); if (udp) udp.close(); }, log };
}
export { ipText, ipNum };
