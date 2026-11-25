// A race (or a menu) in headless Chrome for looks and timings by numbers, with a browser profile that stays (its OPFS saves).
//   node web/tools/race_look.mjs --port 8137 --profile <dir> [--url '/?log=1&...'] [--steps "CREATEPLAYERENTER,type:Glass,..."]
//        [--seconds 90] [--until <regex>] [--keys "<s>:<Key>,..."] [--log <file>] [--server-args "..."]
// The game's log goes to --log (and the lines matching --grep to stdout). --until ends the run at the first log line that matches.
// --steps: menu buttons by command (as web/tools/menu_walk.mjs), "type:<text>", "wait:<s>", "key:<Key>", "click:<x>:<y>",
//   "dump" (the buttons with a command), "dumpall" (every visible control: command or vtable, x, y, w, h).
// --keys: keys pressed at seconds after the start (each held 150 ms), for a race camera or a pause.
// --keys "<s>:SHOT" takes the next frame as raw RGBA (out/web/shot_<frame>.rgba).
// --react '<regex>|<s>:<Key>,...[;;<regex>|...]': the same keys, timed from the first log line that matches (the race's start,
//   its end).
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8137), SECONDS = +arg('seconds', 90), URLPATH = arg('url', '/?log=1&nocloud=1');
const UNTIL = arg('until', '') ? new RegExp(arg('until', '')) : null;
const GREP = new RegExp(arg('grep', 'TRAP|MESSAGEBOX|page error|pageerror|\\[gpu\\] (error|shader)|\\[glass|\\[camera|\\[fps\\]|quickrace'));
const logFile = arg('log', '') ? fs.createWriteStream(arg('log', '')) : null;
let done = null; const finished = new Promise(r => { done = r; });
const t0 = Date.now();
let pressKeys = null;
const REACTS = arg('react', '').split(';;').filter(Boolean).map(r => { const [re, keys] = r.split('|'); return { re: new RegExp(re), keys }; });
const onLine = (line) => {
  logFile?.write(line + '\n');
  for (const r of REACTS) if (r.re && pressKeys && r.re.test(line)) { r.re = null; logFile?.write('[race_look] react: ' + r.keys + '\n'); pressKeys(r.keys); }
  if (GREP.test(line)) console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}] ${line.slice(0, 260)}`);
  if (UNTIL && UNTIL.test(line)) { console.log('[race_look] --until matched'); done(); }
};
const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT), ...arg('server-args', '').split(' ').filter(Boolean)],
  { stdio: ['ignore', 'pipe', 'pipe'] });
let buf = '';
const feed = (d) => { buf += d.toString(); let i; while ((i = buf.indexOf('\n')) >= 0) { onLine(buf.slice(0, i).replace(/\r$/, '')); buf = buf.slice(i + 1); } };
srv.stdout.on('data', feed); srv.stderr.on('data', feed);
await new Promise(r => setTimeout(r, 1000));
const launch = {
  executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
  viewport: { width: +arg('width', 1280), height: +arg('height', 960) },
};
const prof = arg('profile', '');
const ctx = prof ? await chromium.launchPersistentContext(prof, launch) : await (await chromium.launch(launch)).newContext({ viewport: launch.viewport });
const page = await ctx.newPage();
page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') onLine('[console ' + m.type() + '] ' + m.text()); });
page.on('pageerror', e => onLine('[pageerror] ' + e.message));
try {
  await page.goto(`http://127.0.0.1:${PORT}${URLPATH}`);
  const widgets = () => page.evaluate(() => globalThis.nfs5TextFields?.(true) ?? []);
  for (const s of arg('steps', '').split(',').filter(Boolean)) {
    if (s.startsWith('wait:')) { await page.waitForTimeout(+s.slice(5) * 1000); continue; }
    if (s.startsWith('type:')) { await page.keyboard.type(s.slice(5), { delay: 150 }); continue; }
    if (s.startsWith('key:')) { await page.keyboard.press(s.slice(4), { delay: 150 }); continue; }
    if (s.startsWith('click:')) {   // click:<x>:<y> in the menus' 640x480
      const [x, y] = s.slice(6).split(':').map(Number), b = await page.locator('canvas').boundingBox();
      await page.mouse.click(b.x + x / 640 * b.width, b.y + y / 480 * b.height, { delay: 300 }); continue;
    }
    if (s === 'dumpall') { onLine('[race_look] widgets ' + JSON.stringify((await widgets()).map(x => [x.cmd || (x.vt >>> 0).toString(16), x.x, x.y, x.w, x.h]))); continue; }
    if (s === 'dump') { onLine('[race_look] commands ' + JSON.stringify((await widgets()).filter(x => x.cmd).map(x => [x.cmd, x.x, x.y, x.w, x.h]))); continue; }
    let w = null;
    for (let t = 0; t < 120 && !w; t++) { w = (await widgets()).find(x => x.cmd === s); if (!w) await page.waitForTimeout(500); }
    if (!w) { onLine(`[race_look] step ${s}: not on the page`); break; }
    const before = JSON.stringify(await widgets());
    for (let k = 0; k < 6; k++) {
      const b = await page.locator('canvas').boundingBox();
      await page.mouse.click(b.x + (w.x + w.w / 2) / 640 * b.width, b.y + (w.y + w.h / 2) / 480 * b.height, { delay: 300 });
      let changed = false;
      for (let t = 0; t < 25 && !changed; t++) { await page.waitForTimeout(400); changed = JSON.stringify(await widgets()) !== before; }
      if (changed) break;
    }
    onLine('[race_look] clicked ' + s);
    await page.waitForTimeout(800);
  }
  pressKeys = (list) => {
    for (const k of list.split(',').filter(Boolean)) {
      const [s, key] = k.split(':');
      void (async () => {
        await new Promise(r => setTimeout(r, +s * 1000)); onLine(`[race_look] key ${key}`);
        // SHOT: the next frame's raw RGBA to out/web/shot_<frame>.rgba (web/src/main.ts, renderer.ts shotNext)
        if (key === 'SHOT') await page.evaluate(() => { globalThis.stats.shotNext = true; }).catch(() => {});
        else await page.keyboard.press(key, { delay: 150 }).catch(() => {});
      })();
    }
  };
  pressKeys(arg('keys', ''));
  await Promise.race([finished, new Promise(r => setTimeout(r, SECONDS * 1000))]);
  const ev = arg('eval', '');
  if (ev) onLine('[eval] ' + JSON.stringify(await page.evaluate(ev)));
} finally {
  await ctx.close().catch(() => {});
  srv.kill();
  logFile?.end();
}
process.exit(0);
