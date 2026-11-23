// CLOUD1: accounts (login + password) and cloud saves for the browser game, served by web/server.mjs under /cloud/.
// docs/tasks/CLOUD1-cloud-saves.md has the design and the security review; the rules that matter are noted where they are kept.
//
// Storage (dir = --cloud, the service's state folder; files 0600, folders 0700, every write a temporary file + rename):
//   accounts/<key>.json   { v, name, created, pw, sessions: [{ h, created, used, dev }], save, backups }
//   saves/<key>/cur.zip   the current save (as uploaded, after zipcheck.mjs); b-<id>.zip the backups
// key = the first 40 hex digits of SHA-256(lower-case name): file names never come from what a client typed.
//
// Passwords: scrypt (N 2^15, r 8, p 1: 32 MB, ~0.1 s), a random 16-byte salt each, at most SCRYPT_PARALLEL at once and
// SCRYPT_QUEUE waiting (then 503): a flood of logins cannot take the memory or the CPU. Sessions: a random 32-byte token
// in an HttpOnly, SameSite=Strict cookie (__Host- prefixed and Secure on the public site); the account keeps only its
// SHA-256. CSRF: every request that changes something needs the X-NFS5 header (a cross-site page cannot send it without
// a CORS preflight, which this server never answers) and, when the browser sends an Origin, the site's own.
// Limits per client address (X-Real-IP from nginx on the loopback), per account and for the whole server are below.
import crypto from 'node:crypto';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { checkZip, contentHash, profileCount, ZipCheckError, MAX_ZIP } from './zipcheck.mjs';

const MIN = 60e3, HOUR = 60 * MIN, DAY = 24 * HOUR;
export const NAME_RE = /^[A-Za-z0-9][A-Za-z0-9_-]{2,14}$/;        // also the online name: <= 15 (netgather.mjs persona)
const RESERVED = new Set(['admin', 'administrator', 'root', 'system', 'server', 'host', 'guest', 'moderator', 'mod',
  'support', 'staff', 'nfs5', 'nfs5web', 'reforged', 'ea', 'bot', 'null', 'undefined', 'anonymous', 'player']);
const PW_MIN = 8, PW_MAX = 128;
// a few of the most used passwords (a full list would be a dependency; these are the ones bots try first)
const COMMON = new Set(['12345678', '123456789', '1234567890', 'password', 'password1', 'qwertyui', 'qwerty123', '11111111',
  '00000000', 'iloveyou', '1q2w3e4r', '1q2w3e4r5t', 'qwertyuiop', 'abc12345', 'abcd1234', 'passw0rd', 'football',
  'baseball', 'princess', 'sunshine', 'superman', 'trustno1', 'welcome1', 'zaq12wsx', 'asdfghjk', 'asdfasdf', '12341234',
  '87654321', '123123123', 'q1w2e3r4', 'porsche911', 'porsche1', 'needforspeed', 'nfs12345']);
const SCRYPT = { N: 1 << 15, r: 8, p: 1, len: 32, maxmem: 96 << 20 };
const SCRYPT_PARALLEL = 2, SCRYPT_QUEUE = 32;
const MAX_SESSIONS = 20, SESSION_IDLE = 365 * DAY, SESSION_TOUCH = HOUR;
const MAX_BACKUPS = 5, BACKUP_EVERY = 20 * HOUR;
const EMPTY_ACCOUNT_DAYS = 30;
const JSON_MAX = 4096;

export class HttpError extends Error {
  constructor(status, code, message, extra = {}) { super(message || code); this.status = status; this.code = code; this.extra = extra; }
}

// ------------------------------------------------------------------------------------------------ limits ----
/** A fixed-window counter per key; prune() drops the old windows (run every minute), and a map past `cap` keys is
 *  cleared (fail open rather than grow without end under a flood of addresses). */
