// CLOUD1 acceptance and attack tests for the accounts and cloud saves (web/cloud), the site server's hardening and the
// online server's names. Starts its own server.mjs on a scratch state folder; needs no browser and no game.
//   node web/tools/cloud_test.mjs [--port 8191]
// Prints one line per check and "N passed, M failed"; exit code 1 on any failure.
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url)), WEB = path.dirname(HERE);
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8191), INTERNAL = PORT + 1, PUB = PORT + 2;
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'nfs5-cloud-test-'));
const wait = ms => new Promise(r => setTimeout(r, ms));

// the page's zip.ts, bundled for Node (it has CompressionStream, crypto.subtle and Blob): the client's hash and zip
const zipJs = path.join(TMP, 'zip.mjs');
await esbuild.build({ entryPoints: [path.join(WEB, 'src/cloud/zip.ts')], outfile: zipJs, bundle: true, format: 'esm', platform: 'neutral', logLevel: 'warning' });
const Z = await import('file://' + zipJs.replace(/\\/g, '/'));

let passed = 0, failed = 0;
function check(name, ok, detail = '') {
  if (ok) passed++; else failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${!ok && detail ? `   (${detail})` : ''}`);
}

function startServer(extra, port, logTo) {
  const child = spawn(process.execPath, [path.join(WEB, 'server.mjs'), '--port', String(port), '--cars', '', '--logs', path.join(TMP, 'logs'), ...extra],
    { cwd: path.dirname(WEB), stdio: ['ignore', 'pipe', 'pipe'] });
  child.out = '';
  child.stdout.on('data', d => { child.out += d; if (logTo) logTo(d); });
  child.stderr.on('data', d => { child.out += d; });
  child.dead = false; child.on('exit', () => { child.dead = true; });
  return child;
}

/** one request: { status, headers, body (Buffer), json } */
function req(method, p, { body, headers = {}, port = PORT } = {}) {
  return new Promise(resolve => {
    const r = http.request({ host: '127.0.0.1', port, path: p, method, headers }, res => {
      const parts = [];
      res.on('data', d => parts.push(d));
      res.on('end', () => {
        const b = Buffer.concat(parts);
        let json = null; try { json = JSON.parse(b.toString()); } catch { /* not json */ }
        resolve({ status: res.statusCode, headers: res.headers, body: b, json });
      });
    });
    r.on('error', e => resolve({ status: 'ERR ' + e.code, headers: {}, body: Buffer.alloc(0), json: null }));
    r.setTimeout(20000, () => { r.destroy(); resolve({ status: 'TIMEOUT', headers: {}, body: Buffer.alloc(0), json: null }); });
    if (body !== undefined) r.write(body);
    r.end();
  });
}
const H = (cookie, more = {}) => ({ 'X-NFS5': '1', ...(cookie ? { Cookie: cookie } : {}), ...more });
const post = (p, obj, cookie, more = {}, port = PORT) => req('POST', p, { body: JSON.stringify(obj), headers: H(cookie, { 'Content-Type': 'application/json', ...more }), port });
const cookieOf = r => (r.headers['set-cookie'] || []).map(c => c.split(';')[0]).find(c => /=./.test(c)) ?? null;

// ---- a hand-made zip (for the broken ones the page's writer never makes)
function rawZip(entries, { flags = 0x800, method = null, badCrc = false, lieSize = null } = {}) {
  const locals = [], centrals = [];
  let off = 0;
  for (const e of entries) {
    const name = Buffer.from(e.name, 'utf8'), data = e.data;
    const m = method ?? (e.deflate ? 8 : 0);
    const body = m === 8 ? zlib.deflateRawSync(data) : data;
    const crc = (zlib.crc32(data) ^ (badCrc ? 1 : 0)) >>> 0, usize = lieSize ?? data.length;
    const lh = Buffer.alloc(30); lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(flags, 6); lh.writeUInt16LE(m, 8);
    lh.writeUInt32LE(crc, 14); lh.writeUInt32LE(body.length, 18); lh.writeUInt32LE(usize, 22); lh.writeUInt16LE(name.length, 26);
    const ch = Buffer.alloc(46); ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(flags, 8);
    ch.writeUInt16LE(m, 10); ch.writeUInt32LE(crc, 16); ch.writeUInt32LE(body.length, 20); ch.writeUInt32LE(usize, 24);
    ch.writeUInt16LE(name.length, 28); ch.writeUInt32LE(off, 42);
    locals.push(lh, name, body); centrals.push(ch, name);
    off += 30 + name.length + body.length;
  }
  const cd = Buffer.concat(centrals), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(off, 16);
  return Buffer.concat([...locals, cd, end]);
}
const put = (zip, cookie, more = {}) => req('PUT', '/cloud/save', { body: zip, headers: H(cookie, { 'Content-Type': 'application/zip', ...more }) });
const sav = (n, seed) => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = (i * 31 + seed) & 0xff; return b; };
const saveFiles = (seed, profiles = 1) => [
  { name: 'XXDefXX.sav', data: sav(4000, seed) }, { name: 'player.lst', data: Buffer.from('5���') },
  ...Array.from({ length: profiles }, (_, i) => ({ name: `5���${i}.sav`, data: sav(150000, seed + i) })),
];

const srvLog = [];
const srv = startServer(['--cloud', path.join(TMP, 'cloud'), '--internal-port', String(INTERNAL)], PORT, d => srvLog.push(String(d)));
const pub = startServer(['--public', '--cloud', path.join(TMP, 'cloud-pub'), '--exe', path.join(WEB, 'server.mjs'), '--data', path.join(TMP, 'nodata')], PUB);
fs.mkdirSync(path.join(TMP, 'nodata'), { recursive: true });
await wait(2500);
try {
  // ------------------------------------------------------------------------------------- accounts
  let r = await req('GET', '/cloud/me');
  check('me without a session: user null', r.status === 200 && r.json?.user === null, JSON.stringify(r.json));
  check('cloud answers carry no-store and nosniff', r.headers['cache-control'] === 'no-store' && r.headers['x-content-type-options'] === 'nosniff');
  for (const [u, pw, code] of [['ab', 'goodpassword1', 'bad-name'], ['a b c', 'goodpassword1', 'bad-name'], ['../../etc', 'goodpassword1', 'bad-name'],
    ['admin', 'goodpassword1', 'reserved'], ['bob-guest', 'goodpassword1', 'reserved'], ['__proto__', 'goodpassword1', 'reserved'], ['Alice', 'short', 'weak-password'],
    ['Alice', 'password1', 'weak-password'], ['Alice', 'alice', 'weak-password'], ['Alice', 'aaaaaaaaaa', 'weak-password'], ['Alice', 'x'.repeat(200), 'bad-password']]) {
    r = await post('/cloud/register', { user: u, password: pw }, null, { 'X-Real-IP': '10.1.0.1' });
    check(`register refused: ${JSON.stringify(u).slice(0, 20)} / ${pw.slice(0, 12)} -> ${code}`, r.status === 400 && (r.json?.error === code || (code === 'reserved' && r.json?.error === 'bad-name')), `${r.status} ${r.json?.error}`);
  }
  r = await post('/cloud/register', { user: 'Alice', password: 'correct horse 1' }, null, { 'X-Real-IP': '10.1.0.1' });
  const alice = cookieOf(r);
  check('register Alice', r.status === 200 && r.json?.user === 'Alice' && !!alice, `${r.status} ${JSON.stringify(r.json)}`);
  const sc = (r.headers['set-cookie'] || [''])[0];
  check('session cookie HttpOnly + SameSite=Strict, no Secure on a dev server', /HttpOnly/.test(sc) && /SameSite=Strict/.test(sc) && !/Secure/.test(sc), sc);
  check('the cookie value is the account key and a 256-bit token', /^nfs5=[0-9a-f]{40}\.[A-Za-z0-9_-]{43}$/.test(alice ?? ''), alice);
  r = await post('/cloud/register', { user: 'ALICE', password: 'another pass 2' }, null, { 'X-Real-IP': '10.1.0.2' });
  check('register the same name in other case: 409', r.status === 409 && r.json?.error === 'taken');
  const acctFile = fs.readdirSync(path.join(TMP, 'cloud', 'accounts'))[0], acct = JSON.parse(fs.readFileSync(path.join(TMP, 'cloud', 'accounts', acctFile), 'utf8'));
  check('the account file keeps a scrypt hash and only the token\'s SHA-256', /^s1\$15\$8\$1\$/.test(acct.pw) && !fs.readFileSync(path.join(TMP, 'cloud', 'accounts', acctFile), 'utf8').includes('correct horse') &&
    acct.sessions[0].h === crypto.createHash('sha256').update(alice.split('.')[1]).digest('hex'));
  if (process.platform !== 'win32') check('account file mode 0600', (fs.statSync(path.join(TMP, 'cloud', 'accounts', acctFile)).mode & 0o777) === 0o600);
  r = await req('GET', '/cloud/me', { headers: { Cookie: alice } });
  check('me with the session', r.json?.user === 'Alice');
  for (const [what, c] of [['a random token', alice.replace(/\.(.*)$/, '.' + crypto.randomBytes(32).toString('base64url'))],
    ['another key with this token', alice.replace(/^nfs5=[0-9a-f]{40}/, 'nfs5=' + 'a'.repeat(40))], ['garbage', 'nfs5=../../x'], ['a wrong cookie name', alice.replace('nfs5=', 'nfs6=')]]) {
    r = await req('GET', '/cloud/me', { headers: { Cookie: c } });
    check(`forged session (${what}): not signed in`, r.status === 200 && r.json?.user === null);
  }
  r = await post('/cloud/login', { user: 'alice', password: 'wrong password' }, null, { 'X-Real-IP': '10.2.0.1' });
  check('login wrong password: 401, same text as an unknown name', r.status === 401 && r.json?.message === 'Wrong login or password');
  const t0 = Date.now(); r = await post('/cloud/login', { user: 'nobody123', password: 'whatever12' }, null, { 'X-Real-IP': '10.2.0.1' }); const tNobody = Date.now() - t0;
  check('login unknown name: 401 and a scrypt\'s time too', r.status === 401 && r.json?.message === 'Wrong login or password' && tNobody > 20, `${tNobody} ms`);
  r = await post('/cloud/login', { user: 'alice', password: 'correct horse 1' }, null, { 'X-Real-IP': '10.2.0.1' });
  const alice2 = cookieOf(r);
  check('login in another letter case', r.status === 200 && r.json?.user === 'Alice' && alice2 && alice2 !== alice);

  // ------------------------------------------------------------------------------------- CSRF
  r = await req('POST', '/cloud/logout', { headers: { Cookie: alice, 'Content-Type': 'application/json' }, body: '{}' });
  check('a change without X-NFS5 (a form of another site): 403', r.status === 403);
  r = await post('/cloud/logout', {}, alice, { Origin: 'https://evil.example' });
  check('a change from another origin: 403', r.status === 403);
  r = await req('POST', '/cloud/login', { body: 'user=alice&password=correct+horse+1', headers: { 'X-NFS5': '1', 'Content-Type': 'application/x-www-form-urlencoded' } });
  check('a form-encoded login: 415', r.status === 415);
  r = await req('GET', '/cloud/logout', { headers: { Cookie: alice } });
  check('logout by GET (a link or image): 404, the session stays', r.status === 404 && (await req('GET', '/cloud/me', { headers: { Cookie: alice } })).json?.user === 'Alice');
  r = await req('OPTIONS', '/cloud/save', { headers: { Origin: 'https://evil.example', 'Access-Control-Request-Method': 'PUT' } });
  check('no CORS preflight answer', !r.headers['access-control-allow-origin']);

  // ------------------------------------------------------------------------------------- saves
  const files1 = saveFiles(1, 2);
  const zip1 = Buffer.from(await Z.zip(files1));
  const hash1 = await Z.contentHash(files1);
  r = await put(zip1, alice, { 'If-Match': '"7"' });
  check('first upload with an If-Match of nothing there: 412', r.status === 412 && r.json?.save === null);
  r = await put(zip1, alice, { 'If-None-Match': '*', 'X-Device': 'Chrome on Windows<script>' });
  check('first upload: rev 1, the server\'s hash = the page\'s', r.status === 200 && r.json?.save?.rev === 1 && r.json.save.hash === hash1, `${r.status} ${JSON.stringify(r.json)}`);
  check('upload: 2 profiles counted, device label cleaned', r.json?.save?.profiles === 2 && r.json.save.dev === 'Chrome on Windowsscript');
  r = await req('GET', '/cloud/save', { headers: { Cookie: alice } });
  check('download = the upload, as an attachment', r.status === 200 && r.body.equals(zip1) && /attachment; filename="nfs5-saves-Alice-/.test(r.headers['content-disposition'] ?? ''));
  const back = await Z.unzip(new Uint8Array(r.body));
  check('the page\'s unzip reads it back', back.length === files1.length && back.every((e, i) => Buffer.from(e.data).equals(files1[i].data) && e.name === files1[i].name));
  r = await put(zip1, alice, { 'If-Match': '"1"' });
  check('the same content again: no new revision', r.status === 200 && r.json?.save?.rev === 1);
  const files2 = saveFiles(2, 2), zip2 = Buffer.from(await Z.zip(files2));
  r = await put(zip2, alice, { 'If-Match': '"1"', 'X-Device': 'Chrome on Windows' });
  check('upload on rev 1: rev 2', r.json?.save?.rev === 2, JSON.stringify(r.json));
  r = await put(Buffer.from(await Z.zip(saveFiles(3, 2))), alice2, { 'If-Match': '"1"', 'X-Device': 'Safari on iPhone' });
  check('a stale device (rev 1): 412 with the cloud\'s info', r.status === 412 && r.json?.save?.rev === 2 && r.json.save.dev === 'Chrome on Windows');
  r = await put(Buffer.from(await Z.zip(saveFiles(3, 2))), alice2, { 'X-Overwrite': '1', 'X-Device': 'Safari on iPhone' });
  check('overwrite (newest wins): rev 3, rev 2 kept as a backup', r.json?.save?.rev === 3 && r.json.backups?.length >= 1 && r.json.backups[0].dev === 'Chrome on Windows', JSON.stringify(r.json));
  const bid = r.json?.backups?.[0]?.id;
  r = await req('GET', `/cloud/save?b=${bid}`, { headers: { Cookie: alice } });
  check('a backup downloads', r.status === 200 && r.body.equals(zip2));
  for (const b of ['../cur', 'x'.repeat(30), '%2e%2e']) {
    r = await req('GET', `/cloud/save?b=${encodeURIComponent(b)}`, { headers: { Cookie: alice } });
    check(`backup id ${JSON.stringify(b)}: refused`, r.status === 400 || r.status === 404, String(r.status));
  }
  r = await put(Buffer.from(await Z.zip(saveFiles(4, 1))), alice, { 'If-Match': '"3"', 'X-Device': 'Safari on iPhone' });
  check('fewer profiles than before: the old one is kept as a backup', r.json?.save?.profiles === 1 && r.json.backups?.[0]?.profiles === 2);
  for (let i = 5; i < 12; i++) await put(Buffer.from(await Z.zip(saveFiles(i, 1))), alice, { 'X-Overwrite': '1', 'X-Device': `Dev ${i}` });
  r = await req('GET', '/cloud/me', { headers: { Cookie: alice } });
  const onDisk = fs.readdirSync(path.join(TMP, 'cloud', 'saves', acctFile.slice(0, 40)));
  check('at most 5 backups, the files match the list', r.json?.backups?.length === 5 && onDisk.length === 6, `${r.json?.backups?.length} listed, files ${onDisk.join(',')}`);
  r = await req('GET', '/cloud/save');
  check('download without a session: 401', r.status === 401);
  r = await put(zip1, null, { 'If-None-Match': '*' });
  check('upload without a session: 401', r.status === 401);

  // ---- broken and hostile zips
  const cur = (await req('GET', '/cloud/me', { headers: { Cookie: alice } })).json.save.rev;
  const bad = [
    ['not a zip', Buffer.from('hello world, not a zip at all')],
    ['a path in a name', rawZip([{ name: '../evil.sav', data: Buffer.from('x') }])],
    ['a folder in a name', rawZip([{ name: 'SaveData/x.sav', data: Buffer.from('x') }])],
    ['a backslash path', rawZip([{ name: '..\\..\\x.sav', data: Buffer.from('x') }])],
    ['a control character', rawZip([{ name: 'a\u0001.sav', data: Buffer.from('x') }])],
    ['a colon (Windows stream)', rawZip([{ name: 'a.sav:zone', data: Buffer.from('x') }])],
    ['two names that differ in case', rawZip([{ name: 'a.sav', data: Buffer.from('x') }, { name: 'A.SAV', data: Buffer.from('y') }])],
    ['a wrong CRC', rawZip([{ name: 'a.sav', data: Buffer.from('xyz') }], { badCrc: true })],
    ['encrypted', rawZip([{ name: 'a.sav', data: Buffer.from('xyz') }], { flags: 0x801 })],
    ['bzip2', rawZip([{ name: 'a.sav', data: Buffer.from('xyz') }], { method: 12 })],
    ['a zip bomb (says 100 bytes, inflates to 50 MB)', rawZip([{ name: 'a.sav', data: Buffer.alloc(50 << 20), deflate: true }], { lieSize: 100 })],
    ['a file over 1 MB', rawZip([{ name: 'a.sav', data: Buffer.alloc((1 << 20) + 1), deflate: true }])],
    ['65 files', rawZip(Array.from({ length: 65 }, (_, i) => ({ name: `f${i}.sav`, data: Buffer.from('x') })))],
    ['no files', rawZip([])],
    ['a non-UTF-8 name without the flag', rawZip([{ name: 'café.sav', data: Buffer.from('x') }], { flags: 0 })],
  ];
  for (const [what, z] of bad) {
    r = await put(z, alice, { 'X-Overwrite': '1' });
    check(`hostile zip refused: ${what}`, r.status === 400, `${r.status} ${r.json?.message}`);
  }
  r = await put(crypto.randomBytes(2 << 20), alice, { 'X-Overwrite': '1' });
  check('a 2 MB body: 413', r.status === 413, String(r.status));
  r = await req('GET', '/cloud/me', { headers: { Cookie: alice } });
  check('after all that the cloud save is untouched', r.json?.save?.rev === cur);
  check('nothing but cur.zip and b-*.zip in the account\'s folder', fs.readdirSync(path.join(TMP, 'cloud', 'saves', acctFile.slice(0, 40))).every(f => /^(cur|b-[a-z0-9]+)\.zip$/.test(f)));

  // ------------------------------------------------------------------------------------- password, sessions
  r = await post('/cloud/password', { old: 'nope nope', password: 'new password 3' }, alice, { 'X-Real-IP': '10.3.0.1' });
  check('password change with a wrong current one: 401', r.status === 401);
  r = await post('/cloud/password', { old: 'correct horse 1', password: 'new password 3' }, alice, { 'X-Real-IP': '10.3.0.1' });
  check('password change', r.status === 200);
  check('the other device is signed out', (await req('GET', '/cloud/me', { headers: { Cookie: alice2 } })).json?.user === null);
  check('this device stays signed in', (await req('GET', '/cloud/me', { headers: { Cookie: alice } })).json?.user === 'Alice');
  r = await post('/cloud/login', { user: 'Alice', password: 'correct horse 1' }, null, { 'X-Real-IP': '10.3.0.2' });
  check('the old password no longer works', r.status === 401);
  r = await post('/cloud/logout', {}, alice);
  check('logout clears the cookie', r.status === 200 && /Max-Age=0/.test((r.headers['set-cookie'] || [''])[0]));
  check('the logged-out token is dead', (await req('GET', '/cloud/me', { headers: { Cookie: alice } })).json?.user === null);

  // ------------------------------------------------------------------------------------- limits
  let limited = 0;
  for (let i = 0; i < 18; i++) { r = await post('/cloud/login', { user: 'Alice', password: 'guess ' + i }, null, { 'X-Real-IP': '10.4.0.9' }); if (r.status === 429) limited++; }
  check('password guessing from one address: 429 after 15 tries', limited === 3, `${limited} limited`);
  check('429 says when to try again', !!r.headers['retry-after']);
  r = await post('/cloud/login', { user: 'Alice', password: 'new password 3' }, null, { 'X-Real-IP': '10.4.0.10' });
  check('another address still signs in', r.status === 200);
  const parallel = await Promise.all(Array.from({ length: 40 }, (_, i) => post('/cloud/login', { user: 'Alice', password: 'p' + i + 'xxxxxxx' }, null, { 'X-Real-IP': '10.5.0.1' })));
  const pStat = parallel.map(x => x.status);
  check('40 parallel guesses: at most 15 checked (counted before the check)', pStat.filter(s => s === 401).length <= 15 && pStat.every(s => [401, 429, 503].includes(s)), pStat.join(','));
  let reg = 0;
  for (let i = 0; i < 7; i++) { r = await post('/cloud/register', { user: `Spam${i}x`, password: 'spam password ' + i }, null, { 'X-Real-IP': '10.6.0.1' }); if (r.status === 200) reg++; }
  check('new accounts from one address: 5 an hour', reg === 5, `${reg} made`);
  r = await post('/cloud/register', { user: 'Other1', password: 'other password 1' }, null, { 'X-Real-IP': '10.6.0.2' });
  check('another address can still register', r.status === 200);
  const v6a = await post('/cloud/login', { user: 'zz', password: 'x' }, null, { 'X-Real-IP': '2001:db8:1:2::1' });
  check('an IPv6 address is accepted', v6a.status === 401 || v6a.status === 400, String(v6a.status));

  // ------------------------------------------------------------------------------------- delete
  const bob = cookieOf(await post('/cloud/register', { user: 'Bob_1', password: 'bob password 1' }, null, { 'X-Real-IP': '10.7.0.1' }));
  await put(zip1, bob, { 'If-None-Match': '*' });
  r = await post('/cloud/delete', { password: 'wrong one 12' }, bob, { 'X-Real-IP': '10.7.0.1' });
  check('delete with a wrong password: 401', r.status === 401);
  r = await post('/cloud/delete', { password: 'bob password 1' }, bob, { 'X-Real-IP': '10.7.0.1' });
  const bobKey = crypto.createHash('sha256').update('bob_1').digest('hex').slice(0, 40);
  check('delete: the account and its saves are gone', r.status === 200 && !fs.existsSync(path.join(TMP, 'cloud', 'accounts', bobKey + '.json')) && !fs.existsSync(path.join(TMP, 'cloud', 'saves', bobKey)));
  r = await post('/cloud/register', { user: 'bob_1', password: 'bob password 2' }, null, { 'X-Real-IP': '10.7.0.2' });
  check('the name is free again', r.status === 200);

  // ------------------------------------------------------------------------------------- internal port, taken
  r = await req('GET', '/whoami', { headers: { Cookie: cookieOf(await post('/cloud/login', { user: 'Other1', password: 'other password 1' }, null, { 'X-Real-IP': '10.8.0.1' })) }, port: INTERNAL });
  check('internal /whoami names the cookie\'s account', r.json?.user === 'Other1');
  r = await req('GET', '/taken?u=ALICE', { port: INTERNAL });
  check('internal /taken (any case)', r.json?.taken === true);

  // ------------------------------------------------------------------------------------- the site server
  r = await req('GET', '/');
  const csp = r.headers['content-security-policy'] ?? '';
  check('pages: a CSP without inline scripts, no framing', /script-src 'self' 'wasm-unsafe-eval'/.test(csp) && /frame-ancestors 'none'/.test(csp) && r.headers['x-frame-options'] === 'DENY', csp);
  r = await req('GET', '/test-dev.json');
  check('a dev server says so', r.status === 200 && r.json?.dev === true);
  for (const [what, p, h] of [['a bad %-escape', '/%E0%A4%A'], ['a negative block', '/data/PORSCHE.EXE?b=-5'], ['a block of letters', '/data/PORSCHE.EXE?b=abc'],
    ['a Range past the end', '/data/PORSCHE.EXE', { Range: 'bytes=999999999-' }], ['a reversed Range', '/data/PORSCHE.EXE', { Range: 'bytes=500-100' }],
    ['a huge Range number', '/data/PORSCHE.EXE', { Range: 'bytes=99999999999999999999-' }], ['a bad %-escape under /cloud', '/cloud/%E0%A4%A']]) {
    r = await req('GET', p, { headers: h });
    await wait(100);
    const alive = await req('GET', '/cloud/me');
    check(`${what}: answered ${r.status}, the server lives`, alive.status === 200 && !srv.dead, `${r.status}`);
  }
  // the public server: Secure __Host- cookie, HSTS, no dev answers
  r = await post('/cloud/register', { user: 'PubUser', password: 'public password 1' }, null, {}, PUB);
  const psc = (r.headers['set-cookie'] || [''])[0];
  check('public: the cookie is __Host- and Secure', /^__Host-nfs5=/.test(psc) && /; Secure/.test(psc), psc);
  r = await req('GET', '/test-dev.json', { port: PUB });
  check('public: /test-dev.json says dev false (the page ignores test parameters)', r.status === 200 && r.json?.dev === false);
  r = await req('GET', '/test-seed/XXDefXX.sav', { port: PUB });
  check('public: no /test-seed', r.status === 404);
  r = await req('GET', '/', { port: PUB });
  check('public: HSTS on pages', /max-age=/.test(r.headers['strict-transport-security'] ?? ''));

  // ------------------------------------------------------------------------------------- online names
  const ws = (cookie, origin) => new Promise(resolve => {
    const s = net.connect(PORT, '127.0.0.1', () => s.write(`GET /lan HTTP/1.1\r\nHost: 127.0.0.1:${PORT}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
      `Sec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n${origin ? `Origin: ${origin}\r\n` : ''}${cookie ? `Cookie: ${cookie}\r\n` : ''}\r\n`));
    let got = Buffer.alloc(0);
    s.on('data', d => { got = Buffer.concat([got, d]); if (got.includes('\r\n\r\n')) resolve({ s, ok: got.toString().startsWith('HTTP/1.1 101'), head: got }); });
    s.on('close', () => resolve({ s, ok: false }));
    s.on('error', () => resolve({ s, ok: false }));
    setTimeout(() => resolve({ s, ok: false }), 4000);
  });
  const wsFrame = payload => { const mask = crypto.randomBytes(4), n = payload.length; const p = Buffer.from(payload); for (let i = 0; i < n; i++) p[i] ^= mask[i & 3]; return Buffer.concat([Buffer.from([0x82, 0x80 | 126, n >> 8, n & 255]), mask, p]); };
  const join = typed => { const j = Buffer.alloc(1 + 0x88); j[0] = 0x6a; j.write('cuser~', 1 + 0x3c, 'latin1'); j.write(`${typed}~3.5~3.5`, 1 + 0x70, 'latin1');
    const f = Buffer.alloc(16 + j.length); f[0] = 1; f.writeUInt32BE(0x0A4D0001, 8); f.writeUInt16BE(5000, 12); f.writeUInt16BE(9442, 14); j.copy(f, 16); return f; };
  let w = await ws(null, 'https://evil.example');
  check('online: a WebSocket from another site is refused', !w.ok); w.s.destroy();
  const other1 = cookieOf(await post('/cloud/login', { user: 'Other1', password: 'other password 1' }, null, { 'X-Real-IP': '10.8.0.2' }));
  w = await ws(other1, `http://127.0.0.1:${PORT}`);
  check('online: the site\'s own page connects', w.ok);
  w.s.write(wsFrame(join('ProfileName')));
  const w2 = await ws(null, `http://127.0.0.1:${PORT}`);
  w2.s.write(wsFrame(join('alice')));
  const w3 = await ws(null, null);
  w3.s.write(wsFrame(join('Fresh\u0007Name~x')));
  await wait(1500);
  const logText = srvLog.join('');
  check('online: a signed-in player plays under the account name', /"ProfileName" plays as "Other1" \(signed in\)/.test(logText));
  check('online: a guest typing an account\'s name becomes -guest', /"alice" plays as "alice-guest"/.test(logText));
  check('online: a name\'s control characters and ~ are dropped', /persona="FreshName"/.test(logText), (logText.match(/persona="[^"]*Fresh[^"]*"/) || ['no line'])[0]);
  for (const x of [w, w2, w3]) x.s.destroy();
  const many = [];
  for (let i = 0; i < 20; i++) many.push(await ws(null, null).then(x => { x.s.headers = 1; return x; }));
  const manyIp = [];
  for (let i = 0; i < 18; i++) {
    manyIp.push(await new Promise(resolve => {
      const s = net.connect(PORT, '127.0.0.1', () => s.write(`GET /lan HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nX-Real-IP: 10.9.9.9\r\n` +
        `Sec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
      s.once('data', d => resolve({ s, ok: d.toString().startsWith('HTTP/1.1 101') }));
      s.on('error', () => resolve({ s, ok: false }));
    }));
  }
  check('online: 16 connections per address, then refused', manyIp.filter(x => x.ok).length === 16, `${manyIp.filter(x => x.ok).length} accepted`);
  check('online: loopback connections nginx did not name are not limited per address', many.every(x => x.ok));
  for (const x of [...many, ...manyIp]) x.s.destroy();
  check('the server is still alive at the end', !srv.dead && (await req('GET', '/cloud/me')).status === 200);
} finally {
  srv.kill(); pub.kill();
  await wait(300);
  if (failed) console.log(srv.out.split('\n').filter(l => /error|Error/.test(l)).slice(0, 20).join('\n'));
  fs.rmSync(TMP, { recursive: true, force: true });
}
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
