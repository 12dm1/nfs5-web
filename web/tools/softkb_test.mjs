// The on-screen keyboard path (web/src/softkb.ts) in headless Chrome with touch: a fresh profile reaches the player
// name field, a touch tap on it focuses the hidden input, text arrives the way a phone keyboard sends it ("input"
// events, no key codes), a Backspace by value change, Enter by a code-less key. Then the save folder is listed.
//   node web/tools/softkb_test.mjs [--port 8097] [--server-args "..."]
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8097);
const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT), ...arg('server-args', '').split(' ').filter(Boolean)], { stdio: ['ignore', 'ignore', 'inherit'] });
await new Promise(r => setTimeout(r, 1000));
const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio'] });
const ctx = await browser.newContext({ viewport: { width: 1280, height: 960 }, hasTouch: true });
const page = await ctx.newPage();
page.on('console', m => { const t = m.text(); if (/SaveData|TRAP|MESSAGEBOX|page error/.test(t)) console.log('[game] ' + t.slice(0, 160)); });
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
const fail = async (s) => { console.log('FAIL: ' + s); await browser.close(); srv.kill(); process.exit(1); };
await page.goto(`http://127.0.0.1:${PORT}/?log=0`);
const box0 = async () => page.locator('canvas').boundingBox();
const widgets = () => page.evaluate(() => globalThis.nfs5TextFields?.(true) ?? []);
let fields = [], createdAt = -1e9;
for (let t = 0; t < 120 && !fields.length; t++) {
  await page.waitForTimeout(1000);
  fields = await page.evaluate(() => globalThis.nfs5TextFields?.() ?? []);
  if (fields.length || t - createdAt < 5) continue;
  // the profile page: its Create button (a mouse click, as a person would) opens the name field
  const w = await widgets(), c = w.find(x => /CREATE/i.test(x.cmd ?? ''));
  if (c) { const b = await box0(); await page.mouse.click(b.x + (c.x + c.w / 2) / 640 * b.width, b.y + (c.y + c.h / 2) / 480 * b.height, { delay: 300 }); createdAt = t; console.log('clicked ' + c.cmd + ' at ' + t + ' s'); }
}
if (!fields.length) await fail('no text field on screen in 90 s');
console.log('text fields: ' + JSON.stringify(fields));
const box = await page.locator('canvas').boundingBox(), f = fields[0];
const tap = (gx, gy) => page.touchscreen.tap(box.x + gx / 640 * box.width, box.y + gy / 480 * box.height);
await tap(f.x + f.w / 2, f.y + f.h / 2);
// iOS Safari: emulated mouse events follow the tap even though pointerdown was cancelled; they must not take the focus
await page.evaluate(([x, y]) => { const cv = document.querySelector('canvas');
  for (const t of ['mousemove', 'mousedown', 'mouseup', 'click']) cv.dispatchEvent(new MouseEvent(t, { bubbles: true, cancelable: true, clientX: x, clientY: y, button: 0 })); },
  [box.x + (f.x + f.w / 2) / 640 * box.width, box.y + (f.y + f.h / 2) / 480 * box.height]);
await page.waitForTimeout(500);
const focused = await page.evaluate(() => document.activeElement?.tagName + ':' + (document.activeElement?.getAttribute('aria-hidden') ?? ''));
console.log('focused after tap: ' + focused);
if (focused !== 'INPUT:true') await fail('the tap did not focus the hidden input');
for (const c of 'Bobz') { await page.keyboard.insertText(c); await page.waitForTimeout(120); }
// a phone keyboard's Backspace: the value loses its last character, no usable key code
await page.evaluate(() => { const el = document.activeElement; el.value = el.value.slice(0, -1); el.dispatchEvent(new InputEvent('input', { inputType: 'deleteContentBackward' })); });
await page.waitForTimeout(300);
await page.evaluate(() => document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: '', bubbles: true })));
await page.waitForTimeout(500);
console.log('focused after Enter: ' + await page.evaluate(() => document.activeElement?.tagName));
// a tap away from any field must not open it
await tap(20, 20); await page.waitForTimeout(300);
console.log('focused after a tap elsewhere: ' + await page.evaluate(() => document.activeElement?.tagName));
// the page's own OK (CREATEPLAYERENTER) takes the name, as a tap would
{ const w = await widgets(); console.log('name page commands: ' + JSON.stringify(w.filter(x => x.cmd).map(x => x.cmd)));
  const c = w.find(x => /^(OK|CREATEPLAYER|CREATEPLAYEROK|ACCEPT)/i.test(x.cmd) && !/CANCEL/i.test(x.cmd));
  if (c) { await tap(c.x + c.w / 2, c.y + c.h / 2); console.log('tapped ' + c.cmd); } }
await page.waitForTimeout(6000);
const files = await page.evaluate(async () => {
  const out = []; const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('save');
  const sd = await root.getDirectoryHandle('SaveData').catch(() => null);
  if (sd) for await (const [n, h] of sd.entries()) out.push(n + (h.kind === 'file' ? ':' + (await (await h.getFile()).text()).replace(/[^\x20-\x7e]/g, '.').slice(0, 60) : ''));
  return out;
});
console.log('SaveData: ' + JSON.stringify(files));
await browser.close(); srv.kill();
