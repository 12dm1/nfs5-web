// nfs5-web WEB3: the page. Starts the game process (host/process.ts); its log goes to the console and, with ?log=1, to
// the server's stdout (POST /test-log) for the headless tests. URL parameters: args=<game arguments>, env=K=V,K=V, log=1.
import { startProcess } from './host/process';
import { MANIFEST_URL, DATA_BASE } from './host/datacache';

const q = new URLSearchParams(location.search);
const statusEl = document.getElementById('status')!;
const setStatus = (s: string) => { statusEl.textContent = s; };

let pendingLog: string[] = [];
const toServer = q.get('log') === '1';
if (toServer) setInterval(() => {
  if (!pendingLog.length) return;
  const body = pendingLog.join('\n') + '\n'; pendingLog = [];
  void fetch('/test-log', { method: 'POST', body });
}, 500);
const log = (s: string) => { if (toServer) pendingLog.push(s); else console.log(s); };

async function main() {
  if (!crossOriginIsolated) throw new Error('the page is not cross-origin isolated (COOP/COEP headers)');
  const info = await (await fetch('/nfs5.json', { cache: 'no-cache' })).json();
  setStatus('Loading the game module...');
  const module = await WebAssembly.compileStreaming(fetch('/nfs5.wasm', { cache: 'no-cache' }));
  const env: Record<string, string> = { NFS5_SAVEDIR: '/save' };
  for (const kv of (q.get('env') ?? '').split(',')) { const i = kv.indexOf('='); if (i > 0) env[kv.slice(0, i)] = kv.slice(i + 1); }
  if (q.get('args')) env.NFS5_ARGS = q.get('args')!;
  setStatus('Starting the game...');
  const p = await startProcess({
    module,
    cfg: { args: ['nfs5', '/data/PORSCHE.EXE', '/data/PORSCHE.EXE'], env, dataBase: DATA_BASE, manifestUrl: MANIFEST_URL, saveDir: q.get('save') ?? 'save' },
    initialPages: info.pages,
    onLog: (s, _fd, tid) => log(`[${tid}] ${s}`),
    onNote: s => log(s),
    onExit: code => { log(`[exit] ${code}`); setStatus(`The game ended (${code}).`); },
  });
  setStatus('');
  (globalThis as unknown as { game: unknown }).game = p;
}
main().catch(e => { const m = e instanceof Error ? (e.stack ?? e.message) : String(e); log('[page error] ' + m); setStatus('Error: ' + m); });
