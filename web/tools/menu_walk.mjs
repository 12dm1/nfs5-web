// Walks the game's menus in headless Chrome by button command (the page's nfs5TextFields(true) list, web/src/softkb.ts):
//   node web/tools/menu_walk.mjs --steps "CREATEPLAYERENTER,type:Bob,CREATEPLAYERDONE,..." [--seconds 60] [--port 8100]
//   [--server-args "..."] [--env A=1,B=2] [--set 0x... =v]
// A step is a command name (clicked by mouse at its button's centre, waited for up to 30 s), "type:<text>", "wait:<s>",
// "eval:<js>" (in the page, result printed) or "dump" (the page's commands). After the steps it waits --seconds and prints
// the game's log lines that match --grep.
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8100), steps = arg('steps', '').split(',').filter(Boolean), GREP = new RegExp(arg('grep', 'TRAP|MESSAGEBOX|page error|\\[cars'));
const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT), ...arg('server-args', '').split(' ').filter(Boolean)], { stdio: ['ignore', 'ignore', 'inherit'] });
await new Promise(r => setTimeout(r, 1000));
const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio'] });
const page = await (await browser.newContext({ viewport: { width: 1280, height: 960 } })).newPage();
page.on('console', m => { const t = m.text(); if (GREP.test(t)) console.log('[game] ' + t.slice(0, 220)); });
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
await page.goto(`http://127.0.0.1:${PORT}/?log=0${arg('env', '') ? '&env=' + arg('env', '') : ''}`);
const widgets = () => page.evaluate(() => globalThis.nfs5TextFields?.(true) ?? []);
const cmds = async () => (await widgets()).filter(w => w.cmd).map(w => w.cmd);
for (const s of steps) {
  if (s === 'dump') { console.log('commands: ' + JSON.stringify(await cmds())); continue; }
  if (s.startsWith('wait:')) { await page.waitForTimeout(+s.slice(5) * 1000); continue; }
  if (s.startsWith('type:')) { await page.keyboard.type(s.slice(5), { delay: 120 }); continue; }
  if (s.startsWith('eval:')) { console.log('eval: ' + JSON.stringify(await page.evaluate(s.slice(5)))); continue; }
  if (s === 'dumpall') { console.log('widgets: ' + JSON.stringify((await widgets()).filter(x => x.w > 12 && x.h > 8).map(x => [x.vt.toString(16), x.x, x.y, x.w, x.h, x.cmd]))); continue; }
  // near:x:y = the widget (at least 20x8) whose centre is nearest to a layout point (buttons without a command)
  const near = s.startsWith('near:') ? s.slice(5).split(':').map(Number) : null;
  const pick = (list) => near
    ? list.filter(x => x.w >= 20 && x.h >= 8).sort((a, b) => Math.hypot(a.x + a.w / 2 - near[0], a.y + a.h / 2 - near[1]) - Math.hypot(b.x + b.w / 2 - near[0], b.y + b.h / 2 - near[1]))[0]
    : list.find(x => x.cmd === s);
  let w = null;
  for (let t = 0; t < 60 && !w; t++) { w = pick(await widgets()); if (!w) await page.waitForTimeout(500); }
  if (!w) { console.log(`step ${s}: not on the page; commands: ${JSON.stringify(await cmds())}`); break; }
  // a click the page is not ready for does nothing: click again until the page's widgets change (at most 6 times)
  const before = JSON.stringify(await widgets());
  for (let k = 0; k < 6; k++) {
    const b = await page.locator('canvas').boundingBox();
    await page.mouse.click(b.x + (w.x + w.w / 2) / 640 * b.width, b.y + (w.y + w.h / 2) / 480 * b.height, { delay: 300 });
    let changed = false;
    for (let t = 0; t < 25 && !changed; t++) { await page.waitForTimeout(400); changed = JSON.stringify(await widgets()) !== before; }
    if (changed) break;
  }
  console.log('clicked ' + s);
  await page.waitForTimeout(800);
}
console.log('commands now: ' + JSON.stringify(await cmds()));
await page.waitForTimeout(+arg('seconds', 5) * 1000);
await browser.close(); srv.kill();
