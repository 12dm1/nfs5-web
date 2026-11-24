// nfs5-web WEB3: the page. Starts the game process (host/process.ts) and attaches WebGPU, input and sound to it.
// URL parameters for the tests: log=1 (the game's log to the server's stdout, POST /test-log), env=K=V,K=V, save=<OPFS
// folder>, seed=<file>,<file>, shots=<frame>,<frame> (POST /test-shot/<frame>: raw RGBA of the frame), tag=<name>,
// lan=<wss://...>, nocloud=1. They are taken only from a development server (it answers /test-dev.json): on the public
// site a link must not change a player's game or saves (CLOUD1 review, after nfsu2-web's audit PAGE-1).
// CLOUD1: a signed-in player's saves are brought from the cloud before the game starts and sent after every save
// (cloud/sync.ts).
import { startProcess } from './host/process';
import { HostKind } from './host/abi';
import { MANIFEST_URL, DATA_BASE } from './host/datacache';
import { attachRenderer, raceMode } from './gpu/renderer';
import { attachInput } from './input';
import { attachLan } from './net/lan';
import { attachTouch } from './touch';
import { attachSoftKeyboard } from './softkb';
import { startCloud, type CloudSync } from './cloud/sync';
import { gfx, gfxEnv, setGfxForTest } from './quality';

const dev = await fetch('/test-dev.json', { cache: 'no-store' }).then(r => r.ok ? r.json() : null).then(j => j?.dev === true, () => false);
const q = dev ? new URLSearchParams(location.search) : new URLSearchParams();
setGfxForTest(q.get('gfx'));
const statusEl = document.getElementById('status')!;
const canvas = document.getElementById('game') as HTMLCanvasElement;
const setStatus = (s: string) => { statusEl.textContent = s; };
const toastEl = document.getElementById('toast')!;
let toastTimer: ReturnType<typeof setTimeout> | undefined;
/** a line for the player over the game, for 9 s */
const toast = (s: string) => {
  toastEl.textContent = s; toastEl.classList.add('on');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => toastEl.classList.remove('on'), 9000);
};

let pendingLog: string[] = [];
const toServer = q.get('log') === '1';
if (toServer) setInterval(() => {
  if (!pendingLog.length) return;
  const body = pendingLog.join('\n') + '\n'; pendingLog = [];
  void fetch('/test-log', { method: 'POST', body });
}, 500);
// tag=<name>: every line starts with [name] (several games on one test server: web/tools/online_test.mjs)
const tag = q.get('tag') ? `[${q.get('tag')}] ` : '';
const NL = String.fromCharCode(10);
const log = (s: string) => {
  if (!toServer) { console.log(s); return; }
  pendingLog.push(tag ? s.split(NL).filter(l => l.length).map(l => tag + l).join(NL) : s);
};

function attachAudio(memory: WebAssembly.Memory, ptr: number, stat: boolean) {
  // latencyHint 0: the smallest output buffer the device allows ('interactive' may pick a larger one)
  const ac = new AudioContext({ sampleRate: 48000, latencyHint: 0 });
  void ac.audioWorklet.addModule('/audio/worklet.js').then(() => {
    const node = new AudioWorkletNode(ac, 'nfs5-ring', { numberOfInputs: 0, outputChannelCount: [2] });
    node.port.postMessage({ sab: memory.buffer, ptr, stat });
    // the worklet reads play cursors on the game's clock (each worker's time origin + now, game-worker.ts): the
    // context time of now, sent again every second
    const clock = () => node.port.postMessage({ hostUs: (performance.timeOrigin + performance.now()) * 1000, ctx: ac.currentTime });
    clock(); setInterval(clock, 1000);
    node.port.onmessage = (e: MessageEvent<{ stat?: string }>) => {
      if (e.data.stat) log(`[audiostat] worklet: ${e.data.stat}; output: base ${(ac.baseLatency * 1000).toFixed(1)} ms, device ${((ac.outputLatency ?? 0) * 1000).toFixed(1)} ms, ${ac.state}`);
    };
    node.connect(ac.destination);
  });
  // browsers start sound on the first click, key or touch
  const resume = () => { if (ac.state !== 'running') void ac.resume(); };
  for (const ev of ['mousedown', 'keydown', 'pointerdown', 'touchend']) window.addEventListener(ev, resume);
}

