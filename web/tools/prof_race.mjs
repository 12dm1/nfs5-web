// PERF2: a sampling CPU profile of the game's workers (the game thread is lifted x86 in wasm; the name section keeps the
// function names) during a race, from DevTools' Profiler domain over a raw CDP connection (Playwright gives no worker sessions).
//   node web/tools/prof_race.mjs [--port 8180] [--url "/?log=1&env=..."] [--warm 40] [--seconds 15] [--interval 200]
//        [--top 40] [--width 1280 --height 960] [--out out/prof.json] [--log out/prof.log] [--steps "..."]
// --warm: seconds before the profile starts (load + race start); --seconds: the profile's length; --interval: microseconds.
// Prints each worker's busy share and, for the busiest, the functions by self time and by total time (recursion counted once).
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PORT = +arg('port', 8180), DPORT = PORT + 1000, URLPATH = arg('url', '/?log=1&nocloud=1');
const WARM = +arg('warm', 40), SECONDS = +arg('seconds', 15), INTERVAL = +arg('interval', 200), TOP = +arg('top', 40);
const logFile = arg('log', '') ? fs.createWriteStream(arg('log', '')) : null;
const GREP = new RegExp(arg('grep', 'TRAP|pageerror|\\[perf game\\]|\\[fps\\]|\\[cube\\]'));
const t0 = Date.now();
const NOPROF = process.argv.includes('--noprof'), JSFLAGS = arg('jsflags', '');
const onLine = (line) => { logFile?.write(line + '\n'); if (GREP.test(line)) console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}] ${line.slice(0, 220)}`); };

const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT)], { stdio: ['ignore', 'pipe', 'pipe'] });
let buf = '';
const feed = (d) => { buf += d.toString(); let i; while ((i = buf.indexOf('\n')) >= 0) { onLine(buf.slice(0, i).replace(/\r$/, '')); buf = buf.slice(i + 1); } };
srv.stdout.on('data', feed); srv.stderr.on('data', feed);
await new Promise(r => setTimeout(r, 1000));

const browser = await chromium.launch({
  executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required',
         `--remote-debugging-port=${DPORT}`, ...(JSFLAGS ? [`--js-flags=${JSFLAGS}`] : [])],
});
const page = await (await browser.newContext({ viewport: { width: +arg('width', 1280), height: +arg('height', 960) } })).newPage();
page.on('pageerror', e => onLine('[pageerror] ' + e.message));

// ---- raw CDP over the browser's WebSocket, flat sessions ----
let ws, nextId = 1;
const pending = new Map(), handlers = [];
async function cdpOpen() {
  const v = await (await fetch(`http://127.0.0.1:${DPORT}/json/version`)).json();
  ws = new WebSocket(v.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); }
    else for (const h of handlers) h(m);
  };
}
const send = (method, params = {}, sessionId) => new Promise((res, rej) => {
  const id = nextId++; pending.set(id, { res, rej });
  ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
});

