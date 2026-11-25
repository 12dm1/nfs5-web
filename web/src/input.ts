// nfs5-web WEB3: keyboard and mouse into the mailbox of runtime/web/input_web.c.
// Layout (bytes): magic 0, msg_head 4, msg_tail 8, key_head 12, key_tail 16, mx 20, my 24, msgs 28 (256 * 12),
// keys 3100 (256 * 16), dik 7196, vk 7452, then the pad: on 7708, buttons 7712, lx, rt, lt (floats) 7716; mode 7728 (from the
// game: 0 frontend, 1 race menu, 2 driving), rumble seq 7732, low, high (floats) 7736, ms 7744.
import { readFocusPage, readRaceFocusPage, readRows, moveFocus, focusActivationPoint, type FocusItem } from './focus-page';
import { createPadKeyboard, type PadKeyboard } from './padkb';
import { nextFocus, type FocusDirection } from './focus-geometry';

const WM = { KEYDOWN: 0x100, KEYUP: 0x101, CHAR: 0x102, SYSKEYDOWN: 0x104, SYSKEYUP: 0x105, MOVE: 0x200, WHEEL: 0x20A };
// KeyboardEvent.code -> [DirectInput code / scan code (0x80 = extended), Windows virtual key]
const K: Record<string, [number, number]> = {
  Escape: [0x01, 0x1B], Digit1: [0x02, 0x31], Digit2: [0x03, 0x32], Digit3: [0x04, 0x33], Digit4: [0x05, 0x34], Digit5: [0x06, 0x35],
  Digit6: [0x07, 0x36], Digit7: [0x08, 0x37], Digit8: [0x09, 0x38], Digit9: [0x0A, 0x39], Digit0: [0x0B, 0x30], Minus: [0x0C, 0xBD],
  Equal: [0x0D, 0xBB], Backspace: [0x0E, 0x08], Tab: [0x0F, 0x09], KeyQ: [0x10, 0x51], KeyW: [0x11, 0x57], KeyE: [0x12, 0x45],
  KeyR: [0x13, 0x52], KeyT: [0x14, 0x54], KeyY: [0x15, 0x59], KeyU: [0x16, 0x55], KeyI: [0x17, 0x49], KeyO: [0x18, 0x4F], KeyP: [0x19, 0x50],
  BracketLeft: [0x1A, 0xDB], BracketRight: [0x1B, 0xDD], Enter: [0x1C, 0x0D], ControlLeft: [0x1D, 0x11], KeyA: [0x1E, 0x41], KeyS: [0x1F, 0x53],
  KeyD: [0x20, 0x44], KeyF: [0x21, 0x46], KeyG: [0x22, 0x47], KeyH: [0x23, 0x48], KeyJ: [0x24, 0x4A], KeyK: [0x25, 0x4B], KeyL: [0x26, 0x4C],
  Semicolon: [0x27, 0xBA], Quote: [0x28, 0xDE], Backquote: [0x29, 0xC0], ShiftLeft: [0x2A, 0x10], Backslash: [0x2B, 0xDC], KeyZ: [0x2C, 0x5A],
  KeyX: [0x2D, 0x58], KeyC: [0x2E, 0x43], KeyV: [0x2F, 0x56], KeyB: [0x30, 0x42], KeyN: [0x31, 0x4E], KeyM: [0x32, 0x4D], Comma: [0x33, 0xBC],
  Period: [0x34, 0xBE], Slash: [0x35, 0xBF], ShiftRight: [0x36, 0x10], NumpadMultiply: [0x37, 0x6A], AltLeft: [0x38, 0x12], Space: [0x39, 0x20],
  CapsLock: [0x3A, 0x14], F1: [0x3B, 0x70], F2: [0x3C, 0x71], F3: [0x3D, 0x72], F4: [0x3E, 0x73], F5: [0x3F, 0x74], F6: [0x40, 0x75],
  F7: [0x41, 0x76], F8: [0x42, 0x77], F9: [0x43, 0x78], F10: [0x44, 0x79], NumLock: [0x45, 0x90], ScrollLock: [0x46, 0x91], Numpad7: [0x47, 0x67],
  Numpad8: [0x48, 0x68], Numpad9: [0x49, 0x69], NumpadSubtract: [0x4A, 0x6D], Numpad4: [0x4B, 0x64], Numpad5: [0x4C, 0x65], Numpad6: [0x4D, 0x66],
  NumpadAdd: [0x4E, 0x6B], Numpad1: [0x4F, 0x61], Numpad2: [0x50, 0x62], Numpad3: [0x51, 0x63], Numpad0: [0x52, 0x60], NumpadDecimal: [0x53, 0x6E],
  F11: [0x57, 0x7A], F12: [0x58, 0x7B], NumpadEnter: [0x9C, 0x0D], ControlRight: [0x9D, 0x11], NumpadDivide: [0xB5, 0x6F], AltRight: [0xB8, 0x12],
  Home: [0xC7, 0x24], ArrowUp: [0xC8, 0x26], PageUp: [0xC9, 0x21], ArrowLeft: [0xCB, 0x25], ArrowRight: [0xCD, 0x27], End: [0xCF, 0x23],
  ArrowDown: [0xD0, 0x28], PageDown: [0xD1, 0x22], Insert: [0xD2, 0x2D], Delete: [0xD3, 0x2E], Pause: [0xC5, 0x13],
};

/** The on-screen controls of a touch screen (touch.ts) as a second pad, merged with a real one while driving. */
export const touchPad = { active: false, buttons: 0, lx: 0, rt: 0, lt: 0 };
/** What the page's other parts need to know: the game's screen (0 frontend, 1 race menu, 2 driving), when a real pad last moved. */
export const padState = { mode: -1, padAt: -1e9, rumble: (_ms: number) => { } };
/** Text from outside the keyboard events (softkb.ts: a phone's on-screen keyboard): one character or a key by its code. */
export const textInput = { char: (_c: string) => { }, key: (_code: string) => { } };

