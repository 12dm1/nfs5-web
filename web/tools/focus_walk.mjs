// KB2: walks the frontend's menus with the keyboard only, through the focus layer (web/src/input.ts), and checks every page:
//   - every focus item is reachable from the first one with the arrow keys (the same geometry as moveFocus);
//   - the page has a way back (a GOBACK-like item, a dialog's negative button) unless it is the main menu;
//   - open combos and list boxes reach their last row with ArrowDown and close with Esc, the choice untouched;
//   - controls the focus layer does not offer (a census of the visible classes that are not focus items).
// It goes into a page by moving the focus there with the arrows (the path the geometry gives) and Enter, and back with Esc;
// when Esc does not lead back, the game is reloaded and the path replayed from the main menu.
//   node web/tools/focus_walk.mjs [--port 8770] [--depth 3] [--seed f1,f2] [--out out/focus_walk.json] [--start CMD,CMD]
//     [--deny REGEX] [--max-pages 80] [--no-rows]
// The seeded profile (player.lst etc. from the native save folder) skips the new-player screens. Exit code 1 when a page
// fails a check; the JSON has every page.
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { rttiClasses, TREE_JS } from './focus_drive.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url)), ROOT = path.resolve(HERE, '../..');
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8770), DEPTH = +arg('depth', 3), MAX_PAGES = +arg('max-pages', 80), ROWS = !process.argv.includes('--no-rows');
const OUT = path.resolve(ROOT, arg('out', 'out/focus_walk.json'));
const SEED = arg('seed', 'player.lst,5îïðïðî.sav,îïðïðî1.hud,îïðïðî2.hud');
const START = arg('start', '').split(/\s*>\s*/).filter(Boolean);     // as printed: 'a > b'
// never pressed: they leave the menus, start a race, change or delete the profile, spend money
const DENY = new RegExp(arg('deny', '^(QUIT|PLAYGAME|REMOVEPLAYER|CREATEPLAYERDONE|SETNAMETHENBACK|CREATEPLAYERENTER)$|BUY|SELL|DELETE|RESET|FORFEIT|PURCHASE|REPAIR|STARTRACE|RACENOW|ACCEPT|SAVE'));
const BACK = new Set(['GOBACK', 'GOBACKMINUSONE', 'CANCELCREATEPLAYER', 'SETNAMETHENBACK']);

const fp = await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/focus-page.ts')], bundle: true, format: 'esm', platform: 'node', write: false });
const { moveFocus } = await import('data:text/javascript;base64,' + Buffer.from(fp.outputFiles[0].contents).toString('base64'));
const classes = rttiClasses(path.join(ROOT, 'out/PORSCHE.EXE'));
const cls = vt => classes.get(vt) ?? vt.toString(16);
// classes that draw or decorate only (a census of the rest goes in the report)
const PASSIVE = /^(FEGouraudBoxControl|FEGlintyLineControl|FEGlintyBoxControl|FEPlayerPicture|FEAnimatingPicture|FEBeveledOptionsBoxes|FEShowcasePictureControl|FEControl|FEText|FEHeaderText|FEPicture|FEMovieCircle|FEAccountDisplayControl|FEMenu|FEGouraudIndicatorBar|FE3DCar|FEShape|FELine|FEBox|FEStaticText|FETextBox|FEScrollingText|FEImage|FEAnimatedPicture|FEBitmap)$/;

const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT)], { stdio: ['ignore', 'ignore', 'inherit'] });
await new Promise(r => setTimeout(r, 1000));
const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio'] });
const page = await (await browser.newContext({ viewport: { width: 1280, height: 960 } })).newPage();
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
const sleep = ms => page.waitForTimeout(ms);
const focus = () => page.evaluate('globalThis.nfs5Focus?.() ?? null');
const press = async (k, n = 1) => { for (let i = 0; i < n; i++) { await page.keyboard.press(k, { delay: 50 }); await sleep(90); } };

