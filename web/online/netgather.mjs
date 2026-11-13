// The Internet mode's entry server: EA NetGather session host + the game's lobby protocol, after the PC mod's
// N1 server (M:/Games/nfspu_nglide/_mod/netserver/server.js; protocol notes in _mod/docs/findings/
// N1-internet-entry-server.md). Transport-free: the caller hands in datagrams and sends what comes out, so one
// lobby serves browsers (virtual network over a WebSocket, web/online/vnet.mjs) and PC players (real UDP).
//
//   const ng = createNetGather({ transmit(rinfo, buf), log, carFile, carsJson, verbose })
//   ng.datagram(key, rinfo, buf)   a datagram from a client; key identifies the client's socket
//   ng.tick()                      once a second: keepalives, resends, timeouts
import fs from 'node:fs';
import { loadTournaments, onlineEvents, createTournament } from './tournament.mjs';

export function createNetGather({ transmit, ctlSend = () => {}, log = console.log, carFile = '', carsJson = '', trnFile = '', verbose = false } = {}) {
// ON2 career tournaments (tournament.mjs): the events that can run online
const EVENTS = onlineEvents(loadTournaments(trnFile));
if (EVENTS.length) log(`career tournaments: ${EVENTS.length} events for online (${trnFile})`);
function hexdump(buf, max = 512) {
  const lines = [];
  const n = Math.min(buf.length, max);
  for (let off = 0; off < n; off += 16) {
    const chunk = buf.subarray(off, Math.min(off + 16, n));
    const hex = [...chunk].map(b => b.toString(16).padStart(2, '0')).join(' ');
    const ascii = [...chunk].map(b => (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '.')).join('');
    lines.push(`    ${off.toString(16).padStart(4, '0')}  ${hex.padEnd(47)}  ${ascii}`);
  }
  if (n < buf.length) lines.push(`    ... ${buf.length - n} more bytes`);
  return lines.join('\n');
}
function cstr(buf, off, len) {
  const s = buf.subarray(off, off + len);
  const z = s.indexOf(0);
  return (z >= 0 ? s.subarray(0, z) : s).toString('latin1');
}
function typeName(t) {
  const c = String.fromCharCode(t);
  return /[ -~]/.test(c) ? `'${c}' 0x${t.toString(16)}` : `0x${t.toString(16)}`;
}

const MAX_SLOTS = 8;
const clients = new Map();   // "ip:port" -> { slot, account, persona, lastSeen, rinfo }
function freeSlot() {
  const used = new Set([...clients.values()].map(c => c.slot));
  for (let s = 0; s < MAX_SLOTS; s++) if (!used.has(s)) return s;
  return -1;
}

function send(rinfo, type, payload) {
  const pkt = Buffer.concat([Buffer.from([type]), payload || Buffer.alloc(0)]);
  transmit(rinfo, pkt);
  if (verbose) log(`UDP -> ${rinfo.address}:${rinfo.port} ${typeName(type)} ${pkt.length} bytes\n${hexdump(pkt)}`);
}

// ---- reliable link (llpacket) ----
const SEQ_WRAP = 0xfffc;           // valid sequence numbers 0..0xFFFB
const SEQ_ACK = 0xfffd, SEQ_UNRELIABLE = 0xfffe, SEQ_CLOSE = 0xffff;
const nextSeq = s => (s + 1) % SEQ_WRAP;
function seqDiff(a, b) { return (a - b + SEQ_WRAP) % SEQ_WRAP; }   // a - b in the ring

function linkOf(c) {
  if (!c.link) c.link = { recvLast: 0, sendNext: 1, channel: 0, unacked: [], connected: false };
  return c.link;
}
function frame(l, seq, data) {
  const f = Buffer.alloc(6 + data.length);
  f.writeUInt16BE(l.recvLast, 0);
  f.writeUInt16BE(seq, 2);
  f[4] = l.channel;
  f[5] = data.length;
  data.copy(f, 6);
  return f;
}
function sendLinkRaw(c, f, quiet) {
  const pkt = Buffer.concat([Buffer.from([0x3d]), f]);
  transmit(c.rinfo, pkt);
  if (!quiet && verbose) log(`LINK -> ${c.account} ack=${f.readUInt16BE(0)} seq=${f.readUInt16BE(2)} len=${f[5]}
${hexdump(pkt)}`);
}
function sendAck(c, quiet = true) { sendLinkRaw(c, frame(linkOf(c), SEQ_ACK, Buffer.alloc(0)), quiet); }
// Reliable application data to a client (resent until acknowledged).
function sendReliable(c, data, quiet = false) {
  if (c.bot) return;                   // a bot has no connection
  const l = linkOf(c);
  const seq = l.sendNext;
  l.sendNext = nextSeq(seq);
  const item = { seq, data, sentAt: 0 };
  l.unacked.push(item);
  item.sentAt = Date.now();
  sendLinkRaw(c, frame(l, seq, data), quiet);
}
function sendUnreliable(c, data) { if (c.bot) return; sendLinkRaw(c, frame(linkOf(c), SEQ_UNRELIABLE, data), true); }
function deliver(c, data, reliable, seq) {
  if (verbose && data.length && data[0] >= 0xb0) log(`APP <- ${c.account} ${reliable ? `seq ${seq}` : 'unreliable'} ${data.length} bytes
${hexdump(data)}`);
  onAppMessage(c, data);
}
function onLink(c, msg) {
  const f = msg.subarray(1);
  if (f.length < 6) { log(`  link: short frame ${f.length}`); return; }
  const l = linkOf(c);
  const ack = f.readUInt16BE(0), seq = f.readUInt16BE(2), chan = f[4], len = f[5];
  if (6 + len > f.length) { log(`  link: bad length ${len}/${f.length - 6}`); return; }
  l.channel = chan;
  if (!l.connected) { l.connected = true; log(`  link: ${c.account} connected (channel ${chan})`); }
  if (ack < SEQ_WRAP) {
    const before = l.unacked.length;
    l.unacked = l.unacked.filter(u => seqDiff(ack, u.seq) >= SEQ_WRAP / 2);
    if (l.unacked.length !== before) log(`    link: ${c.account} acked up to ${ack}`);
  }
  if (seq === SEQ_CLOSE && len === 0) { log(`  link: ${c.account} closed the link`); sendAck(c); return; }
  if (seq === SEQ_ACK) { sendAck(c); return; }
  const data = f.subarray(6, 6 + len);
  if (seq === SEQ_UNRELIABLE) { deliver(c, data, false); return; }
  if (seq < SEQ_WRAP) {
    const records = [{ seq, data }];
    let off = 6 + len;
    while (off + 2 <= f.length) {                // piggybacked resends
      const rlen = f[off], delta = f[off + 1];
      if (off + 2 + rlen > f.length) break;
      records.push({ seq: (seq - delta + SEQ_WRAP) % SEQ_WRAP, data: f.subarray(off + 2, off + 2 + rlen) });
      off += 2 + rlen;
    }
    records.sort((a, b) => seqDiff(a.seq, l.recvLast) - seqDiff(b.seq, l.recvLast));
    for (const r of records) {
      if (r.seq === nextSeq(l.recvLast)) { l.recvLast = r.seq; deliver(c, r.data, true, r.seq); }
    }
  }
  sendAck(c);
}
// ---- connection layer (netgathr CONMAN) ----
// 0xFF "hello" (0055B550): [FF][session password\0][name\0][info\0]. The
// receiver marks the peer identified. The game's connection callback
// (00491010) then requires a first connection's name to start with 'p'
// (primary/entry server; 's' = ordinary server, 'c' = client) and checks
// info = "<persona>~<version>~<version>" against "3.5" (0048A700).
const SERVER_PERSONA = 'Server';
const VERSION = '3.5';
function helloFor(c) {
  const name = `p${c.entryAddr}.${c.entryPort}.NfsNet`;
  return Buffer.concat([Buffer.from([0xff, 0x00]), Buffer.from(name + '\0', 'latin1'),
                        Buffer.from(`${SERVER_PERSONA}~${VERSION}~${VERSION}\0`, 'latin1')]);
}
// The client's join (0055CED0) sends the hello and then waits 5 s for a
// control message [FE][result] (0055CEA0 stores the result): the host
// (0055B6D0) answers 1 = accepted, 2 = wrong session password.
// ---- lobby protocol (host 00491010 / client 0048F060) ----
// Message built by 0048C870: [u8 type][u24 BE a][u32 BE b][string\0], every
// field after the type optional (a/b omitted when 0x80000000, no string when
// null). Types seen in the client handler:
//   B0 a=member id, string=persona   member joined the lobby
//   B1 a=member id                    member left
//   B2 a=your id, string=game name    you are in game ...
//   B3 b=...                          race start
//   B8/B9 a=game id, b=flags, string  game list entry
//   BB a=from, b=to, string           chat line
//   C6 a=state                        0x0E = logged in (else: error, disconnect)
const MSG_NONE = 0x80000000;
function lobbyMsg(type, a = MSG_NONE, b = MSG_NONE, text = null) {
  const parts = [Buffer.from([type])];
  if (a !== MSG_NONE) { const x = Buffer.alloc(3); x.writeUIntBE(a & 0xffffff, 0, 3); parts.push(x); }
  if (b !== MSG_NONE) { const x = Buffer.alloc(4); x.writeInt32BE(b | 0, 0); parts.push(x); }
  if (text !== null) parts.push(Buffer.from(text + '\0', 'latin1'));
  return Buffer.concat(parts);
}
let nextMemberId = 1;
function lobbyMembers() { return [...clients.values()].filter(x => x.inLobby && !x.game); }
function enterLobby(c) {
  sendReliable(c, lobbyMsg(0xc6, 0x0e));                                  // logged in
  c.inLobby = true;
  lobbyArrive(c);
  // Server selection screen (serverlist.lay, Comms.ServerList = list +258):
  // one B8 record per game server: a = id, b = high16 flags | low16 players,
  // text = "a.b.c.d" of the NfsNet host. The client ICMP-pings the address
  // (0048C6E0 -> 0055DA10; a row is shown once the ping answers) and
  // "enter..." joins that host over UDP (udpip.lay).
  for (const s of GAME_SERVERS) sendReliable(c, serverEntry(s, c));
  broadcastServerInfo();
  for (const h of chatHistory) {        // what the newcomer missed (the client resolves names by member id)
    const here = lobbyMembers().includes(h.from) && h.from.memberId === h.fromId;
    sendReliable(c, here ? lobbyMsg(0xbb, h.fromId, -1, h.text) : lobbyMsg(0xbb, c.memberId, -1, `[${h.name}] ${h.text}`));
  }
  log(`  lobby: ${c.account} (${c.persona}) entered as member ${c.memberId}; ${lobbyMembers().length} in lobby`);
}
// A player appears in the lobby (00490860 on login, 004925A0 back from a game):
// a fresh member id, B0 of the player to everyone in the lobby *including the
// player*, B0 of every other lobby member to the player, then the game list
// (B4 entries, 0048BAA0).
function lobbyArrive(c) {
  c.memberId = nextMemberId++;
  const others = lobbyMembers().filter(m => m !== c);
  const me = lobbyMsg(0xb0, c.memberId, MSG_NONE, c.persona);
  sendReliable(c, me);
  for (const m of others) sendReliable(m, me);
  for (const m of others) sendReliable(c, lobbyMsg(0xb0, m.memberId, MSG_NONE, m.persona));
  for (const g of games.values()) sendReliable(c, gameEntry(g));
}
// A player moves from the lobby into a game (004924D0): B1 of the player to the
// whole lobby including the player, then B1 of every remaining member to the
// player, so the client's player list is empty before the game's B2/B0 arrive.
function lobbyDepart(c) {
  const others = lobbyMembers().filter(m => m !== c);
  const me = lobbyMsg(0xb1, c.memberId);
  sendReliable(c, me);
  for (const m of others) sendReliable(m, me);
  for (const m of others) sendReliable(c, lobbyMsg(0xb1, m.memberId));
}
// One server row. Its address is the one the client reached us at (a browser's virtual address, or this machine's
// real one for a PC player): the client pings it and then logs in to it (ENTER on serverlist).
const GAME_SERVERS = [{ id: 1 }];
// B8 server entry: b = games << 16 | players (the host's +158 / +154 counters, 00490927)
function serverEntry(s, c) {
  const players = [...clients.values()].filter(x => x.inLobby).length;
  return lobbyMsg(0xb8, s.id, ((games.size & 0xffff) << 16) | (players & 0xffff), s.address || c.entryAddr);
}
function broadcastServerInfo() {
  const all = [...clients.values()].filter(x => x.inLobby);
  for (const sv of GAME_SERVERS) for (const m of all) sendReliable(m, serverEntry(sv, m));
}
// Setup record 0x14 = COMMSPOSITIONAL. With the wrapper's race_modes=1 the
// value is the race mode instead of the netcode mode (stock: 0 input exchange,
// 1 positional).
// ---- PvE bots ------------------------------------------------------------
// A bot is a member of the game with no connection. Every client then treats
// it as an ordinary remote player: it gets a slot, a car index of its own
// (004B4E7B walks the member list against the participant mask) and its car
// state arrives as BE packets with its slot inside - which the host sends.
const BOT_NAMES = ['Hans', 'Klaus', 'Dieter', 'Stefan', 'Jurgen', 'Werner', 'Ralf'];
let nextBotId = 1;
const isBot = m => m.bot === true;
const humans = g => g.members.filter(m => !m.bot);
function botsWanted(g) { return (g.raceMode === RACE_MODE_PVE || g.raceMode === RACE_MODE_CAREER) && !g.racing && g.host; }
// PvE and career tournaments: every free slot
function botsTarget(g) { return MAX_SLOTS - humans(g).length; }
// The bots drive what the host set up for itself until they pick their own cars.
function cloneSetup(g, fromSlot, toSlot) {
  for (const r of [...g.setup.values()]) {
    if (r.slot !== fromSlot || r.bytes.length < 12) continue;
    const bytes = Buffer.from(r.bytes);
    bytes.writeUInt32LE(toSlot, 4);
    const id = bytes.readUInt32LE(0);
    g.setup.set(`${id}:${toSlot}`, { key: `${id}:${toSlot}`, slot: toSlot, bytes });
    const msg = Buffer.concat([Buffer.from([0xbc, 0, 0, 0]), bytes]);
    for (const m of g.members) sendReliable(m, msg);
  }
}
// What a client sends per slot (seen on the wire): the car it drives, its
// colour and the ready flag. Everything else stays at the game's defaults.
const SETUP_CAR = 0x226, SETUP_COLOUR = 0x225, SETUP_READY = 0x255, SETUP_LOADING = 0x256;
// 0x141 is the car's display name for a slot: [id][index][string padded to 4].
// The player list shows this, not the car id, so a bot needs it too.
// Record table of 004B4B80 (entries {id, address, name} at 005D1E40):
//   0x13D .FILENAME  - the car model the race loads; this is what makes the
//                      car appear, the id alone does not
//   0x141 .FEMULTIPLAYERCARNAME - the name the player list shows
//   0x226 .FEMULTIPLAYERCARID   - the car id of the slot
const SETUP_CARNAME = 0x141, SETUP_FILENAME = 0x13d;
// nfs5.car: 0x670 per record, +0 id, +2 name, +1F4 kind (0 racer, 1 traffic, 2 cop).
const carNames = new Map();
try {
  const data = fs.readFileSync(carFile);
  for (let o = 0; o + 0x670 <= data.length; o += 0x670) {
    const id = data.readUInt16LE(o), end = data.indexOf(0, o + 2);
    const mEnd = data.indexOf(0, o + 0x7f);
    carNames.set(id, { name: data.toString('latin1', o + 2, end < 0 ? o + 2 : end),
                       model: data.toString('latin1', o + 0x7f, mEnd < 0 ? o + 0x7f : mEnd),
                       kind: data.readInt16LE(o + 0x1f4) });
  }
  log(`car table: ${carNames.size} cars`);
} catch (e) { log(`car table: ${e.message}`); }
function setupString(g, id, slot, text) {
  const body = Buffer.from(text + '\0', 'latin1');
  const pad = Buffer.alloc(Math.ceil(body.length / 4) * 4);
  body.copy(pad);
  const bytes = Buffer.concat([Buffer.alloc(8), pad]);
  bytes.writeUInt32LE(id, 0); bytes.writeUInt32LE(slot, 4);
  g.setup.set(`${id}:${slot}`, { key: `${id}:${slot}`, slot, bytes });
  const msg = Buffer.concat([Buffer.from([0xbc, 0, 0, 0]), bytes]);
  for (const m of g.members) sendReliable(m, msg);
}
// cars.json: every racer of nfs5.car with the OS1 performance index
// (sqrt(hp per tonne * top speed)), stock and fully tuned, sorted by stock.
let carLadder = [];
try {
  carLadder = JSON.parse(fs.readFileSync(carsJson, 'utf8'));
  log(`car ladder: ${carLadder.length} cars ${carLadder[0].stock}..${carLadder[carLadder.length - 1].stock}`);
} catch (e) { log(`car ladder: ${e.message}`); }
function carIndexOf(id) { const c = carLadder.find(x => x.id === id); return c ? c.stock : null; }
// The bots get the cars closest to what the players drive (the OS1 "similar"
// choice, one car per name, the players' own cars left out).
function assignBotCars(g) {
  const bots = g.members.filter(isBot);
  if (bots.length && g.tournament) {          // the event's own opponents, in slot order
    const opp = g.tournament.def.opponents;
    bots.sort((a, b) => a.gameSlot - b.gameSlot).forEach((b, i) => { const o = opp[i % opp.length]; if (o && b.carId !== o.car) setBotCar(g, b, o.car); });
    return;
  }
  if (!bots.length || !carLadder.length) return;
  const mine = humans(g).map(m => {
    const r = g.setup.get(`${SETUP_CAR}:${m.gameSlot}`);
    return r && r.bytes.length >= 12 ? r.bytes.readInt32LE(8) : null;
  }).filter(id => id !== null);
  const values = mine.map(carIndexOf).filter(v => v !== null);
  const ref = values.length ? values.reduce((a, b) => a + b, 0) / values.length : null;
  if (ref === null) return;
  const seen = new Set(), pool = [];
  for (const c of carLadder) {
    if (mine.includes(c.id) || seen.has(c.name)) continue;
    seen.add(c.name); pool.push(c);
  }
  pool.sort((a, b) => Math.abs(a.stock - ref) - Math.abs(b.stock - ref));
  // Stable: the nearest cars in ladder order, so nothing is re-sent while the
  // players are still choosing.
  const picked = pool.slice(0, bots.length).sort((a, b) => a.stock - b.stock);
  let changed = 0;
  bots.forEach((b, i) => { if (picked[i] && b.carId !== picked[i].id) { setBotCar(g, b, picked[i].id); ++changed; } });
  if (changed) log(`  game #${g.id}: bot cars around ${ref.toFixed(0)}: ${picked.map(c => c.name).join(', ')}`);
}
function setBotCar(g, b, carId) {
  b.carId = carId;
  setupRecord(g, SETUP_CAR, b.gameSlot, carId);
  const c = carNames.get(carId);
  if (c) {
    setupString(g, SETUP_CARNAME, b.gameSlot, c.name);
    setupString(g, SETUP_FILENAME, b.gameSlot, c.model);
  }
}
const DEFAULT_BOT_CAR = 102;
const BOT_COLOURS = [-1, -16777216, -65536, -16776961, -16744448, -256, -23296, -8388480];
function hostSetup(g, id) {
  const r = g.host && g.setup.get(`${id}:${g.host.gameSlot}`);
  return r && r.bytes.length >= 12 ? r.bytes.readInt32LE(8) : null;
}
function setupRecord(g, id, slot, value) {
  const bytes = Buffer.alloc(12);
  bytes.writeUInt32LE(id, 0); bytes.writeUInt32LE(slot, 4); bytes.writeInt32LE(value, 8);
  g.setup.set(`${id}:${slot}`, { key: `${id}:${slot}`, slot, bytes });
  const msg = Buffer.concat([Buffer.from([0xbc, 0, 0, 0]), bytes]);
  for (const m of g.members) sendReliable(m, msg);
}
function addBot(g, slot) {
  const b = { bot: true, account: `bot${nextBotId++}`, persona: BOT_NAMES[slot % BOT_NAMES.length],
              game: g, gameSlot: slot, ready: false, raceGone: false, gotInput: false, pingValue: 0,
              carId: hostSetup(g, SETUP_CAR) || DEFAULT_BOT_CAR };
  g.members.push(b);
  for (const m of g.members) sendReliable(m, lobbyMsg(0xb0, slot, MSG_NONE, b.persona));
  if (g.host) cloneSetup(g, g.host.gameSlot, slot);
  setBotCar(g, b, b.carId);
  setupRecord(g, SETUP_COLOUR, slot, BOT_COLOURS[slot % BOT_COLOURS.length]);
  setupRecord(g, SETUP_READY, slot, 1);
  return b;
}
function removeBot(g, b) {
  g.members = g.members.filter(m => m !== b);
  for (const [k, r] of g.setup) if (r.slot === b.gameSlot) g.setup.delete(k);
  for (const m of g.members) sendReliable(m, lobbyMsg(0xb1, b.gameSlot));
}
// 004B4E7B hands out car indices in member-list order, and the loading screen
// reads the local player's car before the whole field is built - so a human
// announced after the bots gets a high index and reads a car that is not there
// yet. The bots are stateless, so they are re-announced after every human.
function rebuildBots(g) {
  for (const b of g.members.filter(isBot)) removeBot(g, b);
  syncBots(g);
}
// Bots take the free slots in order: 004B4E7B hands out car indices densely in
// member order, and the per-slot setup records are indexed by slot, so the two
// only line up when the members sit in slots 0..n without a gap.
function syncBots(g) {
  if (g.racing) return;                // never touch the field of a running race
  const before = g.members.filter(isBot).length;
  if (!botsWanted(g)) {
    for (const b of g.members.filter(isBot)) removeBot(g, b);
  } else {
    const used = new Set(g.members.map(m => m.gameSlot));
    for (const x of g.members.filter(isBot).slice(botsTarget(g))) removeBot(g, x);
    for (let slot = 0; slot < MAX_SLOTS && g.members.length < MAX_SLOTS && g.members.filter(isBot).length < botsTarget(g); ++slot) {
      if (used.has(slot)) continue;
      addBot(g, slot); used.add(slot);
    }
  }
  const after = g.members.filter(isBot).length;
  if (after) assignBotCars(g);
  if (after !== before) {
    log(`  game #${g.id}: ${after} bots (${humans(g).length} players)`);
    broadcastGame(g);
  }
}
const RACE_MODE_PVE = 1;
const RACE_MODE_CAREER = 2;
const SETUP_RACE_MODE = 0x14;
const RACE_MODES = ['PvP', 'PvE', 'career'];
const CHAT_HISTORY = 10;
const chatHistory = [];                 // { from, fromId, name, text } of lobby "to all" lines
const cp1251 = new TextDecoder('windows-1251');
function showText(t) { return cp1251.decode(Buffer.from(t, 'latin1')); }
function leaveLobby(c) {
  if (!c.inLobby) return;
  const inGame = !!c.game;
  leaveGame(c, false);
  c.inLobby = false;
  if (!inGame) for (const m of lobbyMembers()) sendReliable(m, lobbyMsg(0xb1, c.memberId));
  broadcastServerInfo();
  log(`  lobby: ${c.account} left; ${lobbyMembers().length} in lobby`);
}
function onAppMessage(c, data) {
  if (data[0] === 0xff) {
    const parts = data.subarray(1).toString('latin1').split('\0');
    log(`  hello from ${c.account}: password="${parts[0]}" name="${parts[1]}" info="${parts[2]}"`);
    if (!c.helloSent) {
      c.helloSent = true;
      sendReliable(c, Buffer.from([0xfe, 0x01]));   // join accepted
      enterLobby(c);
    }
    return;
  }
  const type = data[0];
  if (type < 0xb0) { onInputRecord(c, data); return; }
  switch (type) {
    case 0xbb: {                      // chat: [BB][u24 to][text\0]; to = 0xFFFFFF all, 0xFFFFFE system
      const to = data.readUIntBE(1, 3);
      const text = cstr(data, 4, data.length - 4);
      log(`  chat <- ${c.account} to ${to === 0xffffff ? 'all' : to === 0xfffffe ? 'system' : to}: "${showText(text)}"`);
      if (to === 0xfffffe) return;    // the client's own console noise (file names)
      // Client display (004C24F0): to == -1 -> "<from>: text", to == own id ->
      // whisper, else "<from> whispers to <to>"; names resolved by member id.
      const all = to === 0xffffff;
      if (c.game) {
        const line = lobbyMsg(0xbb, c.gameSlot, all ? -1 : to, text);
        for (const m of c.game.members) if (all || m.gameSlot === to || m === c) sendReliable(m, line);
        return;
      }
      const line = lobbyMsg(0xbb, c.memberId, all ? -1 : to, text);
      for (const m of lobbyMembers()) if (all || m.memberId === to || m === c) sendReliable(m, line);
      if (all) {
        chatHistory.push({ from: c, fromId: c.memberId, name: c.persona, text });
        if (chatHistory.length > CHAT_HISTORY) chatHistory.shift();
      }
      return;
    }
    case 0xb4: {                      // create / update game: [B4][u24 0][session record 0x4C]
      const rec = data.subarray(4, 4 + 0x4c);
      const info = cstr(rec, 0x14, 0x34);           // "name~flags" as the client typed it
      if (c.game && c.game.host === c) {            // settings changed (00491010: FUN_0048BA70)
        const g = c.game;
        g.info = info; g.name = info.split('~')[0]; g.record = Buffer.from(rec);
        log(`  game #${g.id} updated by ${c.account}: "${info}"`);
        const upd = Buffer.concat([Buffer.from([0xb4]), u24(g.id), rec]);
        for (const m of g.members) sendReliable(m, upd);
        broadcastGame(g);
        return;
      }
      const game = { id: nextGameId++, host: c, name: info.split('~')[0], info, record: Buffer.from(rec), members: [],
                     setup: new Map() };
      games.set(game.id, game);
      log(`  game created by ${c.account}: #${game.id} "${game.name}"`);
      sendReliable(c, Buffer.concat([Buffer.from([0xb4]), u24(game.id), rec]));   // creator hosts it (0048B9B0)
      joinGame(c, game);
      broadcastServerInfo();
      return;
    }
    case 0xb6: {                      // join game: [B6][u24 game id]
      const g = games.get(data.readUIntBE(1, 3));
      if (!g) { log(`  join game: unknown id`); sendReliable(c, lobbyMsg(0xc6, 0x0a)); return; }
      if (humans(g).length >= MAX_SLOTS || g.racing) { sendReliable(c, lobbyMsg(0xc6, 0x0b)); return; }   // Game Full (or racing)
      joinGame(c, g);
      return;
    }
    case 0xba: {                      // leave game
      leaveGame(c);
      return;
    }
    case 0xbc: {                      // setup records (004B4B80): relay to the whole game (0048C050) and remember
      if (!c.game) return;
      for (const m of c.game.members) sendReliable(m, data);
      for (const r of parseSetup(data, 4)) {
        c.game.setup.set(r.key, r);
        // Loading progress (1..9 per slot): the loading screen waits for every
        // participant, so each bot is moved along with the fastest player.
        if (r.slot !== null && r.bytes.length >= 12 && r.bytes.readUInt32LE(0) === SETUP_LOADING) {
          const v = r.bytes.readInt32LE(8);
          if (v > (c.game.loadMax || 0)) {
            c.game.loadMax = v;
            for (const b of c.game.members.filter(isBot)) setupRecord(c.game, SETUP_LOADING, b.gameSlot, v);
          }
        }
        // The host's car is what the bots drive until they pick their own.
        if (r.slot !== null && r.bytes.length >= 12 &&
            r.bytes.readUInt32LE(0) === SETUP_CAR && !c.game.racing) {
          assignBotCars(c.game);
        }
        if (r.key === String(SETUP_RACE_MODE) && r.bytes.length >= 8) {
          const v = r.bytes.readUInt32LE(4);
          if (c.game.raceMode !== v) {
            c.game.raceMode = v;
            log(`  game #${c.game.id}: race mode -> ${RACE_MODES[v] || v}`);
            careerMode(c.game);
            syncBots(c.game);
            broadcastGame(c.game);
          }
        }
      }
      return;
    }
    case 0xbd: {                      // start/end barrier toggle: relay to everyone incl. the sender (0048C0F0)
      const g = c.game;
      if (!g) return;
      for (const m of g.members) sendReliable(m, data);
      // "Wait to go high1/high2/low1/low2": two waves, bit = slot in the first
      // and slot + 8 in the second. Each bot is toggled once per wave, with the
      // same phase as the player that opened it.
      if (!g.bdWave) {
        g.bdWave = new Set();
        const phase = data.readUIntBE(1, 3) - c.gameSlot;
        for (const b of g.members.filter(isBot))
          for (const m of g.members) sendReliable(m, lobbyMsg(0xbd, b.gameSlot + phase));
      }
      g.bdWave.add(c.gameSlot);
      if (g.bdWave.size >= humans(g).length) g.bdWave = null;
      return;
    }
    case 0xbe: {                      // car state: to the others only, unreliable (0048C170). Never echo it:
      // the client copies BE[8..11] into the member's +0x48 (race time), which
      // would overwrite its own finish time from B7.
      if (c.game) for (const m of c.game.members) if (m !== c) sendUnreliable(m, data);
      return;
    }
    case 0xbf: case 0xc0: case 0xc1: case 0xc2: {   // relay to the others (0048C0F0)
      if (c.game) for (const m of c.game.members) if (m !== c) sendReliable(m, data);
      return;
    }
    case 0xc3: {                      // ping (00491661 / 0048C1D0): [C3][u24 id][i32 own rtt]
      // The client asks about one member (lobby: member id, game: slot) at most once
      // a second and reports its own last round-trip time (its +148). The host
      // stores that value and answers the *sender only* with the asked member's
      // value; the client takes now - request time as its new rtt (00490600).
      const id = data.readUIntBE(1, 3), value = data.readInt32BE(4);
      c.pingValue = value;
      const t = c.game ? c.game.members.find(m => m.gameSlot === id) : lobbyMembers().find(m => m.memberId === id);
      if (t) sendReliable(c, lobbyMsg(0xc3, id, t.pingValue || 0), true);
      return;
    }
    case 0xc5: {                      // race start (0048BFB0): C5 0,tick to the game once, then the input frames flow
      const g = c.game;
      if (!g || !g.racing) return;
      c.ready = true;
      if (!g.startTick) {
        g.startTick = Date.now() & 0xffffffff;
        for (const m of g.members) sendReliable(m, lobbyMsg(0xc5, 0, g.startTick));
        log(`  game #${g.id}: race running`);
      }
      startFramesIfReady(g);
      return;
    }
    case 0xb7: {                      // [B7][u24 a][i32 b], host side 004929B0 -> 0048C220:
      // a = -2 "Returning to frontend" (player is out of the race), a = -1
      // "Leaving game", a >= 0 finish: a = race time, b = best lap (1/64 s).
      // The host relays B7 slot,a to the game; when every racer is out
      // (a = -2) the race is over and the game is back in its setup screen.
      const a = (data.readUIntBE(1, 3) << 8) >> 8, b = data.readInt32BE(4);
      const g = c.game;
      if (!g) return;
      for (const m of g.members) sendReliable(m, lobbyMsg(0xb7, c.gameSlot, a));
      if (a === -2) {
        c.raceGone = true;
        log(`  game #${g.id}: ${c.account} returning to frontend`);
        if (g.racing && humans(g).every(m => m.raceGone)) { stopRace(g); log(`  game #${g.id}: race over`); }
      } else if (a === -1) {
        log(`  game #${g.id}: ${c.account} leaving the race`);
      } else {
        log(`  game #${g.id}: ${c.account} finished in ${fmtTime(a)} (best lap ${fmtTime(b)})`);
      }
      return;
    }
    case 0xb3: {                      // race start pressed by the host (00491010 -> 0048BF10)
      if (c.game && c.game.host === c) startRace(c.game);
      return;
    }
    default: {
      const a = data.length >= 4 ? data.readUIntBE(1, 3) : null;
      const b = data.length >= 8 ? data.readInt32BE(4) : null;
      const text = data.length > 8 ? cstr(data, 8, data.length - 8) : '';
      log(`  lobby <- ${c.account}: type 0x${type.toString(16)} a=${a} b=${b} text="${text}" (not handled yet)`);
    }
  }
}
// ---- race (0048B6B0): C4 clock every second, then the input-exchange frame
// ring: 8 slots x 3 bytes per frame; a player's frame input arrives as a raw
// record (first byte < 0xB0, 0048A2C0 -> 0048C2B0: 3 bytes into its slot);
// the host advances the ring 16x/s (copying the previous frame, absent
// players get 08 00 1F) and sends each player the frames it has not seen
// yet, reliably, up to 32x/s.
function startRace(g) {
  if (g.racing) return;
  g.bdWave = null;
  // Last chance to pick the bots' cars: by now every player has chosen one.
  g.loadMax = 0;
  assignBotCars(g);
  for (const b of g.members.filter(isBot)) setupRecord(g, SETUP_READY, b.gameSlot, 1);
  g.racing = true; g.startTick = 0;
  g.frames = [Buffer.from([8,0,0x1f, 8,0,0x1f, 8,0,0x1f, 8,0,0x1f, 8,0,0x1f, 8,0,0x1f, 8,0,0x1f, 8,0,0x1f])];
  for (const m of g.members) { g.frames[0][m.gameSlot * 3] = 0; g.frames[0][m.gameSlot * 3 + 2] = 0; }
  for (const m of g.members) { m.ready = false; m.raceGone = false; m.gotInput = false; }
  const tick = Date.now() & 0xffffffff;
  for (const m of g.members) sendReliable(m, lobbyMsg(0xb3, 0, tick));
  g.clock = setInterval(() => {
    const t = Date.now() & 0xffffffff;
    for (const m of g.members) sendUnreliable(m, lobbyMsg(0xc4, 0, t));
  }, 1000);
  if (g.tournament && !g.tournament.done) for (const m of g.members) g.tournament.enter(m.gameSlot, m.persona, isBot(m));
  log(`  game #${g.id}: race start`);
}
// Host tick 0048B6B0: the ring advances only after C5 and once every racer
// still in the race has sent at least one input record (cc1 & ~gone == racers);
// in positional mode the clients never send records, so no frames flow at all.
// Each tick completes the current frame (gone racers: 08 00 1F), copies it
// into the next one (own byte0 &= 7) and sends the completed frames. The
// client queues two inputs per frame in a 128-entry ring (00415E80) and
// aborts the race (B7 -2) when it overflows, so frames must not run ahead.
function startFramesIfReady(g) {
  if (!g.racing || g.frameTimer || !g.startTick) return;
  if (!g.members.every(m => m.raceGone || m.gotInput)) return;
  g.frameTimer = setInterval(() => {
    const done = g.frames[g.frames.length - 1];
    for (const m of g.members) if (m.raceGone) { done[m.gameSlot*3] = 8; done[m.gameSlot*3+1] = 0; done[m.gameSlot*3+2] = 0x1f; }
    const next = Buffer.from(done);
    for (const m of g.members) next[m.gameSlot * 3] &= 7;
    g.frames.push(next);
    if (g.frames.length > 128) g.frames.shift();
    for (const m of g.members) if (!m.raceGone) sendReliable(m, done, true);
  }, 1000 / 16);
  log(`  game #${g.id}: input records from all racers, frame exchange started`);
}
function fmtTime(t) {                // 1/64 s -> m:ss.hh (as 004929B0 prints it)
  if (t < 0) return String(t);
  const s = t >> 6;
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}.${String(Math.floor(((t & 63) * 100) / 64)).padStart(2, '0')}`;
}
function stopRace(g) {
  // B7 slot,-2 for every bot as well: without it the clients keep showing them
  // as racing (the -2 clears the slot's participant bit, 0048C220).
  for (const b of g.members.filter(isBot)) {
    b.raceGone = true;
    for (const m of g.members) sendReliable(m, lobbyMsg(0xb7, b.gameSlot, -2));
  }
  if (g.clock) clearInterval(g.clock);
  if (g.frameTimer) clearInterval(g.frameTimer);
  g.clock = g.frameTimer = null; g.racing = false; g.loadMax = 0;
  // Back in the setup screen the bots have to be ready again, or the host
  // cannot start the next race.
  // The frontend clears a slot's car when the multiplayer screen comes back, so
  // every bot announces itself again: car, name, colour and ready.
  for (const b of g.members.filter(isBot)) {
    b.raceGone = false;
    setBotCar(g, b, b.carId);
    setupRecord(g, SETUP_COLOUR, b.gameSlot, BOT_COLOURS[b.gameSlot % BOT_COLOURS.length]);
    setupRecord(g, SETUP_READY, b.gameSlot, 1);
  }
}
function onInputRecord(c, data) {          // raw 3-byte input for the current frame
  const g = c.game;
  if (!g || !g.racing || !g.frames.length) return;
  const f = g.frames[g.frames.length - 1];
  data.copy(f, c.gameSlot * 3, 0, Math.min(3, data.length));
  if (!c.gotInput) { c.gotInput = true; startFramesIfReady(g); }
}
function u24(v) { const x = Buffer.alloc(3); x.writeUIntBE(v & 0xffffff, 0, 3); return x; }
let nextGameId = 100;   // ids 1..99 are the server-list entries
const games = new Map();
// B8 game entry (0048C360): a = game id, b = flags<<16 | players, text = name (<= 63)
function gameEntry(g) {             // 0048BAA0: [B4][u24 id][0x4C record], players (LE) at record +0x48
  const rec = Buffer.from(g.record);
  // 004B9E36 refuses to join a game whose +48 is >= 8 ("Sorry, that game is
  // full"), and a joining player takes a bot's place, so only humans count.
  rec.writeUInt32LE(humans(g).length, 0x48);
  // +0x35 (the name field's +21, Comms.Positional at creation) is the MODE column's row: the race mode as it is now
  // (a stored old "positional" value was out of the 3-row table: "THIS IS A BUG")
  rec[0x35] = g.raceMode >= 0 && g.raceMode <= 2 ? g.raceMode : 0;
  return Buffer.concat([Buffer.from([0xb4]), u24(g.id), rec]);
}

// ---- ON2: career tournaments. MODE "career" picks the event (the game's name may carry "#<id>", else the first
// online event), its opponents drive the bots, the creator's runtime gets each race's track and laps over the control
// line and reports the finishing order; points, money and the table come from tournament.mjs, the table goes to chat.
function gameChat(g, text) {
  const from = (g.members.find(isBot) || g.host || g.members[0]);
  if (!from) return;
  for (const m of g.members) sendReliable(m, lobbyMsg(0xbb, from.gameSlot, -1, text));
}
function careerMode(g) {
  if (g.raceMode !== RACE_MODE_CAREER) { if (g.tournament && !g.tournament.race) g.tournament = null; return; }
  if (g.tournament || !EVENTS.length) return;
  const want = /#(\d+)/.exec(g.name || '');
  const def = (want && EVENTS.find(t => t.id === +want[1])) || EVENTS[0];
  g.tournament = createTournament(def, log);
  log(`  game #${g.id}: tournament ${def.title}`);
  gameChat(g, `Tournament ${def.groupName} #${def.id}: ${def.races.length} races`);
  careerNext(g);
}
function careerNext(g) {
  const T = g.tournament;
  if (!T || !g.host) return;
  const line = T.directive();
  ctlSend(g.host.rinfo, line);
  if (!T.done) gameChat(g, `Next: ${line.replace(/ laps=.*track=/, ': ')}, ${T.current().laps} lap(s)`);
}
function control(c, text) {
  log(`  control <- ${c.account}: ${text}`);
  const g = c.game;
  if (!g || g.host !== c) return;
  if (text === 'hello' && g.tournament) { careerNext(g); return; }
  const T = g.tournament;
  if (!text.startsWith('result ') || !T || T.done) return;
  const places = new Map(), times = new Map();
  for (const f of text.slice(7).trim().split(/\s+/)) {
    const [slot, place, time] = f.split(':').map(Number);
    if (place > 0 && place <= 8) places.set(slot, place);
    times.set(slot, time || 0);
  }
  const row = T.finish(places, times);
  if (!row) return;
  const lines = [...row.entries()].sort((a, b) => (a[1].place || 99) - (b[1].place || 99))
    .map(([slot, r]) => `${r.place || '-'}. ${T.entrants.get(slot).name} +${r.points}`);
  gameChat(g, `Race ${T.race}/${T.def.races.length}: ${lines.join(', ')}`);
  gameChat(g, `${T.done ? 'Final' : 'Standings'}: ${T.table().join(', ')}`);
  log(`  game #${g.id}: ${T.done ? 'final' : 'standings'} ${T.table().join(' | ')}`);
  careerNext(g);
}
function broadcastGame(g) { for (const m of lobbyMembers()) sendReliable(m, gameEntry(g)); }
// Host side of a player entering a game (0048BB50): B2 slot + game info to
// the newcomer, B0 of every present member (C5 = that member is racing) to the newcomer,
// B0 of the newcomer to everyone in the game, then the updated B8 to the lobby.
function joinGame(c, g) {
  if (c.game) leaveGame(c, false);
  else if (c.inLobby) lobbyDepart(c);
  while (g.members.length >= MAX_SLOTS && g.members.some(isBot))
    removeBot(g, g.members.filter(isBot).sort((a, b) => a.gameSlot - b.gameSlot)[0]);
  const used = new Set(g.members.map(m => m.gameSlot));
  let slot = 0; while (used.has(slot)) slot++;
  c.game = g; c.gameSlot = slot;
  sendReliable(c, lobbyMsg(0xb2, slot, MSG_NONE, g.info));
  for (const m of g.members) {
    sendReliable(c, lobbyMsg(0xb0, m.gameSlot, MSG_NONE, m.persona));
    // 0048BC68: C5 slot only for members whose "gone" bit (+CC2) is clear,
    // i.e. the ones actually in a race; the newcomer shows them as racing.
    if (g.racing && !m.raceGone) sendReliable(c, lobbyMsg(0xc5, m.gameSlot));
    sendReliable(m, lobbyMsg(0xb0, slot, MSG_NONE, c.persona));
  }
  g.members.push(c);
  sendReliable(c, lobbyMsg(0xb0, slot, MSG_NONE, c.persona));
  if (g.host !== c) {                   // current state of the game for the newcomer
    sendReliable(c, Buffer.concat([Buffer.from([0xb4]), u24(g.id), g.record]));
    for (const r of g.setup.values()) sendReliable(c, Buffer.concat([Buffer.from([0xbc, 0, 0, 0]), r.bytes]));
  }
  log(`  game #${g.id}: ${c.account} joined as slot ${slot} (${humans(g).length} players)`);
  if (g.host) ctlSend(g.host.rinfo, `humans ${humans(g).length}`);
  broadcastGame(g);
  rebuildBots(g);
}
// Host side of leaving (0048BDA0): B1 slot to everyone else, B2 0xFFFFFE "" to the leaver.
function leaveGame(c, toLobby = true) {
  const g = c.game;
  if (!g) return;
  c.game = null;
  g.members = g.members.filter(m => m !== c);
  for (const [k, r] of g.setup) if (r.slot === c.gameSlot) g.setup.delete(k);
  for (const m of g.members) sendReliable(m, lobbyMsg(0xb1, c.gameSlot));
  sendReliable(c, lobbyMsg(0xb1, c.gameSlot));          // 0048BDA0 broadcasts B1 to the leaver too
  for (const m of g.members) sendReliable(c, lobbyMsg(0xb1, m.gameSlot));   // ...and B1 of every remaining member
  sendReliable(c, lobbyMsg(0xb2, 0xfffffe, MSG_NONE, ''));
  log(`  game #${g.id}: ${c.account} left (${g.members.length} players)`);
  if (g.host === c || g.members.length === 0) removeGame(g);
  else { if (g.racing && humans(g).every(m => m.raceGone)) stopRace(g); broadcastGame(g); syncBots(g); }
  if (toLobby && c.inLobby) lobbyArrive(c);
}
function removeGame(g) {
  stopRace(g);
  games.delete(g.id);
  const kicked = g.members;
  g.members = [];
  for (const m of kicked) {             // as 0048BDA0 for each: empty their game player list, then B2 "left"
    m.game = null;
    for (const o of kicked) sendReliable(m, lobbyMsg(0xb1, o.gameSlot));
    if (g.host && g.host !== m && !kicked.includes(g.host)) sendReliable(m, lobbyMsg(0xb1, g.host.gameSlot));
    sendReliable(m, lobbyMsg(0xb2, 0xfffffe, MSG_NONE, ''));
  }
  log(`  game #${g.id} "${g.name}" removed`);
  for (const m of lobbyMembers()) if (!kicked.includes(m)) sendReliable(m, lobbyMsg(0xb5, g.id));   // 0048BB30
  for (const m of kicked) if (m.inLobby) lobbyArrive(m);
  broadcastServerInfo();
}
// Setup records of BC (sizes from 004B4CD0; all little-endian u32):
//   id >= 0x21E            [id][index][value]              per player (index = slot)
//   0x14E..0x21D, < 0x10E  [id][value]
//   0x142..0x14D           [id]([index] if >= 0x147)[count][count x u32]
//   0x10E..0x141           [id]([index] if >= 0x13A)[string, padded to 4]
function parseSetup(buf, off) {
  const out = [];
  while (off + 8 <= buf.length) {
    const id = buf.readUInt32LE(off);
    let len = 8, idx = null;
    if (id >= 0x21e) { len = 12; idx = buf.readUInt32LE(off + 4); }
    else if (id >= 0x14e) len = 8;
    else if (id >= 0x142) {
      const base = id >= 0x147 ? 4 : 0;
      if (base) idx = buf.readUInt32LE(off + 4);
      if (off + base + 8 > buf.length) break;
      len = base + 8 + buf.readUInt32LE(off + base + 4) * 4;
    } else if (id >= 0x10e) {
      const base = id >= 0x13a ? 4 : 0;
      if (base) idx = buf.readUInt32LE(off + 4);
      const z = buf.indexOf(0, off + base + 4);
      if (z < 0) break;
      len = base + 4 + Math.ceil((z - (off + base + 4) + 1) / 4) * 4;
    }
    if (len <= 0 || off + len > buf.length) break;
    out.push({ key: idx === null ? `${id}` : `${id}:${idx}`, slot: idx, bytes: Buffer.from(buf.subarray(off, off + len)) });
    off += len;
  }
  return out;
}
function onJoin(msg, rinfo, key) {
  const p = msg.subarray(1);
  if (p.length < 0x88) { log(`  join: short payload ${p.length}`); return; }
  const login = cstr(p, 0x3c, 0x34);
  const info = cstr(p, 0x70, 0x18);
  const [account, password = ''] = login.replace(/^c/, '').split('~');
  const [persona, clientVer, serverVer] = info.split('~');
  let c = clients.get(key);
  let result = 1;
  if (!c) {
    const slot = freeSlot();
    if (slot < 0) result = -2;
    else {
      c = { slot, account, persona, rinfo, lastSeen: Date.now(),
            entryAddr: `${p[8]}.${p[9]}.${p[10]}.${p[11]}`, entryPort: p.readUInt16BE(6) };
      clients.set(key, c);
    }
  }
  if (c) c.lastSeen = Date.now();
  log(`  join: account="${account}" password=${password ? `"${password}"` : '(none)'} persona="${persona}" ` +
      `client=${clientVer} server=${serverVer} kind='${String.fromCharCode(p[0x3c])}' -> result ${result}` +
      (c ? ` slot ${c.slot}` : ''));
  const a = Buffer.alloc(0x20);
  p.copy(a, 0x00, 0x00, 0x04);          // request id
  p.copy(a, 0x04, 0x04, 0x18);          // address the client sent to
  a.writeInt32BE(result, 0x18);
  a.writeInt32BE(c ? c.slot : 0, 0x1c);
  send(rinfo, 0x61, a);
}

