// nfs5-web ON1: the page side of the game's virtual network (runtime/web/winsock_web.c, mailbox layout net_proto.h
// after nfsu2-web). The C side queues datagrams in the TX ring and rings tx_bell; this link sends them over one
// WebSocket to the server's switch (web/online/vnet.mjs) and puts what comes back into the RX ring.
//
// Frames on the WebSocket, big-endian (as nfsu2-web's switch):
//   [0] kind  [1..3] 0  [4..7] source vip  [8..11] destination vip  [12..13] source port  [14..15] destination port
//   [16..] payload
// kind 0x40 from the switch: the address the page got ([4..7] vip, [8..11] netmask).
const H = {                           // byte offsets in the mailbox header (net_proto.h net_mailbox)
  txSlots: 16, rxSlots: 20, txOff: 24, rxOff: 28, mode: 32, txHead: 40, txBell: 44,
  rxTail: 48, want: 56, pageState: 64, vip: 68, netmask: 72, generation: 76,
  txTail: 80, rxHead: 84, rxBell: 88, rxFullDrop: 92, txOfflineDrop: 96,
};
const SLOT_BYTES = 1536, SLOT_HDR = 32, MAX_PAYLOAD = 1472;
const KIND_HELLO = 0x40;
const PAGE_CONNECTING = 1, PAGE_ONLINE = 2, PAGE_OFFLINE = 3;

export interface LanStats { sent: number; received: number; state: string; vip: string }

export function attachLan(memory: WebAssembly.Memory, ptr: number, log: (s: string) => void): LanStats {
  const u32 = new Uint32Array(memory.buffer);
  const i32 = new Int32Array(memory.buffer);
  const w = (off: number) => (ptr + off) >> 2;
  const get = (off: number) => Atomics.load(u32, w(off));
  const set = (off: number, v: number) => Atomics.store(u32, w(off), v >>> 0);
  const txSlots = get(H.txSlots), rxSlots = get(H.rxSlots), txOff = get(H.txOff), rxOff = get(H.rxOff);
  const stats: LanStats = { sent: 0, received: 0, state: 'connecting', vip: '' };
  let ws: WebSocket | null = null;
  let backoff = 500;

  const ipText = (v: number) => `${v >>> 24}.${(v >>> 16) & 255}.${(v >>> 8) & 255}.${v & 255}`;
  function connect() {
    set(H.pageState, PAGE_CONNECTING);
    // ?lan=wss://host/lan: a remote online server (web/online/standalone.mjs), else the page's own server
    const url = new URLSearchParams(location.search).get('lan') || `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/lan`;
    const s = ws = new WebSocket(url);
    s.binaryType = 'arraybuffer';
    s.onopen = () => { backoff = 500; };
    s.onmessage = e => receive(new Uint8Array(e.data as ArrayBuffer));
    s.onclose = () => {
      if (ws !== s) return;
      ws = null; set(H.pageState, PAGE_OFFLINE); stats.state = 'offline';
      log(`[lan] switch connection closed, again in ${backoff} ms`);
      setTimeout(connect, backoff); backoff = Math.min(backoff * 2, 10000);
    };
  }
  function receive(b: Uint8Array) {
    if (b.length < 16) return;
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    if (b[0] === KIND_HELLO) {
      set(H.vip, dv.getUint32(4)); set(H.netmask, dv.getUint32(8));
      set(H.generation, get(H.generation) + 1);
      set(H.pageState, PAGE_ONLINE);
      stats.state = 'online'; stats.vip = ipText(dv.getUint32(4));
      log(`[lan] online as ${stats.vip}`);
      return;
    }
    const n = b.length - 16;
    if (n > MAX_PAYLOAD) return;
    const head = get(H.rxHead), tail = get(H.rxTail);
    if (head - tail >= rxSlots) { set(H.rxFullDrop, get(H.rxFullDrop) + 1); return; }
    const s = ptr + rxOff + (head % rxSlots) * SLOT_BYTES;
    const m = new DataView(memory.buffer, s, SLOT_HDR);
    m.setUint32(0, n, true); m.setUint32(4, b[0], true);
    m.setUint32(8, dv.getUint32(4), true); m.setUint32(12, dv.getUint32(8), true);
    m.setUint16(16, dv.getUint16(12), true); m.setUint16(18, dv.getUint16(14), true);
    m.setFloat64(24, performance.timeOrigin + performance.now(), true);
    new Uint8Array(memory.buffer, s + SLOT_HDR, n).set(b.subarray(16));
    set(H.rxHead, head + 1);
    Atomics.add(i32, w(H.rxBell), 1);
    Atomics.notify(i32, w(H.rxBell));
    stats.received++;
  }
  function flush() {
    let tail = get(H.txTail);
    const head = get(H.txHead);
    for (; tail !== head; tail = (tail + 1) >>> 0) {
      const s = ptr + txOff + (tail % txSlots) * SLOT_BYTES;
      const m = new DataView(memory.buffer, s, SLOT_HDR);
      const n = Math.min(m.getUint32(0, true), MAX_PAYLOAD);
      if (!ws || ws.readyState !== WebSocket.OPEN) { set(H.txOfflineDrop, get(H.txOfflineDrop) + 1); continue; }
      const f = new Uint8Array(16 + n), fv = new DataView(f.buffer);
      f[0] = m.getUint32(4, true);
      fv.setUint32(4, get(H.vip)); fv.setUint32(8, m.getUint32(12, true));
      fv.setUint16(12, m.getUint16(16, true)); fv.setUint16(14, m.getUint16(18, true));
      f.set(new Uint8Array(memory.buffer, s + SLOT_HDR, n), 16);
      ws.send(f);
      stats.sent++;
    }
    set(H.txTail, tail);
  }
  // wait for C's tx_bell; also look every 20 ms (an Atomics.waitAsync wake can be late on some browsers)
  async function pump() {
    for (;;) {
      if (!ws && get(H.want)) connect();
      flush();
      const bell = Atomics.load(i32, w(H.txBell));
      const r = Atomics.waitAsync(i32, w(H.txBell), bell, 20);
      if (r.async) await r.value;
    }
  }
  void pump();
  return stats;
}