/** a page's identity: its items' kinds, commands and places (window pointers are reused) */
const sigOf = p => p ? p.items.map(it => `${it.kind}:${it.command ?? ''}:${Math.round(it.x)},${Math.round(it.y)}`).sort().join('|') : 'none';
const label = it => it.command || `${it.kind}@${Math.round(it.x)},${Math.round(it.y)}`;
/** waits until the focus page settles (the same signature twice, 600 ms apart), at most ms */
const settled = async (ms = 8000) => {
  const t0 = Date.now();
  let last = null, lastSig = '';
  while (Date.now() - t0 < ms) {
    const f = await focus();
    const s = f?.mode === 0 ? sigOf(f.page) : `mode${f?.mode}`;
    if (s === lastSig && f?.page?.items.length) return f;
    last = f; lastSig = s;
    await sleep(600);
  }
  return last;
};
/** after a key that may change the page: waits for the page to differ from sig (at most ms), then for it to settle */
const changed = async (sig, ms = 9000) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const f = await focus();
    if (f?.mode !== 0 || sigOf(f?.page) !== sig) {
      // the bottom bar and the page's own controls animate in at their own pace: settle twice
      const a = await settled();
      await sleep(800);
      const b = await settled();
      return sigOf(b?.page) === sigOf(a?.page) ? a : b;
    }
    await sleep(300);
  }
  return await focus();
};
const BAR = new Set(['OPTIONS', 'QUIT', 'GOBACK', 'PLAYGAME']);
/** a page with nothing but the bottom bar is a movie (the career's era intro, the factory driver's): Esc skips it */
const skipMovie = async (f) => {
  for (let k = 0; k < 2 && f?.mode === 0 && f.page && !f.page.items.some(it => !BAR.has(it.command ?? '')); k++) {
    await press('Escape');
    f = await changed(sigOf(f.page));
  }
  return f;
};
/** arrow-key paths from the focused item (or from "no focus": the first arrow press focuses the first item) to every item */
const paths = (items, from = 0) => {
  const p = { window: 0, items }, out = new Map();
  if (!items.length) return out;
  let queue;
  if (from && items.some(it => it.id === from)) { out.set(from, []); queue = [from]; }
  else { const first = moveFocus(p, 0, 'down'); out.set(first.id, ['ArrowDown']); queue = [first.id]; }
  while (queue.length) {
    const id = queue.shift();
    for (const [dir, key] of [['up', 'ArrowUp'], ['down', 'ArrowDown'], ['left', 'ArrowLeft'], ['right', 'ArrowRight']]) {
      const n = moveFocus(p, id, dir);
      if (n && !out.has(n.id)) { out.set(n.id, [...out.get(id), key]); queue.push(n.id); }
    }
  }
  return out;
};
/** puts the focus on an item with the arrows; true when the focus layer says it is there */
const focusOn = async (target) => {
  let f = await focus();
  if (f.focusId === target.id) return true;
  const way = paths(f.page.items, f.focusId).get(target.id);
  if (!way) return false;
  for (const k of way) await press(k);
  f = await focus();
  return f.focusId === target.id;
};
const findItem = (p, step) => p?.items.find(it => label(it) === step) ?? null;