class Limit {
  constructor(max, windowMs, cap = 50000) { this.max = max; this.win = windowMs; this.cap = cap; this.m = new Map(); }
  entry(key, now) {
    let e = this.m.get(key);
    if (!e || now - e.t >= this.win) { if (this.m.size >= this.cap) this.m.clear(); e = { t: now, n: 0 }; this.m.set(key, e); }
    return e;
  }
  /** counts one; false when the key is over its limit (then the call does not count) */
  take(key, now = Date.now()) { const e = this.entry(key, now); if (e.n >= this.max) return false; e.n++; return true; }
  over(key, now = Date.now()) { const e = this.m.get(key); return !!e && now - e.t < this.win && e.n >= this.max; }
  add(key, now = Date.now()) { this.entry(key, now).n++; }
  retryAfter(key, now = Date.now()) { const e = this.m.get(key); return e ? Math.max(1, Math.ceil((e.t + this.win - now) / 1000)) : 1; }
  prune(now = Date.now()) { for (const [k, e] of this.m) if (now - e.t >= this.win) this.m.delete(k); }
}

/** IPv6 address -> its /64 (one home line has a whole /64), IPv4 as it is. */
function ipKey(ip) {
  ip = String(ip || '').trim().replace(/^::ffff:(?=\d+\.)/, '');
  if (!ip.includes(':')) return /^\d{1,3}(\.\d{1,3}){3}$/.test(ip) ? ip : 'unknown';
  const [head, tail = ''] = ip.split('::');
  const a = head ? head.split(':') : [], b = tail ? tail.split(':') : [];
  if (!ip.includes('::') && a.length !== 8) return 'unknown';
  const full = [...a, ...Array(Math.max(0, 8 - a.length - b.length)).fill('0'), ...b];
  return full.slice(0, 4).map(x => (parseInt(x, 16) || 0).toString(16)).join(':') + '::/64';
}
/** The client's address: nginx's X-Real-IP when the request came from the loopback (nginx), else the socket's. */
export function clientIp(req) {
  const a = req.socket?.remoteAddress || '';
  const loop = a === '127.0.0.1' || a === '::1' || a === '::ffff:127.0.0.1';
  const real = req.headers['x-real-ip'];
  return ipKey(loop && typeof real === 'string' && real ? real : a);
}
/** An address for the log: the last IPv4 byte dropped (the site's nginx log does the same). */
const anon = key => key.includes('::/64') ? key : key.replace(/\.\d+$/, '.0');

// ------------------------------------------------------------------------------------------------- files ----
async function writeAtomic(file, data) {
  const tmp = `${file}.${crypto.randomBytes(6).toString('hex')}.tmp`;
  const fh = await fsp.open(tmp, 'w', 0o600);
  try { await fh.writeFile(data); await fh.sync(); } finally { await fh.close(); }
  try { await fsp.rename(tmp, file); } catch (e) { await fsp.rm(tmp, { force: true }); throw e; }
}
const keyOf = name => crypto.createHash('sha256').update(name.toLowerCase(), 'utf8').digest('hex').slice(0, 40);
const sha256 = s => crypto.createHash('sha256').update(s).digest();

// ------------------------------------------------------------------------------------------------ module ----
/**
 * dir: the state folder; secure: the public site (Secure, __Host- cookie); log(line); quotaBytes: what all saves may
 * take; maxAccounts. Returns { handle(req, res, url) -> Promise<boolean>, identify(req), names(), close() }.
 */
