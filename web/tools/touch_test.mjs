// The on-screen touch controls in headless Chrome with a touch screen: a race (fe=0), the overlay must show while driving, then
// fingers on GAS and the steering stick (CDP touch events, so the page sees pointerType "touch"), then the pause button.
//   node web/tools/touch_test.mjs [--port 8094] [--seconds 20]
// The game's log comes through the dev server's stdout; with PADLOG=1 the race's input packet is printed once a second
// ("[pad] packet <flags> <steer> <throttle> <brake>").
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8094), SECONDS = +arg('seconds', 20);
const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT)], { stdio: ['ignore', 'inherit', 'inherit'] });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({
  executablePath: arg('chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe'), headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
});
const ctx = await browser.newContext({ viewport: { width: 1280, height: 720 }, hasTouch: true });
const page = await ctx.newPage();
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
await page.addInitScript(() => { document.hasFocus = () => true; });
const args = encodeURIComponent('fe=0 nointromovies=1');
await page.goto(`http://127.0.0.1:${PORT}/?log=1&touch=1&seed=XXhud1XX.hud,XXhud2XX.hud&env=PADLOG=1,NFS5_ARGS=${args}`);
const say = s => console.log('[touch_test] ' + s);
const t0 = Date.now();
while (!(await page.evaluate(() => !!document.querySelector('#touch.on')))) {
  if (Date.now() - t0 > 120000) { say('FAIL: the controls never showed'); process.exit(1); }
  await new Promise(r => setTimeout(r, 500));
}
say(`controls shown after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
const centre = id => page.evaluate(i => { const r = document.getElementById(i).getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; }, id);
const cdp = await ctx.newCDPSession(page);
const touch = (type, points) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: points.map(([x, y, id]) => ({ x, y, id })) });
await new Promise(r => setTimeout(r, 3000));   // the start lights
const [gx, gy] = await centre('gas');
const [sx, sy] = await page.evaluate(() => { const r = document.getElementById('steer').getBoundingClientRect(); return [r.left + r.width * 0.5, r.top + r.height * 0.6]; });
say(`gas at ${gx | 0},${gy | 0}; stick at ${sx | 0},${sy | 0}`);
await touch('touchStart', [[gx, gy, 1]]);
say('gas down');
await new Promise(r => setTimeout(r, 4000));
await touch('touchStart', [[gx, gy, 1], [sx, sy, 2]]);
for (let k = 1; k <= 10; k++) { await touch('touchMove', [[gx, gy, 1], [sx + k * 12, sy, 2]]); await new Promise(r => setTimeout(r, 50)); }
say('stick right');
await new Promise(r => setTimeout(r, 2500));
await touch('touchEnd', [[gx, gy, 1]]);       // the stick's finger stays
await touch('touchEnd', []);
say('fingers up');
await new Promise(r => setTimeout(r, 2000));
const [px, py] = await centre('pause');
await touch('touchStart', [[px, py, 3]]); await new Promise(r => setTimeout(r, 200)); await touch('touchEnd', []);
say('pause tapped');
await new Promise(r => setTimeout(r, 2500));
say('controls shown in the pause menu: ' + await page.evaluate(() => !!document.querySelector('#touch.on')));
await new Promise(r => setTimeout(r, SECONDS * 1000 - 14000 > 0 ? SECONDS * 1000 - 14000 : 0));
await browser.close();
srv.kill();
process.exit(0);
