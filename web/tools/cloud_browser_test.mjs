// CLOUD1 end to end in headless Chrome with the real game: two "devices" (two browser profiles, each its own OPFS) and
// one account. Device A creates the account on the PLAY page and plays; device B signs in and gets A's saves; then
// both change their saves and the newer one wins, the other kept as a backup. Also checks that the game runs under the
// site's Content-Security-Policy (no CSP errors, frames drawn, the online link up).
//   node web/tools/cloud_browser_test.mjs [--port 8195] [--chrome <chrome.exe>] [--keep]
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url)), WEB = path.dirname(HERE);
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8195), BASE = `http://127.0.0.1:${PORT}`;
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'nfs5-cloud-e2e-'));
const wait = ms => new Promise(r => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = '') => { if (ok) passed++; else failed++; console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${!ok && detail ? `   (${detail})` : ''}`); };

const srv = spawn(process.execPath, [path.join(WEB, 'server.mjs'), '--port', String(PORT), '--cars', '', '--cloud', path.join(TMP, 'cloud'), '--logs', path.join(TMP, 'logs')],
  { cwd: path.dirname(WEB), stdio: ['ignore', 'pipe', 'pipe'] });
let out = '';
srv.stdout.on('data', d => { out += d; });
srv.stderr.on('data', d => { out += d; });
await wait(1500);
/** waits for a server output line matching re (after `from`): the match, or null after ms */
async function until(re, ms, from = 0) {
  const t = Date.now();
  while (Date.now() - t < ms) { const m = re.exec(out.slice(from)); if (m) return m; await wait(250); }
  return null;
}

const launch = {
  executablePath: arg('chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe'), headless: true, viewport: { width: 1280, height: 960 },
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
};
const consoleErrors = [];
async function device(name) {
  const ctx = await chromium.launchPersistentContext(path.join(TMP, 'profile-' + name), launch);
  const page = ctx.pages()[0] ?? await ctx.newPage();
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(`[${name}] ${m.text()}`); });
  page.on('pageerror', e => consoleErrors.push(`[${name}] pageerror ${e.message}`));
  return { ctx, page, name };
}
const listSaves = page => page.evaluate(async () => {
  try {
    const d = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('save')).getDirectoryHandle('SaveData');
    const outl = [];
    for await (const [n, h] of d.entries()) if (h.kind === 'file') { const b = new Uint8Array(await (await h.getFile()).arrayBuffer()); let s = 0; for (const x of b) s = (s * 31 + x) >>> 0; outl.push(`${n}:${b.length}:${s}`); }
    return outl.sort();
  } catch { return []; }
});
/** a save changed outside the game (as if played): the first .sav's last byte, or a new small one */
const touchSave = (page, tag) => page.evaluate(async tag => {
  const d = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('save', { create: true })).getDirectoryHandle('SaveData', { create: true });
  const w = await (await d.getFileHandle(`zz${tag}.sav`, { create: true })).createWritable();
  await w.write(new TextEncoder().encode('test save ' + tag + ' ' + Date.now())); await w.close();
}, tag);
async function playGame(dev, seconds, extra = '') {
  const from = out.length;
  await dev.page.goto(`${BASE}/game?log=1&tag=${dev.name}${extra}`);
  const fps = await until(new RegExp(`\\[${dev.name}\\] \\[fps\\] [\\d.]+ frames/s, frame (\\d{3,})`), seconds * 1000, from);
  return { from, fps };
}
async function leaveGame(dev) { await dev.page.goto(`${BASE}/play`); await wait(500); }

try {
  // ---------------------------------------------------------------- device A: create the account on the PLAY page
  const A = await device('A');
  await A.page.goto(`${BASE}/play`);
  await A.page.waitForFunction(() => /not signed in/.test(document.getElementById('acct')?.textContent ?? ''), null, { timeout: 15000 });
  check('PLAY page: "not signed in"', true);
  await A.page.click('#play');
  check('PLAY without an account opens the account dialog', await A.page.locator('#dlg-sign').evaluate(d => d.open));
  check('it starts with the chooser, "Play as guest" focused', await A.page.evaluate(() => document.activeElement?.id === 'f-guest' && document.getElementById('account').hidden));
  await A.page.click('#c-new');
  await A.page.fill('#f-user', 'Tester_1');
  await A.page.fill('#f-pass', 'short');
  await A.page.click('#f-go');
  check('a short password is refused on the page', /at least 8/.test(await A.page.textContent('#f-err')));
  await A.page.fill('#f-pass', 'long enough pw 1');
  await A.page.fill('#f-pass2', 'long enough pw 1');
  await A.page.click('#f-go');
  await A.page.waitForFunction(() => document.getElementById('dlg-done')?.open, null, { timeout: 15000 });
  check('account created: the "Account ready" dialog', (await A.page.textContent('#done-name')) === 'Tester_1');
  const [download] = await Promise.all([A.page.waitForEvent('download'), A.page.click('#done-file')]);
  const reminder = fs.readFileSync(await download.path(), 'utf8');
  check('the reminder file has the login and password', /Login: Tester_1/.test(reminder) && /Password: long enough pw 1/.test(reminder));
  await Promise.all([A.page.waitForURL(/\/game$/), A.page.click('#done-play')]);
  check('Play goes to /game', true);
  // the game with test logging (the page was on /game already: reload with the log)
  let g = await playGame(A, 150);
  check('device A: the game runs under the CSP (frames drawn)', !!g.fps, out.slice(g.from).split('\n').filter(l => /error|Error|cloud/.test(l)).slice(0, 8).join(' | '));
  check('device A: signed in, cloud checked before the game', !!await until(/\[A\] \[cloud\] Tester_1: here/, 5000, g.from));
  check('device A: the online link is up under the CSP', !!await until(/\[A\] \[lan\] online as/, 20000, g.from));
  // whatever the game wrote on its first start (or nothing), then a change as if played: sent before the next game
  await wait(6000);
  await leaveGame(A);
  await touchSave(A.page, 'A1');
  g = await playGame(A, 150);
  const sentA = await until(/\[A\] \[cloud\] sent \((first upload|changed here)\): rev (\d+)/, 20000, g.from);
  check('device A: its saves are sent before the game', !!sentA, out.slice(g.from).split('\n').filter(l => /cloud/.test(l)).join(' | '));
  const savesA = await listSaves(A.page);
  await leaveGame(A);

  // ---------------------------------------------------------------- device B: sign in, get A's saves
  const B = await device('B');
  await B.page.goto(`${BASE}/play`);
  await B.page.waitForFunction(() => /not signed in/.test(document.getElementById('acct')?.textContent ?? ''), null, { timeout: 15000 });
  await B.page.click('#play');
  await B.page.click('#c-old');
  await B.page.fill('#f-user', 'tester_1');
  await B.page.fill('#f-pass', 'wrong password');
  await B.page.click('#f-go');
  await B.page.waitForFunction(() => (document.getElementById('f-err')?.textContent ?? '').length > 0, null, { timeout: 15000 });
  check('device B: a wrong password says so', /Wrong login or password/.test(await B.page.textContent('#f-err')));
  await B.page.fill('#f-pass', 'long enough pw 1');
  await Promise.all([B.page.waitForURL(/\/game$/, { timeout: 15000 }), B.page.click('#f-go')]);
  check('device B: sign in goes straight to the game', true);
  g = await playGame(B, 150);
  check('device B: took the cloud\'s saves', !!await until(/\[B\] \[cloud\] took rev \d+ from the cloud/, 5000, g.from), out.slice(g.from).split('\n').filter(l => /cloud/.test(l)).join(' | '));
  check('device B: the game runs on them', !!g.fps);
  await wait(6000);
  await leaveGame(B);
  const savesB = await listSaves(B.page);
  check('device B has device A\'s files', savesA.length > 0 && savesA.every(x => savesB.includes(x) || /\.(lst|hud|trk)$/.test(x.split(':')[0]) || savesB.some(y => y.split(':')[0] === x.split(':')[0])),
    `A ${savesA.join(' ')} / B ${savesB.join(' ')}`);
  check('device B has A\'s marker save', savesB.some(x => x.startsWith('zzA1.sav')));

  // ---------------------------------------------------------------- both change: the newer wins
  await touchSave(A.page, 'A2');            // older change
  await wait(2500);
  await touchSave(B.page, 'B2');            // newer change
  g = await playGame(A, 150);               // A sends first (only A changed since its last sync)
  check('device A: its change is sent', !!await until(/\[A\] \[cloud\] sent \(changed here\)/, 20000, g.from), out.slice(g.from).split('\n').filter(l => /cloud/.test(l)).join(' | '));
  await leaveGame(A);
  g = await playGame(B, 150);               // B: both changed, B's is newer -> B's goes to the cloud, A's kept as a cloud backup
  check('device B: both changed, its newer saves win', !!await until(/\[B\] \[cloud\] sent \(newer here\)/, 20000, g.from), out.slice(g.from).split('\n').filter(l => /cloud/.test(l)).join(' | '));
  check('device B: the player is told', !!await until(/\[B\] \[cloud\] Cloud saves: your saves here are newer/, 5000, g.from));
  await leaveGame(B);
  const me = await B.page.evaluate(async () => (await fetch('/cloud/me')).json());
  check('the cloud keeps A\'s version as a backup', me.backups?.some(b => /Chrome/.test(b.dev)), JSON.stringify(me.backups));
  g = await playGame(A, 150);               // A: only the cloud changed -> takes B's, no backup needed
  check('device A: takes B\'s newer saves', !!await until(/\[A\] \[cloud\] took rev \d+ from the cloud/, 20000, g.from), out.slice(g.from).split('\n').filter(l => /cloud/.test(l)).join(' | '));
  await leaveGame(A);
  const after = await listSaves(A.page);
  check('device A now has B\'s version (B2; A2 lives on in the cloud backup)', after.some(x => x.startsWith('zzB2.sav')) && !after.some(x => x.startsWith('zzA2.sav')), after.join(' '));

  // ---------------------------------------------------------------- account panel on the PLAY page
  await A.page.goto(`${BASE}/play`);
  await A.page.waitForFunction(() => /Signed in as/.test(document.getElementById('acct')?.textContent ?? ''), null, { timeout: 15000 });
  check('PLAY page: "Signed in as Tester_1"', /Tester_1/.test(await A.page.textContent('#acct')));
  await A.page.click('#acct >> text=Account');
  await A.page.waitForFunction(() => document.getElementById('dlg-acct')?.open, null, { timeout: 10000 });
  const panel = await A.page.textContent('#acct-body');
  check('account panel: cloud save, older versions, this device, password, delete', ['Cloud save', 'Older cloud versions', 'Saves on this device', 'Password', 'Delete account'].every(s => panel.includes(s)), panel.slice(0, 300));
  const [dl] = await Promise.all([A.page.waitForEvent('download'), A.page.locator('#acct-body .row').first().locator('text=Download').click()]);
  const zipBytes = fs.readFileSync(await dl.path());
  check('download the cloud save: a zip with a SaveData folder', zipBytes.readUInt32LE(0) === 0x04034b50 && zipBytes.includes(Buffer.from('SaveData/')));
  // PLAY while signed in: straight to the game
  await A.page.keyboard.press('Escape');
  await Promise.all([A.page.waitForURL(/\/game$/, { timeout: 10000 }), A.page.click('#play')]);
  check('PLAY when signed in: straight to the game', true);
  await A.page.goto(`${BASE}/play`);

  // ---------------------------------------------------------------- device C: the download fails before the game
  // (a fresh device, the network drops): the game's first saves must not overwrite the cloud career
  const C = await device('C');
  const revBefore = me.save?.rev;
  await C.page.goto(`${BASE}/play`);
  await C.page.waitForFunction(() => /not signed in/.test(document.getElementById('acct')?.textContent ?? ''), null, { timeout: 15000 });
  await C.page.click('#play');
  await C.page.click('#c-old');
  await C.page.fill('#f-user', 'Tester_1');
  await C.page.fill('#f-pass', 'long enough pw 1');
  await Promise.all([C.page.waitForURL(/\/game$/, { timeout: 15000 }), C.page.click('#f-go')]);
  await C.page.route('**/cloud/save*', r => r.request().method() === 'GET' ? r.abort() : r.continue());
  g = await playGame(C, 150);
  const guarded = await until(/\[C\] \[cloud\] the cloud has rev \d+ from .*not overwritten/, 60000, g.from);
  const meC = await C.page.evaluate(async () => (await fetch('/cloud/me')).json());
  check('device C: a failed download, then the game saves: the cloud career is not overwritten', !!guarded && meC.save?.rev === revBefore,
    `rev ${revBefore} -> ${meC.save?.rev}; ${out.slice(g.from).split('\n').filter(l => /\[C\] \[cloud\]/.test(l)).join(' | ')}`);
  await C.page.unroute('**/cloud/save*');
  await leaveGame(C);
  g = await playGame(C, 150);
  check('device C: the next start takes the cloud career', !!await until(/\[C\] \[cloud\] took rev \d+ from the cloud/, 20000, g.from),
    out.slice(g.from).split('\n').filter(l => /\[C\] \[cloud\]/.test(l)).join(' | '));
  await leaveGame(C);
  await C.ctx.close();

  const csp = consoleErrors.filter(e => /Content.Security.Policy|Refused to/i.test(e));
  check('no CSP violation on any page', csp.length === 0, csp.slice(0, 5).join(' | '));
  await A.ctx.close(); await B.ctx.close();
} catch (e) {
  failed++;
  console.log('FAIL  test stopped: ' + (e && e.stack || e));
} finally {
  srv.kill();
  if (process.argv.includes('--verbose')) console.log('--- cloud lines:\n' + out.split('\n').filter(l => /\[cloud\]|\[save\]/.test(l)).join('\n'));
  if (failed) {
    console.log('--- console errors:\n' + consoleErrors.slice(0, 20).join('\n'));
    console.log('--- cloud lines:\n' + out.split('\n').filter(l => /cloud|error|Error/.test(l)).slice(-40).join('\n'));
  }
  await wait(500);
  if (!process.argv.includes('--keep')) fs.rmSync(TMP, { recursive: true, force: true }); else console.log('kept ' + TMP);
}
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
