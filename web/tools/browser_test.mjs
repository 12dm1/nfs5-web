// Runs the page in headless Chrome and prints what the game logs; no window on the user's screen.
//   node web/tools/browser_test.mjs [--seconds 60] [--url '/?log=1&...'] [--port 8077]
// The game's log comes through the dev server's stdout (POST /test-log), so start this with the server as child.
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const SECONDS = +arg('seconds', 60), PORT = +arg('port', 8077), URLPATH = arg('url', '/?log=1');
const eval_ = arg('eval', '');

const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT)], { stdio: ['ignore', 'inherit', 'inherit'] });
await new Promise(r => setTimeout(r, 800));
const launch = {
  executablePath: arg('chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe'),
  headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
  viewport: { width: 1280, height: 960 },
};
// --profile <folder>: a browser profile that stays, and with it the game's save folder (OPFS)
const prof = arg('profile', '');
const browser = prof ? await chromium.launchPersistentContext(prof, launch) : await (await chromium.launch(launch)).newContext({ viewport: launch.viewport });
const page = await browser.newPage();
page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') console.log('[console ' + m.type() + '] ' + m.text()); });
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
if (process.argv.includes('--fakepad')) await page.addInitScript(() => {
  const pad = { connected: true, mapping: 'standard', timestamp: 1, axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
  window.__pad = pad;
  navigator.getGamepads = () => [pad];
  document.hasFocus = () => true;
});
await page.goto(`http://127.0.0.1:${PORT}${URLPATH}`);
// --script <file>: lines of "wait <s>", "move <x> <y>", "click <x> <y>" (640x480 coordinates), "type <text>", "key <Name>",
// "shot" (the next frame into out/web/shot_<frame>.rgba), "eval <js>"
const script = arg('script', '');
if (script) {
  const P = (x, y) => [+x * 2, +y * 2];
  for (const line of fs.readFileSync(script, 'utf8').split(String.fromCharCode(10))) {
    const [c, ...a] = line.trim().split(' ');
    if (!c || c.startsWith('#')) continue;
    if (c === 'wait') await new Promise(r => setTimeout(r, +a[0] * 1000));
    else if (c === 'move') await page.mouse.move(...P(a[0], a[1]));
    else if (c === 'click') { await page.mouse.move(...P(a[0], a[1])); await new Promise(r => setTimeout(r, 200)); await page.mouse.down(); await new Promise(r => setTimeout(r, 120)); await page.mouse.up(); }
    else if (c === 'type') await page.keyboard.type(a.join(' '), { delay: 120 });
    else if (c === 'key') await page.keyboard.press(a[0], { delay: 120 });
    else if (c === 'keydown') await page.keyboard.down(a[0]);
    else if (c === 'keyup') await page.keyboard.up(a[0]);
    else if (c === 'pad') await page.evaluate(list => {   // pad lx=-1 ly=0 rx=0 ry=0, or a button number: b0=1 (A), b6=0.5 (left trigger)
      for (const e of list) {
        const [n, v] = e.split('='), f = +v, ax = ['lx', 'ly', 'rx', 'ry'].indexOf(n);
        if (ax >= 0) window.__pad.axes[ax] = f;
        else { const b = window.__pad.buttons[+n.slice(1)]; b.value = f; b.pressed = f > 0; }
      }
    }, a);
    else if (c === 'shot') await page.evaluate('stats.shotNext = true');
    else if (c === 'eval') console.log('[eval] ' + line.trim().slice(5) + ' => ' + JSON.stringify(await page.evaluate(a.join(' '))));
    console.log('[script] ' + line.trim());
  }
}
await new Promise(r => setTimeout(r, SECONDS * 1000));
if (eval_) console.log('[eval] ' + JSON.stringify(await page.evaluate(eval_)));
await browser.close();
srv.kill();
process.exit(0);
