// The phone's on-screen keyboard for the game's text fields (player name, login, IP, chat).
// A touch tap on a text field of the top menu layer focuses a hidden <input>, which brings the keyboard up; what the
// keyboard types reaches the game as keys (input.ts textInput). A tap anywhere else, Enter, or the field leaving the
// screen puts the keyboard away. Mouse and physical keyboard users never see any of it.
//
// Phone keyboards rarely send usable key events: they edit the field (with word suggestions, composition) and fire
// "input". So the field's value is compared with the last one and the difference is sent: Backspaces for what went,
// characters for what came. The value starts with SENTINEL so a Backspace on an "empty" field still changes the value.
//
// The text fields are found as KB1's focus research found them (docs/research/KB1-menu-focus.md on main): the
// frontend 0065B928, its layer vector +0xD8, the top layer's window, its children vector +0x10C, panels +0x14C,
// the position block +0xE0 (+0x38/+0x3C), the rectangle +0xF8..+0x104, hidden/disabled flags 0x30 at +0x10C.
import { textInput } from './input';

const INPUT_VTABLES = new Set([0x005b60ac /* FEInputBox */, 0x005b59e4 /* character-vector input */, 0x005b5ac8 /* chat input */]);
const PANEL_VTABLES = new Set([0x005b6358, 0x005b6600]);
const SENTINEL = '  ';

interface Rect { x: number; y: number; w: number; h: number; vt?: number; cmd?: string }

/** The visible text fields of the top frontend layer, in 640x480 frontend coordinates; [] when anything looks off. */
/** all = every visible child with its vtable and button command (tests) */
export function readTextFields(buffer: ArrayBufferLike, all = false): Rect[] {
  const m = new DataView(buffer);
  const ok = (p: number, n: number) => p >= 0x10000 && p <= m.byteLength - n;
  const u = (p: number) => ok(p, 4) ? m.getUint32(p, true) : 0;
  const i = (p: number) => ok(p, 4) ? m.getInt32(p, true) : 0;
  const fe = u(0x0065b928), layers = u(fe + 0xd8);
  if (!ok(fe, 0xdc) || !ok(layers, 8)) return [];
  const first = u(layers), last = u(layers + 4);
  if (last <= first || last - first > 32 || (last - first) % 4) return [];
  const layer = u(last - 4), win = u(layer);
  if (!ok(layer, 8) || u(layer + 4) || !ok(win, 0x110)) return [];
  const out: Rect[] = [], seen = new Set<number>();
  const walk = (owner: number, px: number, py: number, depth: number) => {
    if (depth > 8 || seen.has(owner) || !ok(owner, 0x110)) return;
    seen.add(owner);
    const vec = u(owner + 0x10c), begin = u(vec), end = u(vec + 4), origin = u(owner + 0xe0);
    if (!ok(vec, 8) || end < begin || end - begin > 4096 || (end - begin) % 4 || !ok(origin, 0x40)) return;
    const ox = px + i(origin + 0x38), oy = py + i(origin + 0x3c);
    for (let p = begin; p < end; p += 4) {
      const obj = u(p);
      if (!ok(obj, 0x114)) continue;
      const vt = u(obj), pos = u(obj + 0xe0);
      if (u(obj + 0x10c) & 0x30 || !ok(pos, 0x40)) continue;
      if (PANEL_VTABLES.has(vt) && ok(obj, 0x150)) { walk(u(obj + 0x14c), ox + i(pos + 0x38), oy + i(pos + 0x3c), depth + 1); continue; }
      if (!all && !INPUT_VTABLES.has(vt)) continue;
      const x = ox + i(pos + 0x38) + i(obj + 0xf8), y = oy + i(pos + 0x3c) + i(obj + 0xfc);
      const w = i(obj + 0x100) - i(obj + 0xf8), h = i(obj + 0x104) - i(obj + 0xfc);
      if (!(w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= 640 && y + h <= 480)) continue;
      if (!all) { out.push({ x, y, w, h }); continue; }
      let cmd = '';
      const at = u(obj + 0x148), a = u(at + 0x3c), b = u(at + 0x40);
      if (ok(obj, 0x14c) && ok(at, 0x44) && b >= a && b - a <= 64 && ok(a, b - a)) for (let k = a; k < b; k++) cmd += String.fromCharCode(m.getUint8(k));
      out.push({ x, y, w, h, vt, cmd });
    }
  };
  walk(win, 0, 0, 0);
  return out;
}

export function attachSoftKeyboard(memory: WebAssembly.Memory, canvas: HTMLElement) {
  const field = document.createElement('input');
  field.type = 'text';
  field.setAttribute('autocomplete', 'off'); field.setAttribute('autocorrect', 'off');
  field.setAttribute('autocapitalize', 'off'); field.setAttribute('spellcheck', 'false');
  field.setAttribute('aria-hidden', 'true'); field.tabIndex = -1;
  // on the screen (a field outside it may make the page scroll), invisible, 16px so the phone does not zoom in
  Object.assign(field.style, { position: 'fixed', left: '0', top: '0', width: '1px', height: '1px', opacity: '0', border: '0', padding: '0', fontSize: '16px', pointerEvents: 'none' });
  document.body.appendChild(field);
  // tests (web/tools/softkb_test.mjs): the fields the page sees
  (globalThis as unknown as Record<string, unknown>).nfs5TextFields = (all = false) => readTextFields(memory.buffer, all);

  let last = SENTINEL, composing = false, open = false, watch = 0;
  const reset = () => { field.value = SENTINEL; last = SENTINEL; try { field.setSelectionRange(SENTINEL.length, SENTINEL.length); } catch { /* not focused */ } };
  const close = () => { if (!open) return; open = false; clearInterval(watch); field.blur(); canvas.focus({ preventScroll: true }); };
  const show = () => {
    reset();
    field.focus({ preventScroll: true });
    if (open) return;
    open = true;
    // the field went away (another page, a dialog over it): the keyboard goes too
    watch = window.setInterval(() => { if (!readTextFields(memory.buffer).length) close(); }, 400);
  };
  field.addEventListener('blur', () => { if (open) { open = false; clearInterval(watch); } });
  field.addEventListener('compositionstart', () => { composing = true; });
  field.addEventListener('compositionend', () => { composing = false; });
  field.addEventListener('input', () => {
    const v = field.value;
    let p = 0;
    while (p < v.length && p < last.length && v[p] === last[p]) p++;
    for (let k = last.length - p; k > 0; k--) textInput.key('Backspace');
    for (const c of v.slice(p)) textInput.char(c);
    last = v;
    if (!composing && (!v.startsWith(SENTINEL) || v.length > 64)) reset();
  });
  field.addEventListener('keydown', e => {
    // keys the window's handler does not know (code '' on phone keyboards); Enter also closes the keyboard
    if (e.key === 'Enter') { if (!e.code) textInput.key('Enter'); e.preventDefault(); close(); }
    else if (e.key === 'Backspace' && !e.code && field.value.length <= SENTINEL.length) { textInput.key('Backspace'); e.preventDefault(); }
  });

  // only touch taps; the tap's click itself still reaches the game (input.ts), so the field gets its caret as with a mouse
  canvas.addEventListener('pointerup', e => {
    if (e.pointerType !== 'touch') return;
    const r = canvas.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width * 640, y = (e.clientY - r.top) / r.height * 480;
    const hit = readTextFields(memory.buffer).some(f => x >= f.x - 4 && x < f.x + f.w + 4 && y >= f.y - 4 && y < f.y + f.h + 4);
    if (hit) show(); else close();
  });
}
