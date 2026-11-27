// Live look tuning (LAMP2 / HEAD1 / WHEEL1): the renderer reads these values every frame (renderer.ts tuneF / bloomF).
// ?tune=1 shows a panel over the game: sliders, F8 hides it, values kept in this browser, "Copy" puts them on the clipboard as
// JSON (send that back to make them the defaults). Keyboard focus never stays on a slider: the game keeps its keys.

export interface TuneParam { key: string; label: string; min: number; max: number; step: number; def: number; group: string; }

export const TUNE_PARAMS: TuneParam[] = [
  // LAMP1 bloom + lens streak
  { group: 'Bloom', key: 'bloom', label: 'overall', min: 0, max: 4, step: 0.05, def: 0.45 },
  { group: 'Bloom', key: 'gain', label: 'halo', min: 0, max: 6, step: 0.05, def: 0.1 },
  { group: 'Bloom', key: 'wide', label: 'wide halo', min: 0, max: 3, step: 0.05, def: 0.65 },
  { group: 'Bloom', key: 'streak', label: 'streak', min: 0, max: 3, step: 0.05, def: 1.9 },
  { group: 'Bloom', key: 'sthr', label: 'streak threshold', min: 0, max: 0.6, step: 0.01, def: 0.12 },
  { group: 'Bloom', key: 'slen', label: 'streak length', min: 0.5, max: 8, step: 0.1, def: 1.4 },
  { group: 'Bloom', key: 'sun', label: 'sun + flares', min: 0, max: 2, step: 0.05, def: 0.3 },
  // HEAD1 projected headlights
  { group: 'Headlights', key: 'hl', label: 'intensity', min: 0, max: 6, step: 0.05, def: 2.45 },
  { group: 'Headlights', key: 'hlforce', label: 'force (day too)', min: 0, max: 1, step: 0.05, def: 0.1 },
  { group: 'Headlights', key: 'hlrange', label: 'range m', min: 5, max: 120, step: 1, def: 32 },
  { group: 'Headlights', key: 'hlwidth', label: 'spread', min: 0.05, max: 1.2, step: 0.01, def: 0.79 },
  { group: 'Headlights', key: 'hlvert', label: 'vertical spread', min: 0.005, max: 0.2, step: 0.001, def: 0.136 },
  { group: 'Headlights', key: 'hlcut', label: 'cutoff (down -)', min: -0.08, max: 0.04, step: 0.002, def: -0.006 },
  { group: 'Headlights', key: 'hlkick', label: 'kerb step-up', min: 0, max: 0.6, step: 0.01, def: 0.07 },
  { group: 'Headlights', key: 'hlsym', label: 'symmetry (0 right side, 1 both)', min: 0, max: 1, step: 0.05, def: 0.8 },
  { group: 'Headlights', key: 'hlsoft', label: 'cutoff softness', min: 0.001, max: 0.05, step: 0.001, def: 0.022 },
  { group: 'Headlights', key: 'hlhot', label: 'hot spot', min: 0, max: 4, step: 0.05, def: 0.4 },
  { group: 'Headlights', key: 'hlwrap', label: 'grazing light', min: 0, max: 1, step: 0.01, def: 0.32 },
  { group: 'Headlights', key: 'hlfringe', label: 'lens fringe', min: 0, max: 2, step: 0.05, def: 1.35 },
  { group: 'Headlights', key: 'hlwarm', label: 'warm (-) / xenon (+)', min: -1, max: 1, step: 0.05, def: -1 },
  { group: 'Headlights', key: 'hlh', label: 'lamp height m', min: 0, max: 1.2, step: 0.01, def: 0.63 },
  { group: 'Headlights', key: 'hlfwd', label: 'lamp ahead of axle m', min: -0.5, max: 2, step: 0.05, def: 0.4 },
  { group: 'Headlights', key: 'hlsep', label: 'lamp spacing x', min: 0, max: 1.2, step: 0.02, def: 0.74 },
  // WHEEL1
  { group: 'Wheels', key: 'wheel', label: 'rim blur', min: 0, max: 4, step: 0.05, def: 1.75 },
  { group: 'Wheels', key: 'wheeldbg', label: 'debug: turn as red', min: 0, max: 1, step: 1, def: 0 },
];

const STORE = 'nfs5.tune.v4';   // v4: the user's third tuning became the defaults
export const tune: Record<string, number> = {};
for (const p of TUNE_PARAMS) tune[p.key] = p.def;