async function main() {
  if (!crossOriginIsolated) throw new Error('the page is not cross-origin isolated (COOP/COEP headers)');
  const info = await (await fetch('/nfs5.json', { cache: 'no-cache' })).json();
  setStatus('Loading the game module...');
  const module = await WebAssembly.compileStreaming(fetch('/nfs5.wasm', { cache: 'no-cache' }));
  const env: Record<string, string> = { NFS5_SAVEDIR: '/save', ...gfxEnv() };
  log(`[gfx] profile ${gfx()}`);
  // the race's mode comes from the window when a race starts (renderer.ts raceMode); the canvas starts with it
  [canvas.width, canvas.height] = raceMode();
  for (const kv of (q.get('env') ?? '').split(',')) { const i = kv.indexOf('='); if (i > 0) env[kv.slice(0, i)] = kv.slice(i + 1); }
  // tests: seed=<name>,<name> puts save files of the native test folder into this browser's save folder
  for (const name of (q.get('seed') ?? '').split(',').filter(Boolean)) {
    const dir = await (await (await navigator.storage.getDirectory()).getDirectoryHandle(q.get('save') ?? 'save', { create: true })).getDirectoryHandle('SaveData', { create: true });
    const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
    await w.write(await (await fetch('/test-seed/' + name)).arrayBuffer());
    await w.close();
  }
  const saveDir = q.get('save') ?? 'save';
  let cloud: CloudSync | null = null;
  if (q.get('nocloud') !== '1') {
    setStatus('Cloud saves...');
    cloud = await startCloud({ saveDir, note: s => log(s), tell: s => { log('[cloud] ' + s); toast(s); } });
    if (cloud) addEventListener('pagehide', () => void cloud!.flush());
  }
  setStatus('Starting the game...');
  const p = await startProcess({
    module,
    cfg: { args: ['nfs5', '/data/PORSCHE.EXE'], env, dataBase: DATA_BASE, manifestUrl: MANIFEST_URL, saveDir },
    initialPages: info.pages,
    onLog: (s, _fd, tid) => log(`[${tid}] ${s}`),
    onNote: s => log(s),
    // the game wrote or removed a save: to the cloud a few seconds later (paths are the save folder's, any case)
    onSaved: path => { if (/^savedata\//i.test(path)) cloud?.changed(); },
    onRemoved: path => { if (/^savedata\//i.test(path)) cloud?.changed(); },
    onExit: code => {
      log(`[exit] ${code}`);
      void cloud?.flush();
      // a page cannot close its own tab
      document.getElementById('stage')!.remove();
      statusEl.className = 'ended';
      statusEl.innerHTML = code === 0 ? 'The game has ended. <a href="">Play again</a>' : `The game stopped with an error (${code}). <a href="">Start again</a>`;
    },
  });
  const g = globalThis as unknown as Record<string, unknown>;
  g.game = p;
  void p.registered(HostKind.Input).then(r => { attachInput(p.memory, r.ptr, canvas); attachTouch(); attachSoftKeyboard(p.memory, canvas); });
  void p.registered(HostKind.Audio).then(r => attachAudio(p.memory, r.ptr, !!env.AUDIOSTAT && env.AUDIOSTAT !== '0'));
  // the game's Winsock (runtime/web/winsock_web.c) registers its mailbox at WSAStartup
  void p.registered(HostKind.Lan).then(r => { g.lan = attachLan(p.memory, r.ptr, log, q.get('lan')); });
  const shots = (q.get('shots') ?? '').split(',').filter(Boolean).map(Number);
  const r = await p.registered(HostKind.Gpu);
  const stats = await attachRenderer(p.memory, r.ptr, canvas, log, shots);
  g.stats = stats;
  setStatus('');
  setInterval(() => {
    for (const [frame, px] of stats.shots) { stats.shots.delete(frame); void fetch('/test-shot/' + frame, { method: 'POST', body: px }); log(`[shot] frame ${frame}`); }
  }, 200);
  if (toServer) { let last = 0; setInterval(() => {
    log(`[fps] ${(stats.frames - last) / 5} frames/s, frame ${stats.frames}, last segment ${stats.lastDraws} draws ${stats.lastVerts} verts, ${stats.textures} textures`);
    if (stats.frames > last) log(`[perf page] js per frame ${(stats.jsSum / (stats.frames - last)).toFixed(2)} ms (max ${stats.jsMax.toFixed(1)}), longest submit ${stats.submitMax.toFixed(1)} ms, longest gap between presents ${stats.gapMax.toFixed(1)} ms, gaps over 25 ms ${stats.slow}`);
    stats.jsSum = 0; stats.jsMax = 0; stats.gapMax = 0; stats.slow = 0; stats.submitMax = 0;
    last = stats.frames;
  }, 5000); }
}
main().catch(e => { const m = e instanceof Error ? (e.stack ?? e.message) : String(e); log('[page error] ' + m); setStatus('Error: ' + m); });
