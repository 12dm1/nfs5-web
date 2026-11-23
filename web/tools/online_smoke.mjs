// ON1 smoke test of the switch + NetGather server, no game: a WebSocket client gets its address, sends a NetGather
// join ('j') to the entry server and an ICMP echo, and checks the answers ('a' result 1, echo reply).
//   node web/tools/online_smoke.mjs [ws://127.0.0.1:8078/lan]
const url = process.argv[2] || 'ws://127.0.0.1:8078/lan';
const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';
let vip = 0, ok = { hello: false, join: false, ping: false };
const frame = (kind, src, dst, sport, dport, payload) => {
  const f = Buffer.alloc(16 + payload.length);
  f[0] = kind; f.writeUInt32BE(src >>> 0, 4); f.writeUInt32BE(dst >>> 0, 8); f.writeUInt16BE(sport, 12); f.writeUInt16BE(dport, 14);
  payload.copy(f, 16); return f;
};
const SERVER = 0x0A4D0001;
ws.onmessage = e => {
  const b = Buffer.from(e.data);
  if (b[0] === 0x40) {
    vip = b.readUInt32BE(4); ok.hello = true;
    console.log('hello, vip', [vip >>> 24, (vip >> 16) & 255, (vip >> 8) & 255, vip & 255].join('.'));
    // join request: 'j' + 0x88 payload (N1 doc: +00 id, +04 sockaddr sent to, +3C "c<account>", +70 "<persona>~3.5~3.5")
    const p = Buffer.alloc(0x88);
    p.writeUInt32BE(7, 0);
    p.writeUInt16LE(2, 4); p.writeUInt16BE(9442, 6); p.writeUInt32BE(SERVER, 8);
    p.write('csmoke', 0x3c, 'latin1'); p.write('Smoke~3.5~3.5', 0x70, 'latin1');
    ws.send(frame(1, vip, SERVER, 50000, 9442, Buffer.concat([Buffer.from([0x6a]), p])));
    const icmp = Buffer.from([8, 0, 0, 0, 0x12, 0x34, 0, 1, 1, 2, 3, 4]);
    ws.send(frame(5, vip, SERVER, 0, 0, icmp));
    return;
  }
  const payload = b.subarray(16);
  if (b[0] === 1 && payload[0] === 0x61) { ok.join = payload.readInt32BE(1 + 0x18) === 1; console.log('join answer, result', payload.readInt32BE(1 + 0x18), 'from port', b.readUInt16BE(12)); }
  if (b[0] === 5 && payload[0] === 0) { ok.ping = true; console.log('echo reply'); }
  if (ok.hello && ok.join && ok.ping) { console.log('PASS'); ws.close(); process.exit(0); }
};
ws.onerror = e => { console.log('FAIL', e.message || e); process.exit(1); };
setTimeout(() => { console.log('FAIL timeout', ok); process.exit(1); }, 5000);
