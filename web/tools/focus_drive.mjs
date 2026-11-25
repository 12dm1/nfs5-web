// KB2: an interactive driver for one game in headless Chrome, for exploring the menus' focus by hand from a shell.
//   node web/tools/focus_drive.mjs [--port 8760] [--ctl 8761] [--env A=1,B=2] [--seed f1,f2] [--url-extra '&x=1'] [--tag D]
// then GET http://127.0.0.1:<ctl>/<verb>?...:
//   focus                 the focus layer's snapshot (window.nfs5Focus) with the controls' class names
//   tree                  every object of every frontend layer + the bottom bar + the race's menu pages, with class names
//   key?k=ArrowDown&n=1   a key press (Playwright keyboard; k=Shift+Tab works)
//   pad?b=0&ms=150        a pad button held for ms (b: 0 A, 1 B, 9 Start, 12..15 d-pad); pad?ax=0&v=-1&ms=300 a stick axis
//   cmd?c=QUICKRACE       click the button with this command (mouse, at its centre)
//   click?x=320&y=240     a mouse click at 640x480 coordinates; drag?x1=&y1=&x2=&y2=&steps=1 a drag
//   eval?js=...           the page evaluates it, the result as JSON
//   log?n=40&grep=re      the game's last log lines
//   reload, quit
//   --players 2: two games through the Internet mode into one race (NETTEST online-host / online-join), g=1 for the second
// The class names come from the RTTI of out/PORSCHE.EXE (read at start, never stored).
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url)), ROOT = path.resolve(HERE, '../..');
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8760), CTL = +arg('ctl', 8761), TAG = arg('tag', 'D');

/** vtable -> class name from the EXE's RTTI (MSVC: vtable[-1] = complete object locator, +12 = type descriptor). */
export function rttiClasses(file) {
  const exe = fs.readFileSync(file);
  const pe = exe.readUInt32LE(0x3c), nsec = exe.readUInt16LE(pe + 6), opt = exe.readUInt16LE(pe + 20), base = exe.readUInt32LE(pe + 52);
  const secs = [];
  for (let i = 0; i < nsec; i++) {
    const o = pe + 24 + opt + i * 40;
    secs.push({ name: exe.toString('latin1', o, o + 8).replace(/\0.*$/, ''), va: base + exe.readUInt32LE(o + 12), rs: exe.readUInt32LE(o + 16), ro: exe.readUInt32LE(o + 20) });
  }
  const off = a => { for (const s of secs) if (a >= s.va && a + 4 <= s.va + s.rs) return s.ro + a - s.va; return -1; };
  const cstr = a => { const o = off(a); if (o < 0) return null; const e = exe.indexOf(0, o); return e > o && e - o < 256 ? exe.toString('latin1', o, e) : null; };
  const cols = new Map(), out = new Map();
  for (const s of secs.filter(s => s.name === '.rdata' || s.name === '.data')) {
    for (let o = 0; o + 20 <= s.rs; o += 4) {
      const p = s.ro + o;
      if (exe.readUInt32LE(p) !== 0) continue;
      const td = exe.readUInt32LE(p + 12), chd = exe.readUInt32LE(p + 16);
      if (!td || !chd) continue;
      const n = cstr(td + 8);
      if (n && n.startsWith('.?AV')) cols.set(s.va + o, n.slice(4).split('@@')[0]);
    }
  }
  for (const s of secs.filter(s => s.name === '.rdata' || s.name === '.data')) {
    for (let o = 0; o + 8 <= s.rs; o += 4) {
      const v = exe.readUInt32LE(s.ro + o), name = cols.get(v);
      if (!name) continue;
      const f0 = exe.readUInt32LE(s.ro + o + 4);
      if (f0 >= 0x401000 && f0 < 0x5b0000) out.set(s.va + o + 4, name);
    }
  }
  return out;
}