export function createCloud({ dir, secure = false, log = () => {}, quotaBytes = 2 << 30, maxAccounts = 100000 }) {
  const ACC = path.join(dir, 'accounts'), SAVES = path.join(dir, 'saves');
  for (const d of [dir, ACC, SAVES]) fs.mkdirSync(d, { recursive: true, mode: 0o700 });
  const COOKIE = secure ? '__Host-nfs5' : 'nfs5';

  // what is on disk: the account count and the bytes of all saves (kept up to date by the writes below)
  let accounts = 0, usedBytes = 0;
  const names = new Map();                              // lower-case name -> name (the online server's guest check)
  for (const f of fs.readdirSync(ACC)) {
    if (!/^[0-9a-f]{40}\.json$/.test(f)) continue;
    try { const a = JSON.parse(fs.readFileSync(path.join(ACC, f), 'utf8')); accounts++; names.set(a.name.toLowerCase(), a.name); } catch { /* skipped */ }
  }
  for (const d of fs.readdirSync(SAVES)) {
    try { for (const f of fs.readdirSync(path.join(SAVES, d))) usedBytes += fs.statSync(path.join(SAVES, d, f)).size; } catch { /* skipped */ }
  }
  log(`[cloud] ${accounts} accounts, saves ${(usedBytes / 1048576).toFixed(1)} MB in ${dir}`);

  // ---- limits (see docs/tasks/CLOUD1-cloud-saves.md "Limits")
  const L = {
    requests: new Limit(600, 10 * MIN),           // any /cloud/ request, per address
    loginIp: new Limit(15, 15 * MIN),             // password checks (logins, password change, delete), per address:
                                                  // counted before the check, so parallel requests cannot pass it
    loginAcct: new Limit(50, HOUR),               // logins per account from all addresses
    registerIpHour: new Limit(5, HOUR),
    registerIpDay: new Limit(20, DAY),
    registerAll: new Limit(500, DAY),             // new accounts per day on the whole server
    put: new Limit(120, HOUR),                    // uploads per account
    get: new Limit(300, HOUR),                    // downloads per account
    logLines: new Limit(20, MIN),                 // log lines a client can cause
  };
  const pruner = setInterval(() => { for (const l of Object.values(L)) l.prune(); }, MIN);
  pruner.unref?.();
  const say = (ip, line) => { if (L.logLines.take(ip)) log(`[cloud] ${line} (${anon(ip)})`); };

  // ---- scrypt, at most SCRYPT_PARALLEL at once
  let active = 0;
  const waiting = [];
  async function scrypt(pw, salt) {
    if (active >= SCRYPT_PARALLEL) {
      if (waiting.length >= SCRYPT_QUEUE) throw new HttpError(503, 'busy', 'The server is busy, try again in a minute');
      await new Promise(r => waiting.push(r));
    } else active++;
    try {
      return await new Promise((res, rej) => crypto.scrypt(pw.normalize('NFKC'), salt, SCRYPT.len,
        { N: SCRYPT.N, r: SCRYPT.r, p: SCRYPT.p, maxmem: SCRYPT.maxmem }, (e, k) => e ? rej(e) : res(k)));
    } finally {
      const next = waiting.shift();
      if (next) next(); else active--;
    }
  }
  async function hashPassword(pw) {
    const salt = crypto.randomBytes(16), k = await scrypt(pw, salt);
    return `s1$${Math.log2(SCRYPT.N)}$${SCRYPT.r}$${SCRYPT.p}$${salt.toString('base64url')}$${k.toString('base64url')}`;
  }
  async function checkPassword(pw, stored) {
    const m = /^s1\$15\$8\$1\$([A-Za-z0-9_-]+)\$([A-Za-z0-9_-]+)$/.exec(stored || '');
    if (!m) return false;
    const k = await scrypt(pw, Buffer.from(m[1], 'base64url')), want = Buffer.from(m[2], 'base64url');
    return k.length === want.length && crypto.timingSafeEqual(k, want);
  }
  // a login for a name nobody has still costs one scrypt (the answer's time does not tell names apart)
  const dummyHash = hashPassword(crypto.randomBytes(12).toString('hex'));

  // ---- accounts
  const accFile = key => path.join(ACC, key + '.json');
  const saveDir = key => path.join(SAVES, key);
  async function loadAccount(key) {
    let text;
    try { text = await fsp.readFile(accFile(key), 'utf8'); } catch (e) { if (e.code === 'ENOENT') return null; throw e; }
    const a = JSON.parse(text);
    if (!a || a.v !== 1 || typeof a.name !== 'string' || !Array.isArray(a.sessions) || !Array.isArray(a.backups)) throw new Error(`account ${key} is damaged`);
    return a;
  }
  const storeAccount = (key, a) => writeAtomic(accFile(key), JSON.stringify(a));

  // one operation at a time per account (two devices saving at once, a login during an upload)
  const locks = new Map();
  function locked(key, fn) {
    const prev = locks.get(key) || Promise.resolve();
    const run = prev.then(fn, fn);
    const tail = run.then(() => {}, () => {});
    locks.set(key, tail);
    void tail.then(() => { if (locks.get(key) === tail) locks.delete(key); });
    return run;
  }

  // ---- requests
  function parseCookie(h) {
    for (const part of String(h || '').split(';')) {
      const i = part.indexOf('=');
      if (i > 0 && part.slice(0, i).trim() === COOKIE) return part.slice(i + 1).trim();
    }
    return null;
  }
  function setCookie(res, value, maxAgeS) {
    res.setHeader('Set-Cookie', `${COOKIE}=${value}; Path=/; HttpOnly; SameSite=Strict; Max-Age=${maxAgeS}${secure ? '; Secure' : ''}`);
  }
  /** the account and session of a request's cookie, or null */
  async function sessionOf(req) {
    const c = parseCookie(req.headers.cookie);
    const m = c && /^([0-9a-f]{40})\.([A-Za-z0-9_-]{43})$/.exec(c);
    if (!m) return null;
    const a = await loadAccount(m[1]).catch(() => null);
    if (!a) return null;
    const h = sha256(m[2]), now = Date.now();
    const s = a.sessions.find(x => typeof x.h === 'string' && x.h.length === 64 && crypto.timingSafeEqual(Buffer.from(x.h, 'hex'), h));
    if (!s || now - s.used > SESSION_IDLE) return null;
    return { key: m[1], acct: a, sess: s };
  }
  async function newSession(res, key, a, dev) {
    const token = crypto.randomBytes(32).toString('base64url'), now = Date.now();
    a.sessions = a.sessions.filter(s => now - s.used <= SESSION_IDLE);
    a.sessions.push({ h: sha256(token).toString('hex'), created: now, used: now, dev });
    a.sessions.sort((x, y) => y.used - x.used);
    a.sessions.length = Math.min(a.sessions.length, MAX_SESSIONS);
    setCookie(res, `${key}.${token}`, Math.floor(SESSION_IDLE / 1000));
  }

  function readBody(req, max) {
    return new Promise((resolve, reject) => {
      const declared = Number(req.headers['content-length']);
      if (declared > max) { reject(new HttpError(413, 'too-big', 'Too big')); return; }
      const parts = [];
      let n = 0, done = false;
      req.on('data', d => {
        if (done) return;
        n += d.length;
        if (n > max) { done = true; reject(new HttpError(413, 'too-big', 'Too big')); return; }
        parts.push(d);
      });
      req.on('end', () => { if (!done) { done = true; resolve(Buffer.concat(parts)); } });
      req.on('error', e => { if (!done) { done = true; reject(e); } });
      req.on('close', () => { if (!done) { done = true; reject(new HttpError(400, 'aborted', 'Aborted')); } });
    });
  }
  async function readJson(req) {
    if (!/^application\/json\b/i.test(req.headers['content-type'] || '')) throw new HttpError(415, 'type', 'JSON expected');
    let v;
    try { v = JSON.parse((await readBody(req, JSON_MAX)).toString('utf8')); } catch (e) { if (e instanceof HttpError) throw e; throw new HttpError(400, 'json', 'Bad request'); }
    if (!v || typeof v !== 'object' || Array.isArray(v)) throw new HttpError(400, 'json', 'Bad request');
    return v;
  }
  const str = (v, max) => typeof v === 'string' && v.length <= max ? v : null;
  /** a device's label from the page ("Chrome on Windows"): letters, digits, spaces and . ( ) - only, 40 at most */
  const devLabel = v => String(v || '').replace(/[^A-Za-z0-9 .()-]/g, '').slice(0, 40).trim() || 'browser';

  /** the site's own origin, or a page of another site (then: refused) */
  function sameOrigin(req) {
    const o = req.headers.origin;
    if (o === undefined) return true;                   // not a browser's cross-site request (curl, the online server)
    const host = req.headers.host || '';           // nginx passes the site's (proxy_set_header Host $host)
    try { return new URL(o).host === host; } catch { return false; }
  }

  function send(res, status, body, headers = {}) {
    const b = Buffer.from(JSON.stringify(body));
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': b.length, ...headers });
    res.end(b);
  }
  const saveInfo = s => s && { rev: s.rev, at: s.at, made: s.made ?? s.at, size: s.size, hash: s.hash, dev: s.dev, profiles: s.profiles };
  const meOf = a => ({ user: a.name, save: saveInfo(a.save), backups: a.backups.map(b => ({ id: b.id, at: b.at, made: b.made ?? b.at, size: b.size, dev: b.dev, profiles: b.profiles })).reverse(), now: Date.now() });

  function checkName(name) {
    if (!name || !NAME_RE.test(name)) throw new HttpError(400, 'bad-name', 'The login is 3 to 15 letters, digits, _ or -, starting with a letter or digit');
    const l = name.toLowerCase();
    if (RESERVED.has(l) || /-guest$/.test(l)) throw new HttpError(400, 'reserved', 'This login is reserved, choose another');
  }
  function checkNewPassword(pw, name) {
    if (pw === null || pw.length < PW_MIN) throw new HttpError(400, 'weak-password', `The password needs at least ${PW_MIN} characters`);
    if (pw.length > PW_MAX) throw new HttpError(400, 'bad-password', `The password has at most ${PW_MAX} characters`);
    const l = pw.toLowerCase();
    if (COMMON.has(l) || l === name.toLowerCase() || /^(.)\1+$/.test(pw)) throw new HttpError(400, 'weak-password', 'This password is too easy to guess, choose another');
  }

  // ---- endpoints
  async function register(req, res, ip) {
    const b = await readJson(req);
    const name = str(b.user, 32), pw = str(b.password, 1024);
    checkName(name);
    checkNewPassword(pw, name);
    if (names.has(name.toLowerCase())) throw new HttpError(409, 'taken', 'This login is taken');
    if (L.registerIpHour.over(ip) || L.registerIpDay.over(ip)) throw new HttpError(429, 'rate', 'Too many new accounts from your address, try again later', { retry: L.registerIpHour.retryAfter(ip) });
    if (L.registerAll.over('all') || accounts >= maxAccounts) throw new HttpError(503, 'full', 'No new accounts can be made today, try again tomorrow');
    const key = keyOf(name), hash = await hashPassword(pw);
    return locked(key, async () => {
      if (await loadAccount(key)) throw new HttpError(409, 'taken', 'This login is taken');
      L.registerIpHour.add(ip); L.registerIpDay.add(ip); L.registerAll.add('all');
      const a = { v: 1, name, created: Date.now(), pw: hash, sessions: [], save: null, backups: [] };
      await newSession(res, key, a, devLabel(b.device));
      await storeAccount(key, a);
      accounts++; names.set(name.toLowerCase(), name);
      say(ip, `new account ${name}`);
      send(res, 200, meOf(a));
    });
  }
  async function login(req, res, ip) {
    const b = await readJson(req);
    const name = str(b.user, 32), pw = str(b.password, 1024);
    if (!name || !pw) throw new HttpError(400, 'json', 'Login and password needed');
    passwordTry(ip);
    const key = NAME_RE.test(name) ? keyOf(name) : null;
    if (key && !L.loginAcct.take(key)) throw new HttpError(429, 'rate', 'Too many logins for this login name, try again later', { retry: L.loginAcct.retryAfter(key) });
    const a = key ? await loadAccount(key) : null;
    const ok = await checkPassword(pw, a ? a.pw : await dummyHash);
    if (!a || !ok) {
      say(ip, `wrong login or password${a ? ` for ${a.name}` : ''}`);
      throw new HttpError(401, 'wrong', 'Wrong login or password');
    }
    return locked(key, async () => {
      const cur = await loadAccount(key);
      if (!cur) throw new HttpError(401, 'wrong', 'Wrong login or password');
      await newSession(res, key, cur, devLabel(b.device));
      await storeAccount(key, cur);
      send(res, 200, meOf(cur));
    });
  }
  function passwordTry(ip) {
    if (!L.loginIp.take(ip)) throw new HttpError(429, 'rate', 'Too many password tries, try again in a few minutes', { retry: L.loginIp.retryAfter(ip) });
  }
  async function needSession(req) {
    const s = await sessionOf(req);
    if (!s) throw new HttpError(401, 'auth', 'Not signed in');
    return s;
  }
  async function me(req, res) {
    const s = await sessionOf(req);
    if (!s) { send(res, 200, { user: null, now: Date.now() }); return; }
    if (Date.now() - s.sess.used > SESSION_TOUCH) {
      await locked(s.key, async () => {
        const a = await loadAccount(s.key);
        const x = a && a.sessions.find(y => y.h === s.sess.h);
        if (x) { x.used = Date.now(); await storeAccount(s.key, a); }
      });
    }
    send(res, 200, meOf(s.acct));
  }
  async function logout(req, res) {
    const s = await sessionOf(req);
    if (s) await locked(s.key, async () => {
      const a = await loadAccount(s.key);
      if (a) { a.sessions = a.sessions.filter(x => x.h !== s.sess.h); await storeAccount(s.key, a); }
    });
    setCookie(res, '', 0);
    send(res, 200, { user: null });
  }
  async function changePassword(req, res, ip) {
    const s = await needSession(req);
    const b = await readJson(req);
    const old = str(b.old, 1024), pw = str(b.password, 1024);
    checkNewPassword(pw, s.acct.name);
    passwordTry(ip);
    if (!old || !await checkPassword(old, s.acct.pw)) { throw new HttpError(401, 'wrong', 'The current password is wrong'); }
    const hash = await hashPassword(pw);
    await locked(s.key, async () => {
      const a = await loadAccount(s.key);
      if (!a) throw new HttpError(401, 'auth', 'Not signed in');
      a.pw = hash;
      a.sessions = a.sessions.filter(x => x.h === s.sess.h);      // every other device signs in again
      await storeAccount(s.key, a);
    });
    say(ip, `password changed for ${s.acct.name}`);
    send(res, 200, { ok: true });
  }
  async function removeAccount(req, res, ip) {
    const s = await needSession(req);
    const b = await readJson(req);
    const pw = str(b.password, 1024);
    passwordTry(ip);
    if (!pw || !await checkPassword(pw, s.acct.pw)) { throw new HttpError(401, 'wrong', 'The password is wrong'); }
    await locked(s.key, () => dropAccount(s.key));
    setCookie(res, '', 0);
    say(ip, `account ${s.acct.name} deleted`);
    send(res, 200, { user: null });
  }
  async function dropAccount(key) {
    const a = await loadAccount(key).catch(() => null);
    const d = saveDir(key);
    try { for (const f of await fsp.readdir(d)) { usedBytes -= (await fsp.stat(path.join(d, f))).size; } } catch { /* none */ }
    await fsp.rm(d, { recursive: true, force: true });
    await fsp.rm(accFile(key), { force: true });
    if (a) { accounts--; names.delete(a.name.toLowerCase()); }
  }

  async function getSave(req, res, url) {
    const s = await needSession(req);
    if (!L.get.take(s.key)) throw new HttpError(429, 'rate', 'Too many downloads, try again later', { retry: L.get.retryAfter(s.key) });
    const id = url.searchParams.get('b');
    let file, info;
    if (id === null) { info = s.acct.save; file = info && path.join(saveDir(s.key), 'cur.zip'); }
    else {
      if (!/^[a-z0-9]{1,16}$/.test(id)) throw new HttpError(400, 'json', 'Bad request');
      info = s.acct.backups.find(b => b.id === id);
      file = info && path.join(saveDir(s.key), `b-${id}.zip`);
    }
    if (!info) throw new HttpError(404, 'none', 'No such save');
    let data;
    try { data = await fsp.readFile(file); } catch { throw new HttpError(404, 'none', 'No such save'); }
    const day = new Date(info.at).toISOString().slice(0, 10);
    res.writeHead(200, {
      'Content-Type': 'application/zip', 'Content-Length': data.length, ETag: `"${info.rev}"`,
      'Content-Disposition': `attachment; filename="nfs5-saves-${s.acct.name}-${day}.zip"`,
      'X-Save': JSON.stringify(saveInfo(info)),
    });
    res.end(data);
  }

  async function putSave(req, res, ip) {
    const s = await needSession(req);
    if (!L.put.take(s.key)) throw new HttpError(429, 'rate', 'Too many uploads, try again later', { retry: L.put.retryAfter(s.key) });
    const body = await readBody(req, MAX_ZIP);
    let entries;
    try { entries = checkZip(body); } catch (e) { if (e instanceof ZipCheckError) throw new HttpError(400, 'bad-zip', `The save is not accepted: ${e.message}`); throw e; }
    if (!entries.length) throw new HttpError(400, 'empty', 'An empty save is never stored');
    const hash = contentHash(entries), profiles = profileCount(entries), dev = devLabel(req.headers['x-device']);
    const ifMatch = req.headers['if-match'], force = req.headers['x-overwrite'] === '1';
    // when the saves were last changed (the page's newest file, on this server's clock): what "the newer one wins"
    // compares; one in the future or from before 2000 is taken as now
    const madeHdr = Number(req.headers['x-made-at']), now = Date.now();
    const made = Number.isFinite(madeHdr) && madeHdr > 946684800000 && madeHdr <= now + MIN ? Math.round(madeHdr) : now;
    await locked(s.key, async () => {
      const a = await loadAccount(s.key);
      if (!a) throw new HttpError(401, 'auth', 'Not signed in');
      const cur = a.save;
      if (!force && (cur ? ifMatch !== `"${cur.rev}"` : ifMatch !== undefined && ifMatch !== '*'))
        throw new HttpError(412, 'conflict', 'The cloud has a newer save', { save: saveInfo(cur) });
      if (cur && cur.hash === hash) { send(res, 200, meOf(a)); return; }
      if (usedBytes + body.length > quotaBytes) { say(ip, 'save refused: the server is full'); throw new HttpError(507, 'full', 'The server has no room for saves now'); }
      const d = saveDir(s.key);
      await fsp.mkdir(d, { recursive: true, mode: 0o700 });
      const curFile = path.join(d, 'cur.zip');
      // the version this one replaces becomes a backup when it came from another device, had more profiles, or the
      // last backup is old (one a day while playing); at most MAX_BACKUPS, the oldest goes
      const last = a.backups[a.backups.length - 1];
      if (cur && (force || cur.dev !== dev || profiles < (cur.profiles ?? 0) || !last || Date.now() - last.at > BACKUP_EVERY)) {
        let id = Date.now().toString(36);
        while (a.backups.some(b => b.id === id)) id += 'x';
        try {
          await fsp.rename(curFile, path.join(d, `b-${id}.zip`));
          a.backups.push({ id, at: cur.at, made: cur.made ?? cur.at, rev: cur.rev, size: cur.size, hash: cur.hash, dev: cur.dev, profiles: cur.profiles });
        } catch (e) { if (e.code !== 'ENOENT') throw e; }
        while (a.backups.length > MAX_BACKUPS) {
          const old = a.backups.shift(), f = path.join(d, `b-${old.id}.zip`);
          try { usedBytes -= (await fsp.stat(f)).size; await fsp.rm(f, { force: true }); } catch { /* gone */ }
        }
      } else if (cur) usedBytes -= cur.size;
      await writeAtomic(curFile, body);
      usedBytes += body.length;
      a.save = { rev: (cur?.rev ?? 0) + 1, at: Date.now(), made, size: body.length, hash, dev, profiles };
      await storeAccount(s.key, a);
      send(res, 200, meOf(a));
    });
  }

  async function handle(req, res, url) {
    const p = url.pathname;
    if (!p.startsWith('/cloud/')) return false;
    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    const ip = clientIp(req);
    try {
      if (!L.requests.take(ip)) throw new HttpError(429, 'rate', 'Too many requests, try again later', { retry: L.requests.retryAfter(ip) });
      const m = req.method;
      if (m !== 'GET' && m !== 'HEAD') {
        if (req.headers['x-nfs5'] !== '1' || !sameOrigin(req)) throw new HttpError(403, 'origin', 'Refused');
      }
      const route = `${m} ${p}`;
      if (route === 'GET /cloud/me') await me(req, res);
      else if (route === 'POST /cloud/register') await register(req, res, ip);
      else if (route === 'POST /cloud/login') await login(req, res, ip);
      else if (route === 'POST /cloud/logout') await logout(req, res);
      else if (route === 'POST /cloud/password') await changePassword(req, res, ip);
      else if (route === 'POST /cloud/delete') await removeAccount(req, res, ip);
      else if (route === 'GET /cloud/save') await getSave(req, res, url);
      else if (route === 'PUT /cloud/save') await putSave(req, res, ip);
      else if (route === 'GET /cloud/taken') {
        const u = url.searchParams.get('u') || '';
        send(res, 200, { taken: names.has(u.toLowerCase()) });
      } else throw new HttpError(404, 'none', 'Not found');
    } catch (e) {
      if (res.headersSent) { res.destroy(); return true; }
      if (e instanceof HttpError) {
        const h = e.extra.retry ? { 'Retry-After': String(e.extra.retry) } : {};
        if (e.status === 413) h.Connection = 'close';
        send(res, e.status, { error: e.code, message: e.message, ...(e.extra.save !== undefined ? { save: e.extra.save } : {}) }, h);
      } else {
        log(`[cloud] error on ${req.method} ${p}: ${e && e.stack || e}`);
        send(res, 500, { error: 'server', message: 'Server error' });
      }
    }
    return true;
  }

  /** the account name of a request's cookie (the online server: the player's online name), or null */
  async function identify(req) { const s = await sessionOf(req).catch(() => null); return s ? s.acct.name : null; }

  // ---- once a day: sessions past their time go, accounts that never stored a save and were not used for
  // EMPTY_ACCOUNT_DAYS go (a name held by nobody playing)
  async function sweep() {
    let dropped = 0;
    for (const f of await fsp.readdir(ACC)) {
      const m = /^([0-9a-f]{40})\.json$/.exec(f);
      if (!m) continue;
      await locked(m[1], async () => {
        const a = await loadAccount(m[1]).catch(() => null);
        if (!a) return;
        const now = Date.now(), used = Math.max(a.created, ...a.sessions.map(s => s.used));
        if (!a.save && !a.backups.length && now - used > EMPTY_ACCOUNT_DAYS * DAY) { await dropAccount(m[1]); dropped++; return; }
        const n = a.sessions.length;
        a.sessions = a.sessions.filter(s => now - s.used <= SESSION_IDLE);
        if (a.sessions.length !== n) await storeAccount(m[1], a);
      });
    }
    if (dropped) log(`[cloud] sweep: ${dropped} unused empty accounts removed`);
  }
  const sweeper = setInterval(() => void sweep().catch(e => log(`[cloud] sweep failed: ${e}`)), DAY);
  sweeper.unref?.();
  setTimeout(() => void sweep().catch(e => log(`[cloud] sweep failed: ${e}`)), 30e3).unref?.();

  return {
    handle, identify, cookieName: COOKIE,
    /** is a name (any letter case) an account's */
    taken: name => names.has(String(name).toLowerCase()),
    close() { clearInterval(pruner); clearInterval(sweeper); },
  };
}
