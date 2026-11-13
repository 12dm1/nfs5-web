// ON1: two games in headless Chrome on one server, through the Internet mode's menus into one race
// (runtime/nettest.c NETTEST=online-host|online-join drives the menus, AI drives the cars).
//   node web/tools/online_test.mjs [--seconds 240] [--port 8079] [--mode 0|1|2] [--players 2] [--env K=V,K=V]
// Prints the games' lines prefixed with their tag and ends with PASS when every game got the race start and its
// cars moved on both sides (nettest's monitor lines), FAIL otherwise. The server's own log: out/online/.
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const SECONDS = +arg('seconds', 240), PORT = +arg('port', 8079), PLAYERS = +arg('players', 2);
const MODE = arg('mode', ''), EXTRA = arg('env', '');
const SEED = arg('seed', ['player.lst', '5îïðïðî.sav', 'îïðïðî1.hud', 'îïðïðî2.hud'].join(','));

const lines = [];
const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT), ...process.argv.slice(2).filter(a => a === '--net-verbose')],
  { stdio: ['ignore', 'pipe', 'inherit'] });
srv.stdout.on('data', d => { for (const l of d.toString().split('\n')) if (l.trim()) { lines.push(l); console.log(l); } });
await new Promise(r => setTimeout(r, 1000));
const browser = await chromium.launch({
  executablePath: arg('chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe'), headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
});
const games = [];
for (let i = 0; i < PLAYERS; i++) {
  const role = i === 0 ? 'online-host' : 'online-join', tag = i === 0 ? 'H' : `J${i}`;
  const env = [`NETTEST=${role}`, 'NETTEST_AI=1', `NETTEST_ACCOUNT=${i === 0 ? 'host' : 'joiner' + i}`, `NETTEST_PLAYERS=${PLAYERS}`,
               ...(MODE !== '' ? [`NETTEST_MODE=${MODE}`] : []), ...(EXTRA ? EXTRA.split(',') : [])].join(',');
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 960 } });
  const page = await ctx.newPage();
  page.on('pageerror', e => console.log(`[${tag} pageerror] ${e.message}`));
  page.on('console', m => { if (m.type() === 'error') console.log(`[${tag} console] ${m.text()}`); });
  const url = `http://127.0.0.1:${PORT}/?log=1&tag=${tag}&env=${encodeURIComponent(env)}&seed=${encodeURIComponent(SEED)}`;
  await page.goto(url);
  games.push({ tag, page });
  await new Promise(r => setTimeout(r, 3000));         // launches apart (the user's rule for parallel games)
}
const t0 = Date.now();
let verdict = null;
while (Date.now() - t0 < SECONDS * 1000 && !verdict) {
  await new Promise(r => setTimeout(r, 2000));
  const all = lines.join('\n');
  if (/FAIL:/.test(all)) verdict = 'FAIL (a game reported FAIL)';
  const started = games.filter(g => new RegExp(`^\\[${g.tag}\\].*race start received`, 'm').test(all)).length;
  if (started === games.length) {
    // the cars move: the last monitor line of each game shows every car with a slice > 20
    const ok = games.every(g => {
      const m = [...all.matchAll(new RegExp(`^\\[${g.tag}\\].*car slices(.*)$`, 'gm'))].pop();
      const sl = m ? [...m[1].matchAll(/ \d+:(-?\d+)/g)].map(x => +x[1]) : [];
      return sl.length >= games.length && sl.every(v => v > 20);
    });
    // career (--mode 2): the whole tournament, up to the server's final table
    if (ok && (MODE !== '2' || /\]\s+game #\d+: final/.test(all))) verdict = 'PASS';
  }
}
console.log(verdict || 'FAIL (timeout)');
await browser.close();
srv.kill();
process.exit(verdict === 'PASS' ? 0 : 1);