export function attachInput(memory: WebAssembly.Memory, ptr: number, el: HTMLElement) {
  // KB1 menu focus: on by default, ?focus=0 turns it off.
  const focusEnabled = new URLSearchParams(location.search).get('focus') !== '0';
  const sab = memory.buffer as unknown as SharedArrayBuffer;
  const i32 = new Int32Array(sab, ptr, 4096 / 4 + 2048), u32 = new Uint32Array(sab, ptr, 4096 / 4 + 2048);
  const dik = new Uint8Array(sab, ptr + 7196, 256), vk = new Uint8Array(sab, ptr + 7452, 256);
  let cursorX = 0.5, cursorY = 0.5;
  const activeFocusPage = () => {
    const mode = Atomics.load(i32, 7728 >> 2);
    return mode === 0 ? readFocusPage(memory.buffer) : mode === 1 ? readRaceFocusPage(memory.buffer) : null;
  };
  let seq = 0;
  const msg = (m: number, w: number, l: number) => {
    const tail = Atomics.load(i32, 2);
    if (tail - Atomics.load(i32, 1) >= 256) return;
    const o = (28 + (tail & 255) * 12) >> 2;
    u32[o] = m; u32[o + 1] = w; u32[o + 2] = l;
    Atomics.store(i32, 2, tail + 1);
  };
  const keyEvent = (code: number, down: boolean) => {
    const tail = Atomics.load(i32, 4);
    if (tail - Atomics.load(i32, 3) >= 256) return;
    const o = (3100 + (tail & 255) * 16) >> 2;
    u32[o] = code & 255; u32[o + 1] = down ? 0x80 : 0; u32[o + 2] = (performance.now() | 0) >>> 0; u32[o + 3] = ++seq;
    Atomics.store(i32, 4, tail + 1);
  };
  let focusId = 0, focusWindow = 0, editing = false, clickTimer = 0, consumedEscape = false;
  let popupId = 0;
  let listId = 0;
  let colorId = 0, colorRegion = 0, colorEditing = false, colorX = 0, colorY = 0;
  let sliderId = 0;
  let padkb: PadKeyboard | null = null;
  const outline = document.createElement('div');
  Object.assign(outline.style, { position: 'fixed', pointerEvents: 'none', zIndex: '10', border: '2px solid #fff36a', boxSizing: 'border-box', display: 'none' });
  document.body.appendChild(outline);
  const clearFocus = () => { focusId = 0; focusWindow = 0; editing = false; listId = 0; colorId = 0; colorEditing = false; sliderId = 0; outline.style.display = 'none'; };
  const showFocus = (item: FocusItem) => {
    const r = el.getBoundingClientRect();
    Object.assign(outline.style, { display: 'block', borderColor: colorEditing || sliderId ? '#66ffff' : '#fff36a', left: `${r.left + item.x * r.width / 640}px`, top: `${r.top + item.y * r.height / 480}px`, width: `${item.width * r.width / 640}px`, height: `${item.height * r.height / 480}px` });
  };
  // KB2: buttons of the focus layer's own (FocusItem.label: the network race's leave notice has none of the game's), drawn over
  // the game; a click, a tap or the focus's Enter sends their key
  const own = document.createElement('div');
  Object.assign(own.style, { position: 'fixed', left: '0', top: '0', width: '0', height: '0', zIndex: '9' });
  document.body.appendChild(own);
  let ownState = '';
  const refreshOwn = () => {
    const page = focusEnabled && Atomics.load(i32, 7728 >> 2) === 1 ? readRaceFocusPage(memory.buffer) : null;
    const items = page?.items.filter(x => x.label && x.key) ?? [];
    const r = el.getBoundingClientRect();
    const state = items.length ? items.map(x => `${x.id} ${x.x} ${x.y} ${x.label}`).join('|') + ` @${r.left} ${r.top} ${r.width} ${r.height}` : '';
    if (state === ownState) return;
    ownState = state;
    own.replaceChildren(...items.map(item => {
      const b = document.createElement('button');
      b.textContent = item.label ?? '';
      Object.assign(b.style, { position: 'fixed', left: `${r.left + item.x * r.width / 640}px`, top: `${r.top + item.y * r.height / 480}px`,
        width: `${item.width * r.width / 640}px`, height: `${item.height * r.height / 480}px`, font: `${Math.max(10, Math.round(r.height * 11 / 480))}px sans-serif`,
        color: '#fff', background: 'rgba(0, 0, 0, 0.7)', border: '1px solid rgba(255, 255, 255, 0.6)', padding: '0', cursor: 'pointer', touchAction: 'none' });
      const stop = (e: Event) => { e.stopPropagation(); e.preventDefault(); };
      b.addEventListener('pointerdown', stop);
      b.addEventListener('pointerup', e => { stop(e); if (item.key) sendKey(item.key); });
      b.addEventListener('mousedown', stop);
      b.addEventListener('mouseup', stop);
      return b;
    }));
  };
  const refreshFocus = () => {
    refreshOwn();
    if (focusId) {
      const page = activeFocusPage();
      const item = page?.window === focusWindow ? page.items.find(x => x.id === focusId) : undefined;
      if (item) {
        const rect = item.customEditor && item.id === colorId ? item.customRects?.[colorRegion] : undefined;
        showFocus(rect ? { ...item, ...rect } : item);
      } else clearFocus();
    }
    if (padkb?.open && !focusId) padkb.hide();             // its text field went (the page changed)
    requestAnimationFrame(refreshFocus);
  };
  requestAnimationFrame(refreshFocus);
  // tests (web/tools/focus_drive.mjs, focus_walk.mjs): the focus layer's view of the screen and its own state
  (globalThis as unknown as Record<string, unknown>).nfs5Focus = () => ({
    mode: Atomics.load(i32, 7728 >> 2), page: activeFocusPage(), focusId, focusWindow, popupId, listId, sliderId, editing, colorId, colorEditing,
    seeking: seeking || seekQueued !== 0, padKeyboard: !!padkb?.open,
  });
  const clickFocus = (item: FocusItem, windowId: number) => {
    if (clickTimer) return;
    if (item.key) { sendKey(item.key); return; }          // a button of the page's own
    const page = activeFocusPage(), point = page?.window === windowId ? focusActivationPoint(page, item) : { x: item.x + item.width / 2, y: item.y + item.height / 2 };
    const x = Math.floor(point.x * 65536 / 640), y = Math.floor(point.y * 65536 / 480);
    const p = (x | y << 16) >>> 0;
    cursorX = x / 65536; cursorY = y / 65536;
    Atomics.store(i32, 5, x); Atomics.store(i32, 6, y);
    msg(WM.MOVE, 0, p);
    clickTimer = window.setTimeout(() => {
      const current = activeFocusPage();
      if (current?.window !== windowId || !current.items.some(x => x.id === item.id)) { clickTimer = 0; clearFocus(); return; }
      msg(0x201, 1, p);
      clickTimer = window.setTimeout(() => { msg(0x202, 0, p); clickTimer = 0; }, 120);
    }, 80);
  };
  const moveCursor = (x: number, y: number) => {
    const x16 = Math.floor(x * 65536 / 640), y16 = Math.floor(y * 65536 / 480);
    cursorX = x16 / 65536; cursorY = y16 / 65536;
    Atomics.store(i32, 5, x16); Atomics.store(i32, 6, y16);
    msg(WM.MOVE, 0, (x16 | y16 << 16) >>> 0);
  };
  // FEHorizontalSlider (the chronicle's year): a click on the track beside the thumb jumps there, one on the thumb drags it, so a
  // step is a drag of the thumb by one cell and a half (its width is not known: the cell is guessed from where the thumb is,
  // then the value says whether to drag again)
  let dragging = false;
  const dragSlider = async (item: FocusItem, windowId: number, direction: number) => {
    if (dragging || item.thumbX === undefined || item.sliderCount === undefined || item.sliderValue === undefined) return;
    dragging = true;
    try {
      const want = Math.max(0, Math.min(item.sliderCount - 1, item.sliderValue + direction));
      if (want === item.sliderValue) return;
      const offset = item.thumbX - item.x;
      const thumbWidth = item.sliderValue > 0 ? Math.max(4, item.width - offset * (item.sliderCount - 1) / item.sliderValue) : item.width * 0.155;
      const cell = Math.max(1, (item.width - thumbWidth) / (item.sliderCount - 1));
      const y = item.y + item.height / 2;
      for (let attempt = 0; attempt < 3; attempt++) {
        const now = readFocusPage(memory.buffer)?.items.find(t => t.id === item.id);
        if (!now || now.thumbX === undefined || now.sliderValue === undefined || readFocusPage(memory.buffer)?.window !== windowId) return;
        const left = want - now.sliderValue;
        if (!left) return;
        // the game truncates the dragged distance in cells: half a cell more lands on the cell asked for
        const x0 = now.thumbX + Math.min(4, thumbWidth / 2), x1 = x0 + (left + Math.sign(left) * 0.5) * cell;
        const p0 = (Math.floor(x0 * 65536 / 640) | Math.floor(y * 65536 / 480) << 16) >>> 0;
        const p1 = (Math.floor(x1 * 65536 / 640) | Math.floor(y * 65536 / 480) << 16) >>> 0;
        moveCursor(x0, y); await settle();
        msg(0x201, 1, p0); await settle();
        msg(WM.MOVE, 1, p1); await settle();
        msg(0x202, 0, p1); await settle();
      }
    } finally { dragging = false; }
  };
  const changeSlider = (item: FocusItem, windowId: number, direction: number) => {
    if (item.thumbX !== undefined) { void dragSlider(item, windowId, direction); return; }
    if (clickTimer || item.sliderCount === undefined || item.sliderValue === undefined) return;
    const value = Math.max(0, Math.min(item.sliderCount - 1, item.sliderValue + direction));
    if (value === item.sliderValue) return;
    const x = item.x + (value + 0.5) * item.width / item.sliderCount, y = item.y + item.height / 2;
    const p = (Math.floor(x * 65536 / 640) | Math.floor(y * 65536 / 480) << 16) >>> 0;
    moveCursor(x, y);
    clickTimer = window.setTimeout(() => {
      const page = readFocusPage(memory.buffer);
      if (page?.window !== windowId || !page.items.some(t => t.id === item.id && t.kind === 'slider')) { clickTimer = 0; return; }
      msg(0x201, 1, p);
      clickTimer = window.setTimeout(() => { msg(0x202, 0, p); clickTimer = 0; }, 120);
    }, 80);
  };
  // ---- KB2: the rows of an open combo or of a list box (focus-page.ts readRows). The cursor goes from row to row and the
  // game's own answer (the row under the mouse) decides: rows are 14 px for most lists, other heights or per row for some
  // (the car lists), and a list scrolls only through the arrows of a scroll bar it shows (+0x1DC; a hidden one still has
  // rectangles, and a click there closed a short combo).
  let seeking = false, seekQueued = 0, seekGen = 0;
  const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));
  const gameFrames = () => (globalThis as unknown as { stats?: { frames: number } }).stats?.frames;
  /** until the game took the mailbox's messages and drew two more frames (at most 400 ms) */
  const settle = async () => {
    const tail = Atomics.load(i32, 2), t0 = performance.now();
    while (Atomics.load(i32, 1) - tail < 0 && performance.now() - t0 < 400) await sleep(8);
    const f0 = gameFrames();
    if (f0 === undefined) { await sleep(40); return; }
    while ((gameFrames() ?? f0) - f0 < 2 && performance.now() - t0 < 400) await sleep(8);
  };
  const clickAt = async (x: number, y: number) => {
    moveCursor(x, y);
    await settle();
    const p = (Math.floor(x * 65536 / 640) | Math.floor(y * 65536 / 480) << 16) >>> 0;
    msg(0x201, 1, p);
    await sleep(120);
    msg(0x202, 0, p);
    await settle();
  };
  const rowsOf = (itemId: number, windowId: number) => {
    const page = readFocusPage(memory.buffer);
    const item = page?.window === windowId ? page.items.find(x => x.id === itemId) : undefined;
    const rows = item ? readRows(memory.buffer, item) : null;
    return item && rows ? { item, rows } : null;
  };
  /** dir 0: onto the selected row when it shows, else the first one shown; +1 / -1: the next / previous row */
  const stepRow = async (itemId: number, windowId: number, dir: number, scrolls = true): Promise<void> => {
    let s = rowsOf(itemId, windowId);
    if (!s) return;
    const x = s.item.x + s.item.width * 0.4;
    let r = s.rows;
    const h = r.rowHeight || 14, lo = r.top + 1, hi = r.top + r.height - 1;
    const clampY = (y: number) => Math.max(lo, Math.min(hi, y));
    let y = cursorY * 480;
    const inside = Math.abs(cursorX * 640 - x) < 1 && y >= r.top && y < r.top + r.height;
    if (!dir || !r.hovered || !inside) {
      const shown = r.rowHeight ? Math.floor(r.height / r.rowHeight) : 1;
      const at = r.selected >= r.first && r.selected < r.first + shown ? r.selected - r.first : 0;
      moveCursor(x, clampY(r.top + at * h + h / 2));
      await settle();
      // rows of other heights (the car lists): on from the first one shown to the selected one, as long as it shows
      for (let k = 0; k < 16 && !dir; k++) {
        const now = rowsOf(itemId, windowId)?.rows;
        if (!now?.hovered || now.selected <= now.hoverIndex) break;
        const before = now.hoverIndex;
        await stepRow(itemId, windowId, 1, false);
        if (rowsOf(itemId, windowId)?.rows.hoverIndex === before) break;
      }
      return;
    }
    const target = r.hoverIndex + dir;
    if (target < 0 || (r.count >= 0 && target >= r.count)) return;
    y += dir * h;
    for (let k = 0; k < 12; k++) {
      if (y < lo || y > hi) {
        const arrow = scrolls ? dir > 0 ? r.scrollDown : r.scrollUp : undefined;
        if (!arrow) return;                                  // the end of a list without a scroll bar
        const back = clampY(cursorY * 480);
        await clickAt(arrow.x + arrow.width / 2, arrow.y + arrow.height / 2);
        moveCursor(x, back);
        y = back;
      } else moveCursor(x, y);
      await settle();
      s = rowsOf(itemId, windowId);
      if (!s || !s.rows.hovered) return;
      r = s.rows;
      if (r.hoverIndex === target) return;
      // between rows of other heights: a third of a row more (or back after one too far)
      const step = Math.max(2, Math.round(h / 3));
      y = cursorY * 480 + ((r.hoverIndex - target) * dir < 0 ? dir * step : -dir * step);
    }
  };
  // presses while a step runs add up (at most 10 rows ahead); a held key's repeats only keep one waiting
  const seekRow = (itemId: number, windowId: number, dir: number, repeat = false) => {
    if (seeking) { seekQueued = repeat ? seekQueued || dir : Math.max(-10, Math.min(10, seekQueued + dir)); return; }
    seeking = true;
    const gen = ++seekGen;
    void (async () => {
      try {
        await stepRow(itemId, windowId, dir);
        while (seekQueued && gen === seekGen) { const d = Math.sign(seekQueued); seekQueued -= d; await stepRow(itemId, windowId, d); }
      } finally { seeking = false; seekQueued = 0; }
    })();
  };
  const colorClick = (item: FocusItem, windowId: number, x: number, y: number, drag: boolean) => {
    if (clickTimer) return;
    const p = (Math.floor(x * 65536 / 640) | Math.floor(y * 65536 / 480) << 16) >>> 0;
    moveCursor(x, y);
    clickTimer = window.setTimeout(() => {
      const page = readFocusPage(memory.buffer);
      if (page?.window !== windowId || !page.items.some(t => t.id === item.id && t.customEditor)) { clickTimer = 0; return; }
      msg(0x201, 1, p);
      clickTimer = window.setTimeout(() => {
        if (drag) msg(WM.MOVE, 1, p);
        clickTimer = window.setTimeout(() => { msg(0x202, 0, p); clickTimer = 0; }, 80);
      }, 60);
    }, 60);
  };
  const focusKey = (e: KeyboardEvent, down: boolean): boolean => {
    if (!focusEnabled) return false;
    if (Atomics.load(i32, 7728 >> 2) === 2) { clearFocus(); return false; }
    const page = activeFocusPage();
    if (!page) { clearFocus(); return false; }
    if (focusWindow && focusWindow !== page.window) clearFocus();
    const item = page.items.find(x => x.id === focusId);
    if (focusId && !item) clearFocus();
    // The game's binding control must receive the next physical key itself.
    if (page.items.some(x => x.capturing)) return false;
    // A combo draws its rows in the expanded control and updates its hovered row on mouse move.
    const popup = page.items.find(x => x.expanded);
    if (popup) {
      if (popup.customEditor) {
        const rects = popup.customRects, hsv = popup.customHsv;
        if (!rects || !hsv || hsv.some(v => !Number.isFinite(v) || v < 0 || v > 1)) return false;
        const resetPoint = () => {
          const r = rects[colorRegion];
          colorX = colorRegion === 0 ? r.x + hsv[0] * r.width : colorRegion === 1 ? r.x + hsv[2] * r.width : r.x + r.width / 16;
          colorY = colorRegion === 0 ? r.y + (1 - hsv[1]) * r.height : r.y + r.height / 2;
          colorX = Math.max(r.x + 1, Math.min(r.x + r.width - 1, colorX));
          colorY = Math.max(r.y + 1, Math.min(r.y + r.height - 1, colorY));
        };
        if (colorId !== popup.id) { colorId = popup.id; colorRegion = 0; colorEditing = false; focusId = popup.id; focusWindow = page.window; resetPoint(); }
        if (e.code === 'Escape') {
          if (down && !e.repeat) {
            if (colorEditing) colorEditing = false;
            else { const r = rects[4]; colorClick(popup, page.window, r.x + r.width / 2, r.y + r.height / 2, false); }
          }
          return true;
        }
        if (e.code === 'Enter' || e.code === 'NumpadEnter') {
          if (down && !e.repeat) {
            if (colorRegion < 3) { colorEditing = !colorEditing; if (colorEditing) resetPoint(); }
            else { const r = rects[colorRegion]; colorClick(popup, page.window, r.x + r.width / 2, r.y + r.height / 2, false); }
          }
          return true;
        }
        const dirs: Record<string, FocusDirection> = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
        if (dirs[e.code] || e.code === 'Tab') {
          if (down) {
            if (colorEditing && dirs[e.code] && colorRegion < 3) {
              const r = rects[colorRegion], dx = e.code === 'ArrowRight' ? 1 : e.code === 'ArrowLeft' ? -1 : 0, dy = e.code === 'ArrowDown' ? 1 : e.code === 'ArrowUp' ? -1 : 0;
              if (!clickTimer && (dx || (dy && colorRegion === 0))) {
                colorX = Math.max(r.x + 1, Math.min(r.x + r.width - 1, colorX + dx * Math.max(1, r.width / (colorRegion === 2 ? 8 : 20))));
                colorY = Math.max(r.y + 1, Math.min(r.y + r.height - 1, colorY + dy * Math.max(1, r.height / 20)));
                colorClick(popup, page.window, colorX, colorY, colorRegion !== 2);
              }
            } else {
              colorEditing = false;
              const index = e.code === 'Tab' ? (colorRegion + (e.shiftKey ? rects.length - 1 : 1)) % rects.length : nextFocus(rects, colorRegion, dirs[e.code]);
              if (index >= 0) { colorRegion = index; resetPoint(); }
            }
            showFocus({ ...popup, ...rects[colorRegion] });
          }
          return true;
        }
        return false;
      }
      colorId = 0; colorEditing = false;
      if ((popup.kind !== 'combo' && popup.kind !== 'color') || !popup.popupHeight || popup.popupHeight > 480) return false;
      if (popupId !== popup.id) { popupId = popup.id; seekGen++; }
      const close = () => { seekGen++; seekQueued = 0; clickFocus(popup, page.window); };
      if (e.code === 'Escape') {
        if (down && !e.repeat) close();
        return true;
      }
      if (e.code === 'ArrowDown' || e.code === 'ArrowUp') {
        if (down) seekRow(popup.id, page.window, e.code === 'ArrowDown' ? 1 : -1, e.repeat);
        return true;
      }
      if (e.code === 'Enter' || e.code === 'NumpadEnter') {
        if (down && !e.repeat && !clickTimer && !seeking) {
          // a row under the cursor is picked; without one (nothing chosen yet) Enter closes the list as it was
          const r = readRows(memory.buffer, popup), y = cursorY * 480;
          if (r?.hovered && y >= r.top && y < r.top + r.height) {
            const p = (Math.floor(cursorX * 65536) | Math.floor(cursorY * 65536) << 16) >>> 0;
            msg(0x201, 1, p);
            clickTimer = window.setTimeout(() => { msg(0x202, 0, p); clickTimer = 0; }, 120);
          } else close();
        }
        return true;
      }
      // the other keys would reach the game's own handling of the page under the open list
      return e.code === 'ArrowLeft' || e.code === 'ArrowRight' || e.code === 'Tab';
    }
    popupId = 0;
    if (sliderId && (!item || item.id !== sliderId || item.kind !== 'slider')) sliderId = 0;
    if (sliderId && item) {
      if (e.code === 'ArrowLeft' || e.code === 'ArrowRight') {
        if (down) changeSlider(item, page.window, e.code === 'ArrowLeft' ? -1 : 1);
        return true;
      }
      if (e.code === 'Enter' || e.code === 'NumpadEnter' || e.code === 'Escape') {
        if (down) sliderId = 0;
        return true;
      }
      if (e.code === 'Tab' && down) sliderId = 0;
      else if (e.code !== 'Tab') return false;
    }
    const list = page.items.find(x => x.id === listId && x.kind === 'list');
    if (listId && !list) listId = 0;
    if (list) {
      if (e.code === 'ArrowUp' || e.code === 'ArrowDown') {
        if (down) seekRow(list.id, page.window, e.code === 'ArrowDown' ? 1 : -1, e.repeat);
        return true;
      }
      if (e.code === 'Escape' || e.code === 'Tab') { if (down) { listId = 0; seekGen++; } return true; }
      if (e.code === 'Enter' || e.code === 'NumpadEnter') {
        if (down && !e.repeat && !clickTimer && !seeking) {
          const r = readRows(memory.buffer, list), y = cursorY * 480;
          if (r?.hovered && y >= r.top && y < r.top + r.height) {
            const p = (Math.floor(cursorX * 65536) | Math.floor(cursorY * 65536) << 16) >>> 0;
            msg(0x201, 1, p);
            clickTimer = window.setTimeout(() => { msg(0x202, 0, p); clickTimer = 0; }, 120);
          }
          listId = 0;
        }
        return true;
      }
      return e.code === 'ArrowLeft' || e.code === 'ArrowRight';
    }
    // the pad's B and Start on a page that says which key goes back (the network race's leave notice: Esc there leaves)
    if (e.code === 'Escape' && page.padBack && (e as KeyboardEvent & { pad?: boolean }).pad) {
      if (down && !e.repeat) sendKey(page.padBack);
      return true;
    }
    // Enter (the pad's A) with nothing focused on a race page that Space closes (results, records, standings)
    if ((e.code === 'Enter' || e.code === 'NumpadEnter') && !item && page.spaceCloses) {
      if (down && !e.repeat) sendKey('Space');
      return true;
    }
    if (e.code === 'Escape') {
      if (!down && consumedEscape) { consumedEscape = false; return true; }
      // the profile page has no Back: its "choose" (SETNAMETHENBACK) returns with the highlighted profile, the one in use
      const back = page.items.find(x => x.command === 'GOBACK' || x.command === 'GOBACKMINUSONE' || x.command === 'CANCELCREATEPLAYER')
        ?? page.items.find(x => x.command === 'SETNAMETHENBACK');
      if (down && back) {
        consumedEscape = true;
        if (!e.repeat) { editing = false; clickFocus(back, page.window); }
        return true;
      }
    }
    if (editing) {
      if (e.code === 'Tab' || e.code === 'Escape') { if (down) editing = false; else return true; }
      else return false;
    }
    const direction: Record<string, FocusDirection> = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
    if (direction[e.code] || e.code === 'Tab') {
      if (down) {
        let target: FocusItem | undefined;
        if (e.code === 'Tab') {
          const at = page.items.findIndex(x => x.id === focusId);
          const step = e.shiftKey ? -1 : 1;
          target = page.items[(at < 0 ? (step > 0 ? 0 : page.items.length - 1) : (at + step + page.items.length) % page.items.length)];
        } else target = moveFocus(page, focusId, direction[e.code]);
        if (target) { focusId = target.id; focusWindow = page.window; showFocus(target); }
      }
      return page.items.length > 0;
    }
    if ((e.code === 'Enter' || e.code === 'NumpadEnter') && item) {
      if (down && !e.repeat && !clickTimer) {
        if (item.kind === 'slider') {
          sliderId = item.id; showFocus(item);
        } else if (item.kind === 'list') {
          listId = item.id;
          seekRow(item.id, page.window, 0);
        } else {
          if (!(item.kind === 'input' && item.key === '')) clickFocus(item, page.window);   // key '': a race text entry, no click
          editing = item.kind === 'input';
          if (editing && (e as KeyboardEvent & { pad?: boolean }).pad) padkb?.show();
        }
      }
      return true;
    }
    return false;
  };
  const key = (e: KeyboardEvent, down: boolean) => {
    const k = K[e.code];
    if (!k) return;
    // the browser's own keys stay with the browser
    if (e.ctrlKey && (e.code === 'KeyR' || e.code === 'KeyW' || e.code === 'KeyT' || e.code === 'KeyN' || e.code === 'KeyL')) return;
    e.preventDefault();
    if (padkb?.key(e.code, down)) return;
    if (focusKey(e, down)) {
      // a key the game saw go down (it opened the page the focus layer now has) also goes up there, or its next press is no press
      if (!down && dik[k[0]]) gameKey(e, false);
      return;
    }
    gameKey(e, down);
  };
  /** the key to the game: DirectInput (buffered + state) and the window's messages */
  const gameKey = (e: KeyboardEvent, down: boolean) => {
    const k = K[e.code];
    if (!k) return;
    const [sc, v] = k;
    const ext = (sc & 0x80) !== 0, scan = sc & 0x7F;
    const alt = e.altKey && e.code !== 'AltLeft' && e.code !== 'AltRight';
    const repeat = e.repeat;
    if (!(down && repeat)) keyEvent(sc, down);
    dik[sc] = down ? 0x80 : 0;
    vk[v] = down ? 1 : 0;
    const l = (1 | scan << 16 | (ext ? 1 << 24 : 0) | (alt ? 1 << 29 : 0) | (repeat || !down ? 1 << 30 : 0) | (down ? 0 : 1 << 31)) >>> 0;
    msg(down ? (alt || e.code.startsWith('Alt') ? WM.SYSKEYDOWN : WM.KEYDOWN) : (alt || e.code.startsWith('Alt') ? WM.SYSKEYUP : WM.KEYUP), v, l);
    if (down && e.key.length === 1) msg(WM.CHAR, e.key.charCodeAt(0), l);
    else if (down && (e.code === 'Enter' || e.code === 'NumpadEnter')) msg(WM.CHAR, 13, l);
    else if (down && e.code === 'Backspace') msg(WM.CHAR, 8, l);
    else if (down && e.code === 'Escape') msg(WM.CHAR, 27, l);
    else if (down && e.code === 'Tab') msg(WM.CHAR, 9, l);
  };
  /** a key for the game's own handling, held long enough for a game that polls the keyboard's state once a frame */
  const sendKey = (code: string) => {
    const ev = { code, key: code === 'Space' ? ' ' : '', altKey: false, ctrlKey: false, repeat: false, preventDefault() { } } as unknown as KeyboardEvent;
    gameKey(ev, true);
    window.setTimeout(() => gameKey(ev, false), 150);
  };
  const tap = (code: string, ch: string) => {
    const ev = (down: boolean) => key({ code, key: ch, altKey: false, ctrlKey: false, repeat: false, preventDefault() { } } as unknown as KeyboardEvent, down);
    ev(true); ev(false);
  };
  textInput.key = code => tap(code, '');
  // a letter, digit or space as its key (down + up, the character with it); anything else only as WM_CHAR (the game's
  // text is 8-bit: other characters are dropped)
  const typeChar = (c: string, press: (code: string, ch: string) => void) => {
    const u = c.toUpperCase();
    if (u >= 'A' && u <= 'Z' && u.length === 1) press('Key' + u, c);
    else if (c >= '0' && c <= '9') press('Digit' + c, c);
    else if (c === ' ') press('Space', c);
    else if (c.charCodeAt(0) < 256) msg(WM.CHAR, c.charCodeAt(0), 1);
  };
  textInput.char = c => typeChar(c, tap);
  // the pad's on-screen keyboard (padkb.ts) types straight to the game, past the focus layer
  const gameTap = (code: string, ch: string) => {
    const ev = { code, key: ch, altKey: false, ctrlKey: false, repeat: false, preventDefault() { } } as unknown as KeyboardEvent;
    gameKey(ev, true); gameKey(ev, false);
  };
  padkb = createPadKeyboard({ char: c => typeChar(c, gameTap), key: code => gameTap(code, '') }, () => { editing = false; });
  window.addEventListener('keydown', e => key(e, true));
  window.addEventListener('keyup', e => key(e, false));
  window.addEventListener('blur', () => {
    clearFocus();
    for (let i = 0; i < 256; i++) { if (dik[i]) { keyEvent(i, false); dik[i] = 0; } vk[i] = 0; }
  });
  const pos = (e: MouseEvent) => {
    const r = el.getBoundingClientRect();
    const x = Math.max(0, Math.min(65535, Math.floor((e.clientX - r.left) / r.width * 65536)));
    const y = Math.max(0, Math.min(65535, Math.floor((e.clientY - r.top) / r.height * 65536)));
    cursorX = x / 65536; cursorY = y / 65536;
    Atomics.store(i32, 5, x); Atomics.store(i32, 6, y);
    return (x | y << 16) >>> 0;
  };
  const wParam = (e: MouseEvent) => ((e.buttons & 1) ? 1 : 0) | ((e.buttons & 2) ? 2 : 0) | ((e.buttons & 4) ? 0x10 : 0);
  // Window messages for the button: left 0x201/2, right 0x204/5, middle 0x207/8
  const btn = (b: number, down: boolean) => (b === 0 ? 0x201 : b === 2 ? 0x204 : 0x207) + (down ? 0 : 1);
  // iOS Safari sends emulated mouse events after a tap even when pointerdown was cancelled; the tap already reached the
  // game (below), and the emulated mousedown's focus() would take the focus from the phone keyboard's field (softkb.ts),
  // which closes the keyboard as soon as it opened. So mouse events shortly after a touch are dropped.
  let lastTouch = -1e9;
  window.addEventListener('pointerdown', e => { if (e.pointerType === 'touch') lastTouch = performance.now(); }, { capture: true });
  window.addEventListener('pointerup', e => { if (e.pointerType === 'touch') lastTouch = performance.now(); }, { capture: true });
  const emulated = () => performance.now() - lastTouch < 1000;
  window.addEventListener('mousemove', e => { if (emulated()) return; clearFocus(); msg(WM.MOVE, wParam(e), pos(e)); });
  el.addEventListener('mousedown', e => {
    e.preventDefault();
    if (emulated()) return;
    el.focus();
    const p = pos(e), x = (p & 0xffff) * 640 / 65536, y = (p >>> 16) * 480 / 65536;
    const item = focusEnabled && Atomics.load(i32, 7728 >> 2) === 0 ? readFocusPage(memory.buffer)?.items.find(t => t.kind === 'input' && x >= t.x && x < t.x + t.width && y >= t.y && y < t.y + t.height) : undefined;
    editing = !!item;
    msg(btn(e.button, true), wParam(e), p);
  });
  window.addEventListener('mouseup', e => { if (!emulated()) msg(btn(e.button, false), wParam(e), pos(e)); });
  el.addEventListener('contextmenu', e => e.preventDefault());
  // A tap on a touch screen is a click the game sees: the button stays down at least TAP_HOLD ms (the game polls it once a frame
  // and misses a press and release that come together). preventDefault keeps the browser's emulated mouse events away.
  const TAP_HOLD = 150;
  let touchId: number | null = null, touchAt = 0, touchPos = 0;
  el.style.touchAction = 'none';
  el.addEventListener('pointerdown', e => {
    if (e.pointerType !== 'touch') return;
    e.preventDefault();
    if (touchId !== null) return;
    touchId = e.pointerId; touchAt = performance.now(); touchPos = pos(e);
    msg(WM.MOVE, 0, touchPos); msg(0x201, 1, touchPos);
  });
  el.addEventListener('pointermove', e => { if (e.pointerId === touchId) { touchPos = pos(e); msg(WM.MOVE, 1, touchPos); } });
  const touchUp = (e: PointerEvent) => {
    if (e.pointerId !== touchId) return;
    touchId = null;
    const at = touchPos;
    setTimeout(() => msg(0x202, 0, at), Math.max(0, TAP_HOLD - (performance.now() - touchAt)));
  };
  window.addEventListener('pointerup', touchUp);
  window.addEventListener('pointercancel', touchUp);
  el.addEventListener('wheel', e => { e.preventDefault(); msg(WM.WHEEL, ((e.deltaY > 0 ? -120 : 120) << 16) >>> 0, pos(e)); }, { passive: false });

  // ---- The pad (PC wrapper enhanced/subsystems/gamepad). Only pads with the standard mapping: wheels keep to their own. In menus the
  // left stick is the cursor, A clicks, B and Start are Escape, the d-pad the arrows, the right stick the wheel. While driving Start is
  // Escape and the rest goes to the game's packet (pu_pad_input in runtime/pu_ext.c).
  const f32 = new Float32Array(sab, ptr, 4096 / 4 + 2048);
  const PAD_ON = 7708 >> 2, PAD_BUTTONS = PAD_ON + 1, PAD_LX = PAD_ON + 2, PAD_RT = PAD_ON + 3, PAD_LT = PAD_ON + 4, PAD_MODE = PAD_ON + 5, RUMBLE_SEQ = PAD_ON + 6;
  const B = { A: 0, B: 1, X: 2, LT: 6, RT: 7, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
  const REPEAT_DELAY = 400, REPEAT_PERIOD = 80, CURSOR_SPEED = 1.2, CURSOR_CURVE = 1.8, WHEEL_NOTCHES = 8, STICK_DEAD = 0.10, TRIGGER_DEAD = 0.05;
  const press = (code: string, down: boolean, repeat = false) =>
    key({ code, key: '', altKey: false, ctrlKey: false, repeat, pad: true, preventDefault() { } } as unknown as KeyboardEvent, down);
  const padKeys: Record<string, { down: boolean, next: number }> = {};
  const setKey = (code: string, down: boolean, now: number) => {
    const st = padKeys[code] ??= { down: false, next: 0 };
    if (down === st.down) {
      if (down && code !== 'Escape' && code !== 'Enter' && now >= st.next) { press(code, true, true); st.next = now + REPEAT_PERIOD; }   // Escape would close the page it opened
      return;
    }
    press(code, down);
    st.down = down; st.next = now + REPEAT_DELAY;
  };
  const cursorParam = () => (Math.floor(cursorX * 65536) | Math.floor(cursorY * 65536) << 16) >>> 0;
  let mode = -1, latched = 0, padMouse = false, padFocusA = false, wheelRest = 0, lastTime = 0, rumbleSeen = 0, wasOn = false;
  const setMouse = (down: boolean) => {
    if (down === padMouse) return;
    msg(down ? 0x201 : 0x202, down ? 1 : 0, cursorParam());
    padMouse = down;
  };
  const releaseAll = (now: number) => {
    for (const code in padKeys) setKey(code, false, now);
    setMouse(false);
    padFocusA = false;
  };
  const deadStick = (x: number, y: number) => {
    const m = Math.hypot(x, y);
    if (m <= STICK_DEAD) return [0, 0];
    const k = Math.min(1, (m - STICK_DEAD) / (1 - STICK_DEAD)) / m;
    return [x * k, y * k];
  };
  const deadTrigger = (v: number) => v <= TRIGGER_DEAD ? 0 : Math.min(1, (v - TRIGGER_DEAD) / (1 - TRIGGER_DEAD));
  const padTick = (now: number) => {
    requestAnimationFrame(padTick);
    const seconds = Math.min(0.1, (now - lastTime) / 1000);
    lastTime = now;
    // a pad with the standard layout first (the latest used); else any pad, read as if it had that layout (most generic pads
    // put the face buttons and the left stick where the standard one has them)
    let pad: Gamepad | null = null;
    if (navigator.getGamepads && document.hasFocus())
      for (const p of navigator.getGamepads()) {
        if (!p || !p.connected) continue;
        if (p.mapping === 'standard') { if (!pad || pad.mapping !== 'standard' || p.timestamp > pad.timestamp) pad = p; }
        else if (!pad) pad = p;
      }
    const gameMode = Atomics.load(i32, PAD_MODE);
    padState.mode = gameMode;
    const touch = touchPad.active && gameMode === 2;
    if (!pad && !touch) {
      if (wasOn) { Atomics.store(i32, PAD_ON, 0); releaseAll(now); wasOn = false; }
      return;
    }
    wasOn = true;
    let bits = 0;
    if (pad) {
      pad.buttons.forEach((b, i) => { if (b.pressed && i < 32) bits |= 1 << i; });
      if (bits || pad.axes.some(a => Math.abs(a) > 0.3)) padState.padAt = now;
    }
    if (touch) bits |= touchPad.buttons;
    if (gameMode !== mode) { latched = bits; releaseAll(now); mode = gameMode; }   // A that clicked Continue is not the hand brake
    latched &= bits;
    const held = bits & ~latched;
    const is = (b: number) => (held & 1 << b) !== 0;
    const menu = mode !== 2;
    let [lx, ly] = pad ? deadStick(pad.axes[0] ?? 0, pad.axes[1] ?? 0) : [0, 0];
    if (touch && Math.abs(touchPad.lx) > Math.abs(lx)) lx = touchPad.lx;
    u32[PAD_BUTTONS] = (held & ~(1 << B.START)) >>> 0;
    f32[PAD_LX] = lx;
    const trigger = (b: number) => pad && (is(b) || (pad.buttons[b]?.value ?? 0) > 0) ? deadTrigger(pad.buttons[b]?.value ?? 0) : 0;
    f32[PAD_RT] = Math.max(trigger(B.RT), touch ? touchPad.rt : 0);
    f32[PAD_LT] = Math.max(trigger(B.LT), touch ? touchPad.lt : 0);
    Atomics.store(i32, PAD_ON, 1);
    setKey('Escape', is(B.START) || (menu && is(B.B)), now);
    setKey('ArrowUp', menu && is(B.UP), now);
    setKey('ArrowDown', menu && is(B.DOWN), now);
    setKey('ArrowLeft', menu && is(B.LEFT), now);
    setKey('ArrowRight', menu && is(B.RIGHT), now);
    setKey('Backspace', menu && !!padkb?.open && is(B.X), now);     // X deletes on the pad keyboard
    if (!is(B.A)) padFocusA = false;
    else if (menu && (focusId || (mode === 1 && !padMouse && activeFocusPage()?.spaceCloses))) padFocusA = true;
    setKey('Enter', menu && padFocusA && is(B.A), now);
    if (menu) {
      const m = Math.min(1, Math.hypot(lx, ly));
      if (m > 0) {
        clearFocus();
        const speed = CURSOR_SPEED * Math.pow(m, CURSOR_CURVE) * seconds / Math.hypot(lx, ly);    // screen heights per second
        cursorX = Math.max(0, Math.min(65535 / 65536, cursorX + lx * speed * 0.75));
        cursorY = Math.max(0, Math.min(65535 / 65536, cursorY + ly * speed));
        Atomics.store(i32, 5, Math.floor(cursorX * 65536)); Atomics.store(i32, 6, Math.floor(cursorY * 65536));
        msg(WM.MOVE, padMouse ? 1 : 0, cursorParam());
      }
      setMouse(!padFocusA && is(B.A));
      const [, ry] = pad ? deadStick(pad.axes[2] ?? 0, pad.axes[3] ?? 0) : [0, 0];
      wheelRest -= ry * WHEEL_NOTCHES * seconds;
      const notches = Math.trunc(wheelRest);
      if (notches) { wheelRest -= notches; msg(WM.WHEEL, ((notches * 120) << 16) >>> 0, cursorParam()); }
    } else { setMouse(false); wheelRest = 0; }
    const seq = Atomics.load(i32, RUMBLE_SEQ);
    if (seq !== rumbleSeen) {
      rumbleSeen = seq;
      if (touch && !pad) padState.rumble(i32[PAD_ON + 9]);
      const a = (pad as unknown as { vibrationActuator?: { playEffect(t: string, p: object): Promise<unknown> } } | null)?.vibrationActuator;
      a?.playEffect('dual-rumble', { startDelay: 0, duration: i32[PAD_ON + 9], strongMagnitude: Math.min(1, f32[PAD_ON + 7]), weakMagnitude: Math.min(1, f32[PAD_ON + 8]) }).catch(() => { });
    }
  };
  requestAnimationFrame(padTick);
  // the browser shows a pad to the page after its first button press: say so when it comes, and which layout it is read with
  window.addEventListener('gamepadconnected', e => notice(`Gamepad: ${e.gamepad.id.replace(/\s*\(.*$/, '') || 'connected'}` +
    (e.gamepad.mapping === 'standard' ? '' : ' (layout not standard: read as a standard pad)')));
  window.addEventListener('gamepaddisconnected', () => notice('Gamepad disconnected'));
}

/** a line at the top of the page for a few seconds */
function notice(text: string) {
  let el = document.getElementById('notice');
  if (!el) {
    el = document.createElement('div'); el.id = 'notice';
    el.style.cssText = 'position:fixed;top:12px;left:50%;transform:translateX(-50%);z-index:20;padding:6px 14px;border-radius:6px;' +
      'background:rgba(0,0,0,.7);color:#fff;font:14px system-ui,sans-serif;pointer-events:none;transition:opacity .4s';
    document.body.appendChild(el);
  }
  el.textContent = text; el.style.opacity = '1';
  clearTimeout((el as unknown as { t?: number }).t);
  (el as unknown as { t?: number }).t = window.setTimeout(() => { el!.style.opacity = '0'; }, 4000);
}