function datagram(key, rinfo, msg) {
  if (!msg.length) return;
  const type = msg[0];
  const known = clients.get(key);
  if (known) { known.lastSeen = Date.now(); known.rinfo = rinfo; }
  const quiet = type === 0x2a || type === 0x71 ||
    (type === 0x3d && msg.length === 7 && msg.readUInt16BE(3) === 0xfffd);   // keepalives
  if (!quiet && verbose) log(`UDP <- ${key} ${typeName(type)} ${msg.length} bytes${known ? ` (slot ${known.slot} ${known.account})` : ''}\n${hexdump(msg)}`);
  switch (type) {
    case 0x6a: onJoin(msg, rinfo, key); break;            // 'j'
    case 0x2a: break;                                     // '*' keepalive answer
    case 0x71: break;                                     // 'q': a known peer gets no answer ('!' = unknown)
    case 0x3d: if (known) onLink(known, msg); else if (verbose) log('  link frame from an unknown peer'); break;
    case 0x62: case 0x4b:                                 // 'b' / 'K' leave
      if (known) { log(`client ${key} (${known.account}) left`); leaveLobby(known); clients.delete(key); }
      break;
    default: if (verbose) log(`  (${typeName(type)} not handled yet)`);
  }
}
// Host keepalive: the client answers '*' and drops a silent host.
function tick() {
  const now = Date.now();
  for (const [key, c] of clients) {
    if (now - c.lastSeen > 30000) {
      log(`client ${key} (${c.account}) timed out`);
      leaveLobby(c);
      clients.delete(key);
      continue;
    }
    transmit(c.rinfo, Buffer.from([0x2a]));
    if (c.link) {
      sendAck(c);
      for (const u of c.link.unacked) {             // resend unacknowledged data
        if (now - u.sentAt > 1000) { u.sentAt = now; sendLinkRaw(c, frame(c.link, u.seq, u.data), true); }
      }
    }
  }
}
// A client socket went away for good (its WebSocket closed): as a leave.
function drop(key) {
  const c = clients.get(key);
  if (!c) return;
  log(`client ${key} (${c.account}) disconnected`);
  leaveLobby(c); clients.delete(key);
}
return { datagram, tick, drop, clients, games, control, events: EVENTS };
}