try {
  await page.goto('about:blank');
  await cdpOpen();
  // every worker of the page, nested ones too (the game's thread pool is spawned from the game worker), held at its start
  // until its profiler runs: a profile started after the wasm was compiled shows no wasm frames
  const sessions = new Map();
  handlers.push((m) => {
    if (m.method !== 'Target.attachedToTarget') return;
    const { sessionId, targetInfo } = m.params;
    sessions.set(sessionId, targetInfo);
    void (async () => {
      if (process.argv.includes('--debugger') && targetInfo.type === 'worker') await send('Debugger.enable', {}, sessionId).catch(() => {});
      if (!NOPROF && (targetInfo.type === 'worker' || targetInfo.type === 'shared_worker')) {
        await send('Profiler.enable', {}, sessionId).catch(() => {});
        await send('Profiler.setSamplingInterval', { interval: INTERVAL }, sessionId).catch(() => {});
        await send('Profiler.start', {}, sessionId).catch(() => {});
      }
      await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, sessionId).catch(() => {});
      await send('Runtime.runIfWaitingForDebugger', {}, sessionId).catch(() => {});
    })();
  });
  const { targetInfos } = await send('Target.getTargets');
  for (const t of targetInfos.filter(t => t.type === 'page' && t.url === 'about:blank')) {
    const { sessionId } = await send('Target.attachToTarget', { targetId: t.targetId, flatten: true });
    await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, sessionId);
  }
  await page.goto(`http://127.0.0.1:${PORT}${URLPATH}`);
  const tStart = Date.now();
  await page.waitForTimeout((WARM + SECONDS) * 1000);
  const workers = [...sessions].filter(([, t]) => t.type === 'worker' || t.type === 'shared_worker');
  console.log(`[prof] ${workers.length} workers`);
  if (NOPROF) throw new Error('noprof: done');
  const profiles = await Promise.all(workers.map(([s, t]) => send('Profiler.stop', {}, s).then(r => ({ t, p: r.profile })).catch(() => null)));
  // a thread's waits: sleeps, the futex waits of the host (wasm-to-js under them), the idle loop
  const WAITS = new Set(['(idle)', 'sleepNs', 'poll_oneoff', 'clock_nanosleep', 'nanosleep', 'wasm-to-js', 'monoNs', 'Atomics.wait', 'wait', 'r_pageflip', '__wasilibc_futex_wait', 'onmessage', 'clock_time_get', 'writeUsAsNs']);
  const report = [];
  for (const x of profiles.filter(Boolean)) {
    const { nodes, samples, timeDeltas, endTime } = x.p;
    const from = endTime - SECONDS * 1e6;            // the last SECONDS of the profile
    let tt = x.p.startTime;
    const byId = new Map(nodes.map(n => [n.id, n]));
    const parent = new Map();
    for (const n of nodes) for (const c of n.children || []) parent.set(c, n.id);
    const selfMs = new Map(), totalMs = new Map();
    let all = 0, idle = 0;
    for (let i = 0; i < samples.length; i++) {
      tt += timeDeltas[i] || 0;
      if (tt < from) continue;
      const dt = (timeDeltas[i + 1] ?? timeDeltas[i] ?? 0) / 1000;
      const n = byId.get(samples[i]);
      const name = n.callFrame.functionName || '(anon)';
      all += dt;
      if (WAITS.has(name)) idle += dt;
      selfMs.set(name, (selfMs.get(name) || 0) + dt);
      const seen = new Set();
      for (let id = samples[i]; id !== undefined; id = parent.get(id)) {
        const nm = byId.get(id).callFrame.functionName || '(anon)';
        if (seen.has(nm)) continue; seen.add(nm);
        totalMs.set(nm, (totalMs.get(nm) || 0) + dt);
      }
    }
    // --focus <fn>: within that function's subtree, the leaves' self time and its direct callees' total time
    const FOCUS = arg('focus', ''), focusSelf = new Map(), focusKids = new Map();
    if (FOCUS) {
      let t2 = x.p.startTime;
      for (let i = 0; i < samples.length; i++) {
        t2 += timeDeltas[i] || 0;
        if (t2 < from) continue;
        const dt = (timeDeltas[i + 1] ?? timeDeltas[i] ?? 0) / 1000;
        const chain = [];
        for (let id = samples[i]; id !== undefined; id = parent.get(id)) chain.push(byId.get(id).callFrame.functionName || '(anon)');
        const k = chain.lastIndexOf(FOCUS);   // the outermost call of it
        if (k < 0) continue;
        // --callers <fn>: who calls that function inside the focus (its caller's name, by the time under it)
        const CAL = arg('callers', '');
        if (CAL) { const j = chain.indexOf(CAL); if (j >= 0 && j + 1 < chain.length) { const nm = 'caller of ' + CAL + ': ' + chain[j + 1] + ' < ' + (chain[j + 2] || ''); focusKids.set(nm, (focusKids.get(nm) || 0) + dt); } }
        focusSelf.set(chain[0], (focusSelf.get(chain[0]) || 0) + dt);
        const kid = k > 0 ? chain[k - 1] : '(self)';
        focusKids.set(kid, (focusKids.get(kid) || 0) + dt);
      }
    }
    report.push({ url: x.t.url.split('/').pop(), all, busy: all - idle, selfMs, totalMs, focusSelf, focusKids });
  }
  // the game's main thread is the one that presents (--pick: another function to find a thread by)
  const PICK = arg('pick', 'r_pageflip');
  report.sort((a, b) => (b.totalMs.get(PICK) || 0) - (a.totalMs.get(PICK) || 0) || b.busy - a.busy);
  const out = [];
  for (const r of report) out.push(`[prof] worker ${r.url}: busy ${(r.busy / 1000).toFixed(2)} s of ${(r.all / 1000).toFixed(2)} s`);
  for (const r of report.slice(0, +arg('workers', 1))) {
    out.push(`\n[prof] ${r.url} by self time (ms over ${SECONDS} s, % of busy):`);
    for (const [n, ms] of [...r.selfMs].sort((a, b) => b[1] - a[1]).slice(0, TOP)) out.push(`  ${ms.toFixed(0).padStart(7)}  ${(100 * ms / r.busy).toFixed(1).padStart(5)}%  ${n}`);
    out.push(`\n[prof] ${r.url} by total time:`);
    for (const [n, ms] of [...r.totalMs].sort((a, b) => b[1] - a[1]).slice(0, TOP)) out.push(`  ${ms.toFixed(0).padStart(7)}  ${(100 * ms / r.busy).toFixed(1).padStart(5)}%  ${n}`);
  }
  if (arg('focus', '') && report[0]) {
    const r = report[0], F = arg('focus', '');
    const tot = [...r.focusKids.values()].reduce((x, y) => x + y, 0);
    out.push(`\n[prof] inside ${F} (${tot.toFixed(0)} ms): its direct callees by total time`);
    for (const [n, ms] of [...r.focusKids].sort((a, b) => b[1] - a[1]).slice(0, 25)) out.push(`  ${ms.toFixed(0).padStart(7)}  ${(100 * ms / tot).toFixed(1).padStart(5)}%  ${n}`);
    out.push(`\n[prof] inside ${F}: leaves by self time`);
    for (const [n, ms] of [...r.focusSelf].sort((a, b) => b[1] - a[1]).slice(0, 35)) out.push(`  ${ms.toFixed(0).padStart(7)}  ${(100 * ms / tot).toFixed(1).padStart(5)}%  ${n}`);
  }
  console.log(out.join('\n'));
  if (arg('out', '')) fs.writeFileSync(arg('out', ''), JSON.stringify(report.map(r => ({ url: r.url, busy: r.busy, all: r.all,
    self: Object.fromEntries([...r.selfMs].sort((a, b) => b[1] - a[1]).slice(0, 300)), total: Object.fromEntries([...r.totalMs].sort((a, b) => b[1] - a[1]).slice(0, 300)) }))));
} catch (e) { console.log('[prof] error ' + (e.stack || e)); }
ws?.close();
await browser.close().catch(() => {});
srv.kill();
process.exit(0);