// URL overrides (?bloom=, ?wheelblur=, any ?t_<key>=) and the browser's saved values
export function loadTune() {
  const q = new URLSearchParams(location.search);
  try { const s = JSON.parse(localStorage.getItem(STORE) ?? '{}'); for (const k of Object.keys(s)) if (k in tune && typeof s[k] === 'number') tune[k] = s[k]; } catch { /* none */ }
  const b = Number(q.get('bloom') ?? NaN); if (b > 0) tune.bloom = b;
  const w = Number(q.get('wheelblur') ?? NaN); if (w >= 0) tune.wheel = w;
  for (const p of TUNE_PARAMS) { const v = Number(q.get('t_' + p.key) ?? NaN); if (!Number.isNaN(v)) tune[p.key] = v; }
}

function save() { try { localStorage.setItem(STORE, JSON.stringify(tune)); } catch { /* private window */ } }

export function mountTunePanel() {
  const css = `
#tunep{position:fixed;top:8px;right:8px;z-index:9999;width:300px;max-height:calc(100vh - 16px);overflow:auto;background:rgba(14,16,20,.86);
 color:#e8e8e8;font:12px/1.3 system-ui,sans-serif;border:1px solid #444;border-radius:8px;padding:8px 10px;user-select:none}
#tunep h4{margin:8px 0 4px;font-size:12px;color:#9cf;text-transform:uppercase;letter-spacing:.05em}
#tunep .r{display:grid;grid-template-columns:118px 1fr 44px;gap:6px;align-items:center;margin:2px 0}
#tunep input[type=range]{width:100%}
#tunep .v{text-align:right;font-variant-numeric:tabular-nums;color:#fc6}
#tunep .b{display:flex;gap:6px;margin-top:8px}
#tunep button{flex:1;background:#2a2f38;color:#eee;border:1px solid #555;border-radius:5px;padding:4px;cursor:pointer}
#tunep .hint{color:#888;margin-top:6px}`;
  const st = document.createElement('style'); st.textContent = css; document.head.appendChild(st);
  const box = document.createElement('div'); box.id = 'tunep';
  let group = '';
  const vals = new Map<string, { inp: HTMLInputElement; out: HTMLElement }>();
  for (const p of TUNE_PARAMS) {
    if (p.group !== group) { group = p.group; const h = document.createElement('h4'); h.textContent = group; box.appendChild(h); }
    const r = document.createElement('div'); r.className = 'r';
    const l = document.createElement('span'); l.textContent = p.label;
    const inp = document.createElement('input'); inp.type = 'range'; inp.min = String(p.min); inp.max = String(p.max); inp.step = String(p.step);
    inp.value = String(tune[p.key]); inp.tabIndex = -1;
    const out = document.createElement('span'); out.className = 'v'; out.textContent = fmt(tune[p.key]);
    inp.addEventListener('input', () => { tune[p.key] = Number(inp.value); out.textContent = fmt(tune[p.key]); save(); });
    // the game's keys stay the game's: a slider never keeps the focus, arrow keys never move it
    inp.addEventListener('pointerup', () => inp.blur());
    inp.addEventListener('keydown', e => e.preventDefault());
    out.addEventListener('dblclick', () => { tune[p.key] = p.def; inp.value = String(p.def); out.textContent = fmt(p.def); save(); });
    r.append(l, inp, out); box.appendChild(r);
    vals.set(p.key, { inp, out });
  }
  const bar = document.createElement('div'); bar.className = 'b';
  const bCopy = document.createElement('button'); bCopy.textContent = 'Copy';
  bCopy.onclick = () => { const t = JSON.stringify(tune); void navigator.clipboard?.writeText(t).catch(() => {}); console.log('[tune] ' + t); bCopy.textContent = 'Copied'; setTimeout(() => { bCopy.textContent = 'Copy'; }, 1200); bCopy.blur(); };
  const bReset = document.createElement('button'); bReset.textContent = 'Defaults';
  bReset.onclick = () => { for (const p of TUNE_PARAMS) { tune[p.key] = p.def; const v = vals.get(p.key)!; v.inp.value = String(p.def); v.out.textContent = fmt(p.def); } save(); bReset.blur(); };
  bar.append(bCopy, bReset); box.appendChild(bar);
  const hint = document.createElement('div'); hint.className = 'hint'; hint.textContent = 'F8 hide/show · double-click a value: its default'; box.appendChild(hint);
  document.body.appendChild(box);
  addEventListener('keydown', e => { if (e.key === 'F8') { box.style.display = box.style.display === 'none' ? '' : 'none'; e.preventDefault(); } }, true);
}

function fmt(v: number) { return Math.abs(v) >= 10 ? v.toFixed(0) : v.toFixed(Math.abs(v) < 0.1 && v !== 0 ? 3 : 2); }
