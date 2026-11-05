// nfs5-web WEB3: the page. Starts the game process (host/process.ts) and attaches WebGPU, input and sound to it.
// URL parameters for the tests: log=1 (the game's log to the server's stdout, POST /test-log), env=K=V,K=V, save=<OPFS
// folder>, shots=<frame>,<frame> (POST /test-shot/<frame>: raw RGBA of the frame).
import { startProcess } from './host/process';
import { HostKind } from './host/abi';
import { MANIFEST_URL, DATA_BASE } from './host/datacache';
import { attachRenderer } from './gpu/renderer';
import { attachInput } from './input';

const q = new URLSearchParams(location.search);
const statusEl = document.getElementById('status')!;
const canvas = document.getElementById('game') as HTMLCanvasElement;
const setStatus = (s: string) => { statusEl.textContent = s; };

let pendingLog: string[] = [];
const toServer = q.get('log') === '1';
if (toServer) setInterval(() => {
  if (!pendingLog.length) return;
  const body = pendingLog.join('\n') + '\n'; pendingLog = [];
  void fetch('/test-log', { method: 'POST', body });
}, 500);
const log = (s: string) => { if (toServer) pendingLog.push(s); else console.log(s); };

function attachAudio(memory: WebAssembly.Memory, ptr: number) {
  const ac = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  void ac.audioWorklet.addModule('/audio/worklet.js').then(() => {
    const node = new AudioWorkletNode(ac, 'nfs5-ring', { numberOfInputs: 0, outputChannelCount: [2] });
    node.port.postMessage({ sab: memory.buffer, ptr });
    node.connect(ac.destination);
  });
  // browsers start sound on the first click or key
  const resume = () => { if (ac.state !== 'running') void ac.resume(); };
  window.addEventListener('mousedown', resume);
  window.addEventListener('keydown', resume);
}

async function main() {
  if (!crossOriginIsolated) throw new Error('the page is not cross-origin isolated (COOP/COEP headers)');
  const info = await (await fetch('/nfs5.json', { cache: 'no-cache' })).json();
  setStatus('Loading the game module...');
  const module = await WebAssembly.compileStreaming(fetch('/nfs5.wasm', { cache: 'no-cache' }));
  const env: Record<string, string> = { NFS5_SAVEDIR: '/save' };
  // widescreen: the race runs 960 lines high and as wide as the window (4:3 .. 32:9); ?ws=0 keeps 4:3. The canvas holds the
  // widest mode, the renderer gives the element the shape of the mode in use (the menus stay 4:3).
  const aspect = q.get('ws') === '0' ? 4 / 3 : Math.min(32 / 9, Math.max(4 / 3, innerWidth / Math.max(1, innerHeight)));
  const raceW = Math.round(960 * aspect / 8) * 8;
  env.NFS5_RACE_W = String(raceW);
  canvas.width = raceW; canvas.height = 960;
  for (const kv of (q.get('env') ?? '').split(',')) { const i = kv.indexOf('='); if (i > 0) env[kv.slice(0, i)] = kv.slice(i + 1); }
  // tests: seed=<name>,<name> puts save files of the native test folder into this browser's save folder
  for (const name of (q.get('seed') ?? '').split(',').filter(Boolean)) {
    const dir = await (await (await navigator.storage.getDirectory()).getDirectoryHandle(q.get('save') ?? 'save', { create: true })).getDirectoryHandle('SaveData', { create: true });
    const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
    await w.write(await (await fetch('/test-seed/' + name)).arrayBuffer());
    await w.close();
  }
  setStatus('Starting the game...');
  const p = await startProcess({
    module,
    cfg: { args: ['nfs5', '/data/PORSCHE.EXE'], env, dataBase: DATA_BASE, manifestUrl: MANIFEST_URL, saveDir: q.get('save') ?? 'save' },
    initialPages: info.pages,
    onLog: (s, _fd, tid) => log(`[${tid}] ${s}`),
    onNote: s => log(s),
    onExit: code => {
      log(`[exit] ${code}`);
      // a page cannot close its own tab
      document.getElementById('stage')!.remove();
      statusEl.className = 'ended';
      statusEl.innerHTML = code === 0 ? 'The game has ended. <a href="">Play again</a>' : `The game stopped with an error (${code}). <a href="">Start again</a>`;
    },
  });
  const g = globalThis as unknown as Record<string, unknown>;
  g.game = p;
  void p.registered(HostKind.Input).then(r => attachInput(p.memory, r.ptr, canvas));
  void p.registered(HostKind.Audio).then(r => attachAudio(p.memory, r.ptr));
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
    last = stats.frames;
  }, 5000); }
}
main().catch(e => { const m = e instanceof Error ? (e.stack ?? e.message) : String(e); log('[page error] ' + m); setStatus('Error: ' + m); });