async function boot() {
  await page.goto(`http://127.0.0.1:${PORT}/?log=0&seed=${encodeURIComponent(SEED)}${process.env.WALK_QS || ""}`);
  const isMain = f => f?.page?.items.some(it => it.command === 'SETTYPECAREERMODE');
  for (let t = 0; t < 120; t++) {
    let f = await focus();
    if (isMain(f)) return await settled();
    const pick = f?.page?.items.find(it => it.command === 'SETNAMETHENBACK');
    if (pick) {
      await sleep(1500);
      f = await focus();
      const now = f?.page?.items.find(it => it.command === 'SETNAMETHENBACK');
      if (now && await focusOn(now)) await press('Enter');
      if (isMain(await settled())) return await settled();
    }
    await sleep(1000);
  }
  throw new Error('no main menu');
}
async function replay(route) {
  await boot();
  let f = await settled();
  for (const step of route) {
    let it = findItem(f?.page, step);
    // pages that build in steps (an overlay layer over a page that is still coming up)
    for (let t = 0; t < 8 && !it; t++) { await sleep(1500); f = await settled(3000); it = findItem(f?.page, step); }
    if (!it || !await focusOn(it)) throw new Error('replay: ' + step + ' not found in ' + (f?.page?.items ?? []).map(label).join(' '));
    const before = sigOf(f.page);
    await press('Enter');
    f = await skipMovie(await changed(before));
    // a button still animating in takes no click: once more
    for (let k = 0; k < 2 && sigOf(f?.page) === before; k++) {
      const again = findItem(f?.page, step);
      if (!again || !await focusOn(again)) break;
      await press('Enter');
      f = await skipMovie(await changed(before));
    }
  }
  return f;
}

const report = { pages: [], problems: [] };
const seen = new Set();
const problem = (where, what) => { report.problems.push({ where, what }); console.log(`  PROBLEM ${where}: ${what}`); };

/** visible controls of the top layer that are not focus items, by class */
async function census(ids) {
  const t = await page.evaluate(TREE_JS);
  const top = t.layers[t.layers.length - 1];
  const objs = [...(top?.objs ?? []), ...(t.layers.length === 1 ? t.bar?.objs ?? [] : [])];
  const visible = objs.filter(o => !(o.flags & 0x30) && o.w > 0 && o.h > 0 && o.w < 640 && o.h < 480);
  const out = {};
  for (const o of visible) { const c = cls(o.vt); if (!PASSIVE.test(c) && !ids.has(o.obj)) out[c] = (out[c] ?? 0) + 1; }
  return out;
}

async function checkRows(where, f, it) {
  // a combo opens on Enter; a list box takes the arrows after Enter
  if (!await focusOn(it)) { problem(where, `${label(it)} not reachable by arrows`); return null; }
  await press('Enter');
  // a long list (the merged car list: 87 rows) takes a while to drop down
  let g, open;
  for (let t = 0; t < 12; t++) {
    await sleep(t ? 250 : 700);
    g = await focus();
    open = g.page?.items.find(x => x.id === it.id);
    if (it.kind !== 'combo' || open?.expanded) break;
  }
  if (it.kind === 'combo' && !open?.expanded) { problem(where, `combo ${label(it)} did not open on Enter`); return null; }
  // ArrowDown many times (the stepper queues them), then the hovered row should be the last
  const read = () => page.evaluate(`(() => { const m = new DataView(game.memory.buffer); const o = ${it.id};
    const acc = m.getUint32(o + 0x144, true); return { hover: m.getInt32(o + 0x1a8, true), hovered: m.getUint8(o + 0x1a4),
    count: m.getUint32(acc, true) && m.getUint32(m.getUint32(acc, true) + 0x24, true) === 0x4bc4a0 ? m.getInt32(acc + 0x14, true) : -1,
    selected: m.getUint32(m.getUint32(acc, true) + 0x28, true) === 0x4bc4b0 ? m.getInt32(acc + 0x18, true) : -1 }; })()`);
  let info;
  // 40 presses, then as many more as the list says are left (the merged car list has 87 rows)
  for (let round = 0, n = 40; round < 4 && n > 0; round++) {
    await press('ArrowDown', n);
    for (let t = 0; t < 120; t++) { await sleep(250); g = await focus(); if (!g?.seeking) break; }
    info = await read();
    n = info.count > 0 && info.hovered ? Math.min(120, info.count - 1 - info.hover) : 0;
  }
  await press('Escape');
  await sleep(600);
  g = await focus();
  const after = g.page?.items.find(x => x.id === it.id);
  const res = { item: label(it), kind: it.kind, ...info, closed: it.kind !== 'combo' || !after?.expanded, focusKept: g.focusId === it.id || it.kind === 'list' };
  if (info.count > 0 && (!info.hovered || info.hover !== info.count - 1)) problem(where, `${it.kind} ${label(it)}: last row not reached (hover ${info.hover} of ${info.count})`);
  if (!res.closed) { problem(where, `combo ${label(it)} did not close on Esc`); await press('Escape'); }
  return res;
}