/** Runs in the page: a raw dump of the frontend's layers, the bottom bar and the race's menu pages. */
export const TREE_JS = `(() => {
  const m = new DataView(game.memory.buffer);
  const ok = (p, n) => p >= 0x10000 && p <= m.byteLength - n;
  const u = p => ok(p, 4) ? m.getUint32(p, true) : 0, i = p => ok(p, 4) ? m.getInt32(p, true) : 0;
  const text = (a, b) => { let s = ''; if (b >= a && b - a <= 64 && ok(a, b - a)) for (let k = a; k < b; k++) s += String.fromCharCode(m.getUint8(k)); return s; };
  const walk = (owner, px, py, depth, out, seen) => {
    if (depth > 8 || seen.has(owner) || !ok(owner, 0x110)) return;
    seen.add(owner);
    const vec = u(owner + 0x10c), begin = u(vec), end = u(vec + 4), origin = u(owner + 0xe0);
    if (!ok(vec, 8) || end < begin || end - begin > 4096 || (end - begin) % 4 || !ok(origin, 0x40)) return;
    const ox = px + i(origin + 0x38), oy = py + i(origin + 0x3c);
    for (let p = begin; p < end; p += 4) {
      const obj = u(p);
      if (!ok(obj, 0x114)) continue;
      const vt = u(obj), pos = u(obj + 0xe0), flags = u(obj + 0x10c);
      const x = ok(pos, 0x40) ? ox + i(pos + 0x38) + i(obj + 0xf8) : NaN, y = ok(pos, 0x40) ? oy + i(pos + 0x3c) + i(obj + 0xfc) : NaN;
      const w = i(obj + 0x100) - i(obj + 0xf8), h = i(obj + 0x104) - i(obj + 0xfc);
      const at = u(obj + 0x148);
      const cmd = ok(at, 0x44) ? text(u(at + 0x3c), u(at + 0x40)) : '';
      out.push({ obj, vt, flags, x, y, w, h, cmd, depth });
      if ((vt === 0x5b6358 || vt === 0x5b6600) && ok(obj, 0x150) && !(flags & 0x30)) walk(u(obj + 0x14c), x - i(obj + 0xf8), y - i(obj + 0xfc), depth + 1, out, seen);
    }
  };
  const fe = u(0x65b928), layers = u(fe + 0xd8);
  const res = { fe, layers: [], bar: null, race: null, mode: globalThis.nfs5Focus ? nfs5Focus().mode : -1 };
  if (ok(fe, 0xe0) && ok(layers, 8)) {
    for (let l = u(layers); l < u(layers + 4) && res.layers.length < 8; l += 4) {
      const layer = u(l), win = u(layer), pending = u(layer + 4), out = [];
      walk(win, 0, 0, 0, out, new Set());
      res.layers.push({ layer, win, winVt: u(win), pending, objs: out });
    }
    const bar = u(fe + 0xdc), out = [];
    walk(bar, 0, 0, 0, out, new Set());
    res.bar = { win: bar, winVt: u(bar), objs: out };
  }
  const rv = u(0x60c9c4);
  if (ok(rv, 8)) {
    res.race = { pages: [], hudW: u(0x6c1f00), hudH: u(0x6c1f04), w: u(0x619784), h: u(0x619788), paused: u(0x606a90), options: u(0x606aa4) };
    for (let e = u(rv); e < u(rv + 4) && res.race.pages.length < 16; e += 8) {
      const page = u(e), extra = u(e + 4), buttons = [];
      if (ok(page, 0x200)) for (let o = 4; o < 0x200; o += 4) {
        const b = u(page + o);
        if (ok(b, 0x110) && u(b) === 0x5b2904) buttons.push({ off: o, b, x: i(b + 4), y: i(b + 8), w: i(b + 12), h: i(b + 16), on: m.getUint8(b + 0x103) });
      }
      res.race.pages.push({ page, extra, vt: u(page), buttons });
    }
  }
  return res;
})()`;

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const classes = rttiClasses(path.join(ROOT, 'out/PORSCHE.EXE'));
  const cls = vt => classes.get(vt) ?? vt.toString(16);
  const lines = [];
  const keep = l => { lines.push(l); if (lines.length > 20000) lines.splice(0, 5000); };
  const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT), ...arg('server-args', '').split(' ').filter(Boolean)], { stdio: ['ignore', 'pipe', 'inherit'] });
  srv.stdout.on('data', d => { for (const l of d.toString().split('\n')) if (l.trim()) keep(l); });
  await new Promise(r => setTimeout(r, 1000));
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'] });
  // --players N (ON1 online, as web/tools/online_test.mjs): game 0 hosts, the others join; verbs take g=<n>
  const PLAYERS = +arg('players', 1), env = arg('env', ''), seed = arg('seed', '');
  const pages = [];
  for (let g = 0; g < PLAYERS; g++) {
    const page = await (await browser.newContext({ viewport: { width: 1280, height: 960 } })).newPage();
    const tag = g ? TAG + g : TAG;
    page.on('console', m => keep(`[${tag} console] ` + m.text()));
    page.on('pageerror', e => keep(`[${tag} pageerror] ` + e.message));
    await page.addInitScript(() => {
      const pad = { id: 'fake standard pad', index: 0, connected: true, mapping: 'standard', timestamp: 1, axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
      globalThis.__pad = pad;
      navigator.getGamepads = () => [pad];
      document.hasFocus = () => true;
    });
    const online = PLAYERS > 1 ? [`NETTEST=${g ? 'online-join' : 'online-host'}`, `NETTEST_ACCOUNT=${g ? 'joiner' + g : 'host'}`, `NETTEST_PLAYERS=${PLAYERS}`] : [];
    const e = [...online, ...(env ? env.split(',') : [])].join(',');
    await page.goto(`http://127.0.0.1:${PORT}/?log=1&tag=${tag}${e ? '&env=' + encodeURIComponent(e) : ''}${seed ? '&seed=' + encodeURIComponent(seed) : ''}${arg('url-extra', '')}`);
    pages.push(page);
    if (g + 1 < PLAYERS) await new Promise(r => setTimeout(r, 3000));
  }
  let page = pages[0];
  const describe = o => ({ ...o, cls: cls(o.vt) });
  const verbs = {
    async focus() {
      const f = await page.evaluate('globalThis.nfs5Focus?.()');
      if (!f) return f;
      if (f.page) f.page.items = await Promise.all(f.page.items.map(async it => ({ ...it, cls: cls(await page.evaluate(`new DataView(game.memory.buffer).getUint32(${it.id}, true)`)) })));
      return f;
    },
    async tree() {
      const t = await page.evaluate(TREE_JS);
      for (const l of t.layers) { l.winCls = cls(l.winVt); l.objs = l.objs.map(describe); }
      if (t.bar) { t.bar.winCls = cls(t.bar.winVt); t.bar.objs = t.bar.objs.map(describe); }
      if (t.race) t.race.pages = t.race.pages.map(p => ({ ...p, cls: cls(p.vt) }));
      return t;
    },
    async key(q) { for (let n = 0; n < +(q.get('n') ?? 1); n++) { await page.keyboard.press(q.get('k'), { delay: 60 }); await page.waitForTimeout(+(q.get('gap') ?? 150)); } return 'ok'; },
    async pad(q) {
      const ms = +(q.get('ms') ?? 150), set = v => page.evaluate(([b, ax, v]) => {
        const p = globalThis.__pad; p.timestamp++;
        if (ax !== null) p.axes[+ax] = v; else { p.buttons[+b].pressed = v > 0; p.buttons[+b].value = v; }
      }, [q.get('b'), q.get('ax'), v]);
      for (let n = 0; n < +(q.get('n') ?? 1); n++) { await set(+(q.get('v') ?? 1)); await page.waitForTimeout(ms); await set(0); await page.waitForTimeout(+(q.get('gap') ?? 150)); }
      return 'ok';
    },
    async cmd(q) {
      const c = q.get('c');
      const w = (await page.evaluate('globalThis.nfs5TextFields?.(true) ?? []')).find(x => x.cmd === c)
        ?? (await page.evaluate('globalThis.nfs5Focus?.()?.page?.items ?? []')).filter(x => x.command === c).map(x => ({ x: x.x, y: x.y, w: x.width, h: x.height }))[0];
      if (!w) return 'no ' + c;
      const b = await page.locator('canvas').boundingBox();
      await page.mouse.click(b.x + (w.x + w.w / 2) / 640 * b.width, b.y + (w.y + w.h / 2) / 480 * b.height, { delay: 200 });
      return 'clicked ' + c;
    },
    async click(q) {
      const b = await page.locator('canvas').boundingBox();
      await page.mouse.click(b.x + +q.get('x') / 640 * b.width, b.y + +q.get('y') / 480 * b.height, { delay: 200 });
      return 'ok';
    },
    async drag(q) {
      const b = await page.locator('canvas').boundingBox(), at = (x, y) => [b.x + x / 640 * b.width, b.y + y / 480 * b.height];
      await page.mouse.move(...at(+q.get('x1'), +q.get('y1'))); await page.waitForTimeout(150);
      await page.mouse.down(); await page.waitForTimeout(150);
      await page.mouse.move(...at(+q.get('x2'), +q.get('y2')), { steps: +(q.get('steps') ?? 1) }); await page.waitForTimeout(150);
      await page.mouse.up();
      return 'ok';
    },
    async eval(q) { return await page.evaluate(q.get('js')); },
    async log(q) { const re = new RegExp(q.get('grep') ?? '.'); return lines.filter(l => re.test(l)).slice(-(+(q.get('n') ?? 40))).join('\n'); },
    async quit() { setTimeout(async () => { await browser.close(); srv.kill(); process.exit(0); }, 100); return 'bye'; },
    async reload() { await page.reload(); return 'reloaded'; },
  };
  http.createServer(async (req, res) => {
    const u = new URL(req.url, 'http://x'), v = verbs[u.pathname.slice(1)];
    page = pages[+(u.searchParams.get('g') ?? 0)] ?? pages[0];
    try {
      const r = v ? await v(u.searchParams) : 'verbs: ' + Object.keys(verbs).join(' ');
      res.end(typeof r === 'string' ? r + '\n' : JSON.stringify(r, (k, x) => typeof x === 'number' && k !== 'x' && k !== 'y' && k !== 'w' && k !== 'h' && k !== 'width' && k !== 'height' && x > 0xffff ? '0x' + x.toString(16) : x) + '\n');
    } catch (e) { res.end('error: ' + e.message + '\n'); }
  }).listen(CTL, '127.0.0.1');
  console.log(`focus_drive: game on ${PORT}, control on ${CTL}`);
}