async function visit(route, f, depth) {
  const where = route.length ? route.join(' > ') : 'main menu';
  const sig = sigOf(f.page);
  if (seen.has(sig) || report.pages.length >= MAX_PAGES) return;
  seen.add(sig);
  const items = f.page.items;
  const reach = paths(items);
  const unreachable = items.filter(it => !reach.has(it.id)).map(label);
  // a message with one button (the factory driver's welcome): Esc presses it (input.ts)
  const back = items.find(it => BACK.has(it.command ?? '')) ?? (items.length === 1 && items[0].kind === 'button' ? items[0] : undefined);
  const rec = { route, where, depth, window: f.page.window, items: items.map(it => ({ label: label(it), kind: it.kind })),
    unreachable, back: back ? label(back) : null, census: await census(new Set(items.map(it => it.id))), rows: [], children: [] };
  report.pages.push(rec);
  console.log(`page ${where}: ${items.length} items, back ${rec.back ?? '-'}, not offered ${JSON.stringify(rec.census)}`);
  if (unreachable.length) problem(where, 'unreachable by arrows: ' + unreachable.join(', '));
  if (!back && route.length) problem(where, 'no back item');
  if (ROWS) for (const it of items.filter(x => x.kind === 'combo' || x.kind === 'list')) {
    rec.rows.push(await checkRows(where, f, it));
    f = await settled(3000);
    if (sigOf(f?.page) !== sig) { problem(where, 'the page changed while checking ' + label(it)); f = await replay(route); }
  }
  if (depth >= DEPTH) return;
  const targets = items.filter(it => (it.kind === 'button' || it.kind === 'tab') && !BACK.has(it.command ?? '') && !DENY.test(it.command ?? ''));
  for (const it of targets) {
    const step = label(it);
    let cur = await settled(3000);
    if (sigOf(cur?.page) !== sig) cur = await replay(route);
    const now = findItem(cur.page, step);
    if (!now || !await focusOn(now)) { problem(where, `${step} not reachable by arrows`); continue; }
    await press('Enter');
    const next = await skipMovie(await changed(sig));
    if (next?.mode !== 0) { rec.children.push({ step, result: `mode ${next?.mode}` }); await replay(route); continue; }
    const nsig = sigOf(next.page);
    if (nsig === sig) { rec.children.push({ step, result: 'same page' }); continue; }
    rec.children.push({ step, result: seen.has(nsig) ? 'seen' : 'page' });
    if (!seen.has(nsig)) await visit([...route, step], next, depth + 1);
    // back: Esc (the focus layer clicks the page's back item), else the path again
    await press('Escape');
    let home = await changed(nsig);
    if (sigOf(home?.page) !== sig) home = await changed(sigOf(home?.page), 9000);
    if (sigOf(home?.page) !== sig) {
      // what Esc led to (a slow 3D page, a page whose items changed after the visit below, a dialog)
      rec.children[rec.children.length - 1].back = 'Esc did not lead back: ' + (home?.mode !== 0 ? 'mode ' + home?.mode : (home?.page?.items ?? []).map(label).join(' '));
      await replay(route);
    }
  }
}

try {
  let f = await boot();
  if (START.length) f = await replay(START);
  await visit(START, f, START.length);
} catch (e) {
  problem('walk', e.message);
}
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, JSON.stringify(report, null, 1));
console.log(`focus_walk: ${report.pages.length} pages, ${report.problems.length} problems -> ${OUT}`);
await browser.close();
srv.kill();
process.exit(report.problems.length ? 1 : 0);
