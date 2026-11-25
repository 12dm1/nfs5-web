import { nextFocus, type FocusDirection, type FocusRect } from './focus-geometry';

/** A snapshot, never a retained guest pointer: the guest can replace windows on any frame. */
export interface FocusItem extends FocusRect { id: number; kind: 'button' | 'control' | 'tab' | 'input' | 'portrait' | 'combo' | 'color' | 'list' | 'slider' | 'binding'; command?: string; expanded?: boolean; capturing?: boolean; popupHeight?: number; hovered?: boolean; customEditor?: boolean; customRects?: FocusRect[]; customHsv?: number[]; scrollUp?: FocusRect; scrollDown?: FocusRect; sliderValue?: number; sliderCount?: number;
  /** a button of the page's own (no game object): the key it sends to the game, its caption */
  key?: string; label?: string;
  /** a slider moved by dragging its thumb (FEHorizontalSlider): the thumb's left edge */
  thumbX?: number; }
/** spaceCloses: a race page that Space closes (Enter / pad A without a focused item send Space); padBack: the key pad B
 * and Start send instead of Escape */
export interface FocusPage { window: number; items: FocusItem[]; spaceCloses?: boolean; padBack?: string; }

const kinds = new Map<number, FocusItem['kind']>([
  [0x005b5d10, 'button'],       // FEGoToButton
  [0x005b60ac, 'input'],        // FEInputBox
  [0x005b64b0, 'portrait'],     // FEPlayerDriverPicture
  [0x005b5c24, 'combo'],        // FEComboBox
  [0x005b5ba8, 'color'],        // colour selector
  [0x005b611c, 'list'],         // FEListBox
  [0x005b5d80, 'button'],       // bottom bar button
  [0x005b59e4, 'input'],        // character-vector input
  [0x005b5ac8, 'input'],        // chat input
  [0x005b5b38, 'control'],      // check box
  [0x005b5ed0, 'tab'],          // overlapping graphical tabs
  [0x005b667c, 'control'],      // radio item
  [0x005b5798, 'control'],      // camera selection
  [0x005b5970, 'control'],      // car setting button
  [0x005b5e60, 'slider'],       // changeable indicator bar
  [0x005bac84, 'binding'],      // controller binding
  [0x005b5fc4, 'slider'],       // FEHorizontalSlider (the chronicle's year): its thumb is dragged
  [0x005b8ce4, 'button'],       // FEChroniclePictureIconControl (its click: 005089C0)
  [0x005b6278, 'button'],       // FEMovieControlButton (its click: 004CE760)
  [0x005b7e0c, 'control'],      // FEKnockoutTrackListControl (its click: 004FA470)
]);
/** controls of their own class that are clicked like buttons but have no goto command attribute at +0x148 */
const OWN_CLICK = new Set([0x005b8ce4, 0x005b6278, 0x005b7e0c]);

/** The rows of an open combo (FEComboBox) or of a list box (FEListBox derives from it, same fields), as the game's own hover
 * test (004C6F60) sees them: rows from the first visible one down, each as high as the accessor's row height (+0x68 of the
 * accessor at +0x144, a constant for most accessors, per row for the car lists), within the open height +0x1C4. The row under
 * the mouse (+0x1A4 flag, +0x1A8 index) is what counts; the rest is the first guess. */
export interface RowsInfo {
  top: number; height: number; hovered: boolean; hoverIndex: number;
  first: number; count: number; rowHeight: number; selected: number; scroll: boolean;
  scrollUp?: FocusRect; scrollDown?: FocusRect;
}
export function readRows(buffer: ArrayBufferLike, item: FocusItem): RowsInfo | null {
  const m = new DataView(buffer);
  const ok = (p: number, n: number) => p >= 0x10000 && p <= m.byteLength - n;
  const u = (p: number) => ok(p, 4) ? m.getUint32(p, true) : 0;
  const i = (p: number) => ok(p, 4) ? m.getInt32(p, true) : 0;
  const b = (p: number) => ok(p, 1) ? m.getUint8(p) : 0;
  const obj = item.id;
  if (!ok(obj, 0x1e0) || (item.kind !== 'combo' && item.kind !== 'color' && item.kind !== 'list')) return null;
  const list = item.kind === 'list';
  if (!list && !(u(obj + 0x10c) & 0x40)) return null;
  const height = list ? item.height : i(obj + 0x1c4);
  if (height <= 0 || height > 480) return null;
  const acc = u(obj + 0x144), avt = u(acc);
  // the common accessor methods: count 004BC4A0 (+0x14), position 004BC4B0 (+0x18 unless +0x1D), row height "mov eax, imm32; ret"
  const count = ok(acc, 0x20) && u(avt + 0x24) === 0x004bc4a0 ? i(acc + 0x14) : -1;
  const selected = ok(acc, 0x20) && u(avt + 0x28) === 0x004bc4b0 && !b(acc + 0x1d) ? i(acc + 0x18) : -1;
  const code = u(avt + 0x68);
  const rowHeight = ok(code, 8) && b(code) === 0xb8 && (b(code + 5) === 0xc3 || b(code + 5) === 0xc2) ? i(code + 1) : 0;
  const sa = u(obj + 0x1bc);
  const first = ok(sa, 0x20) && u(u(sa) + 0x28) === 0x004bc4b0 && !b(sa + 0x1d) ? Math.max(0, i(sa + 0x18)) : 0;
  const scroll = !!b(obj + 0x1dc);
  let scrollUp: FocusRect | undefined, scrollDown: FocusRect | undefined;
  if (scroll) {
    const slider = u(obj + 0x1b8);
    const arrow = (offset: number): FocusRect | undefined => {
      if (!ok(slider, offset + 16) || u(slider) !== 0x005b6a64) return undefined;
      const x = i(slider + offset), y = i(slider + offset + 4), width = i(slider + offset + 8), height = i(slider + offset + 12);
      if (width <= 0 || height <= 0 || width > 64 || height > 64 || x < 0 || y < 0 || x + width > 640 || y + height > 480) return undefined;
      return { x, y, width, height };
    };
    scrollUp = arrow(0x12c); scrollDown = arrow(0x11c);
  }
  return { top: list ? item.y : item.y + item.height, height, hovered: !!b(obj + 0x1a4), hoverIndex: i(obj + 0x1a8),
    first, count, rowHeight: rowHeight > 0 && rowHeight < 64 ? rowHeight : 0, selected, scroll, scrollUp, scrollDown };
}

/** The frontend's message boxes (FEDialog: yes/no, OK, error): a list of their own at [0069A05C], not windows of the layers. The
 * one the game sends the input to (00512120) is the last whose +0x44 (005124E0) says it is up: +0xD8 shown and +0xDC (its fade)
 * 0xFF. Its children (vector at +0xD4) are FEDialogButton (0x005BA108, 0xF8 bytes): hit-tested (00515060) at the dialog's
 * corner +0xDC/+0xE0 plus the button's +0xD4/+0xD8, float size +0xE4/+0xE8; a click (00515130) needs the hover the mouse
 * move set. The negative button is the one whose caption is 0x2BF (FEDialogTwoItem 00513AD0: 0x2BE/0x2BF, "yes"/"no";
 * FEDialogInputCancel lists it first), else the last of two; a single one is also the way out. */
function readDialog(m: DataView): FocusPage | null {
  const ok = (p: number, n: number) => p >= 0x10000 && p <= m.byteLength - n;
  const u = (p: number) => ok(p, 4) ? m.getUint32(p, true) : 0;
  const i = (p: number) => ok(p, 4) ? m.getInt32(p, true) : 0;
  const list = u(0x0069a05c);
  if (!ok(list, 8)) return null;
  const begin = u(list), end = u(list + 4);
  if (end <= begin || end - begin > 256 || (end - begin) % 4 || !ok(begin, end - begin)) return null;
  for (let e = end - 4; e >= begin; e -= 4) {
    const dialog = u(e);
    if (!ok(dialog, 0x104) || !m.getUint8(dialog + 0xd8) || u(dialog + 0xdc) !== 0xff) continue;
    const children = u(dialog + 0xd4), cb = u(children), ce = u(children + 4);
    if (!ok(children, 8) || ce < cb || ce - cb > 64 || (ce - cb) % 4 || (ce > cb && !ok(cb, ce - cb))) return null;
    const items: FocusItem[] = [];
    let negative: FocusItem | undefined;
    for (let c = cb; c < ce; c += 4) {
      const b = u(c);
      if (ok(b, 0xf4) && u(b) === 0x005ba23c) {
        // FEDialogInput (FEDialogInputCancel's text, 005149A0): its text is centred at the corner + (+0xD4, +0xD8); it takes
        // every typed character while the dialog is up, so no click (key '')
        const cx = i(b + 0xdc) + i(b + 0xd4), cy = i(b + 0xe0) + i(b + 0xd8);
        if (cx >= 100 && cx <= 540 && cy >= 0 && cy + 14 <= 480) items.push({ id: b, kind: 'input', x: cx - 100, y: cy, width: 200, height: 14, key: '' });
        continue;
      }
      if (!ok(b, 0xf8) || u(b) !== 0x005ba108) continue;
      const x = i(b + 0xdc) + i(b + 0xd4), y = i(b + 0xe0) + i(b + 0xd8);
      const width = m.getFloat32(b + 0xe4, true), height = m.getFloat32(b + 0xe8, true);
      if (!(width > 0 && height > 0 && width < 640 && height < 480) || x < 0 || y < 0 || x + width > 640 || y + height > 480) continue;
      const item: FocusItem = { id: b, kind: 'button', x, y, width, height };
      // the caption's string: 0x2BF "no" / "cancel" (and 0x2BD of 00505B50's pair) is the way out
      const text = i(b + 0xf0);
      if (text === 0x2bf || text === 0x2bd) negative = item;
      items.push(item);
    }
    const buttons = items.filter(x => x.kind === 'button');
    if (!negative && (buttons.length === 1 || buttons.length === 2)) negative = buttons[buttons.length - 1];
    if (negative) negative.command = 'GOBACK';
    return { window: dialog, items };
  }
  return null;
}

/** Read the top frontend layer (or the message box over it). Invalid or changing structures produce an empty snapshot. */
export function readFocusPage(buffer: ArrayBufferLike): FocusPage | null {
  const m = new DataView(buffer);
  const dialog = readDialog(m);
  if (dialog) return dialog;
  const inRange = (p: number, n: number) => p >= 0x10000 && p <= m.byteLength - n;
  const u = (p: number) => inRange(p, 4) ? m.getUint32(p, true) : 0;
  const i = (p: number) => inRange(p, 4) ? m.getInt32(p, true) : 0;
  const fe = u(0x0065b928), layers = u(fe + 0xd8);
  if (!inRange(fe, 0xdc) || !inRange(layers, 8)) return null;
  const first = u(layers), last = u(layers + 4);
  if (last <= first || last - first > 32 || (last - first) % 4 || !inRange(first, last - first)) return null;
  const layer = u(last - 4), window = u(layer), pending = u(layer + 4);
  if (!inRange(layer, 8) || pending || !inRange(window, 0x110)) return null;
  const items: FocusItem[] = [];
  const visited = new Set<number>();
  const collect = (owner: number, parentX = 0, parentY = 0, depth = 0): boolean => {
  if (depth > 8 || visited.has(owner) || !inRange(owner, 0x110)) return false;
  visited.add(owner);
  const vector = u(owner + 0x10c), begin = u(vector), end = u(vector + 4);
  if (!inRange(vector, 8) || end < begin || end - begin > 4096 || (end - begin) % 4 || (end > begin && !inRange(begin, end - begin))) return false;
  const origin = u(owner + 0xe0);
  if (!inRange(origin, 0x40)) return false;
  const ox = parentX + i(origin + 0x38), oy = parentY + i(origin + 0x3c);
  for (let p = begin; p < end; p += 4) {
    const obj = u(p);
    if (!inRange(obj, 0x114)) continue;
    const vt = u(obj), flags = u(obj + 0x10c), kind = kinds.get(vt);
    if (flags & 0x30) continue;
    const pos = u(obj + 0xe0);
    if (!inRange(pos, 0x40)) continue;
    if ((vt === 0x005b6358 || vt === 0x005b6600) && inRange(obj, 0x150)) {
      collect(u(obj + 0x14c), ox + i(pos + 0x38), oy + i(pos + 0x3c), depth + 1);
      continue;
    }
    if (!kind) continue;
    let sliderValue: number | undefined, sliderCount: number | undefined;
    let thumbX: number | undefined;
    if (kind === 'slider' && vt === 0x005b5fc4) {
      // FEHorizontalSlider: FESlider_MainFunctionality at +0x144, its accessor +0xD8, the thumb's PointAttribute +0xEC (x +0x38)
      const main = u(obj + 0x144), accessor = u(main + 0xd8), thumb = u(main + 0xec);
      if (!inRange(main, 0x110) || u(main) !== 0x005b6764 || !inRange(accessor, 0x1e) || u(accessor) !== 0x005b502c || m.getUint8(accessor + 0x1d)) continue;
      if (!inRange(thumb, 0x40) || u(thumb) !== 0x005bb0ac) continue;
      sliderCount = i(accessor + 0x14); sliderValue = i(accessor + 0x18); thumbX = i(thumb + 0x38);
      if (sliderCount < 2 || sliderCount > 4096 || sliderValue < 0 || sliderValue >= sliderCount) continue;
    } else if (kind === 'slider') {
      if (!inRange(obj, 0x158) || !u(obj + 0x150)) continue;
      const accessor = u(obj + 0x154);
      if (!inRange(accessor, 0x1e) || u(accessor) !== 0x005b502c || m.getUint8(accessor + 0x1d)) continue;
      sliderCount = i(accessor + 0x14); sliderValue = i(accessor + 0x18);
      if (sliderCount < 2 || sliderCount > 4096 || sliderValue < 0 || sliderValue >= sliderCount) continue;
    }
    const x = ox + i(pos + 0x38) + i(obj + 0xf8), y = oy + i(pos + 0x3c) + i(obj + 0xfc);
    const width = i(obj + 0x100) - i(obj + 0xf8), height = i(obj + 0x104) - i(obj + 0xfc);
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > 640 || y + height > 480) continue;
    let command: string | undefined;
    if (kind === 'button' && !OWN_CLICK.has(vt) && inRange(obj, 0x14c)) {
      const attribute = u(obj + 0x148), start = u(attribute + 0x3c), stop = u(attribute + 0x40);
      if (inRange(attribute, 0x44) && stop >= start && stop - start <= 64 && (stop === start || inRange(start, stop - start))) {
        command = Array.from({ length: stop - start }, (_, n) => String.fromCharCode(m.getUint8(start + n))).join('');
      }
    }
    const expanded = (kind === 'combo' || kind === 'color') && !!(u(obj + 0x10c) & 0x40);
    const selector = kind === 'combo' || kind === 'color';
    const popupHeight = selector && inRange(obj, 0x1c8) ? i(obj + 0x1c4) : 0;
    const hovered = (selector || kind === 'list') && inRange(obj, 0x1a5) && !!m.getUint8(obj + 0x1a4);
    const customEditor = kind === 'color' && !!m.getUint8(0x0065b710);
    let customRects: FocusRect[] | undefined, customHsv: number[] | undefined;
    if (customEditor && inRange(obj, 0x294)) {
      const rects = [0x224, 0x234, 0x274, 0x254, 0x264].map(offset => {
        const x = i(obj + offset), y = i(obj + offset + 4);
        return { x, y, width: i(obj + offset + 8) - x, height: i(obj + offset + 12) - y };
      });
      if (rects.every(r => r.width > 0 && r.height > 0 && r.x >= 0 && r.y >= 0 && r.x + r.width < 640 && r.y + r.height < 480)) {
        customRects = rects;
        customHsv = [0x288, 0x28c, 0x290].map(offset => m.getFloat32(obj + offset, true));
      }
    }
    let scrollUp: FocusRect | undefined, scrollDown: FocusRect | undefined;
    // only a scroll bar the list shows (+0x1DC): a hidden one keeps its rectangles
    if (selector && expanded && !customEditor && inRange(obj, 0x1e0) && m.getUint8(obj + 0x1dc)) {
      const slider = u(obj + 0x1b8);
      const arrow = (offset: number): FocusRect | undefined => {
        if (!inRange(slider, offset + 16) || u(slider) !== 0x005b6a64) return undefined;
        const x = i(slider + offset), y = i(slider + offset + 4), width = i(slider + offset + 8), height = i(slider + offset + 12);
        if (width <= 0 || height <= 0 || width > 64 || height > 64 || x < 0 || y < 0 || x + width > 640 || y + height > 480) return undefined;
        return { x, y, width, height };
      };
      scrollUp = arrow(0x12c); scrollDown = arrow(0x11c);
    }
    const capturing = kind === 'binding' && !!(flags & 0x40);
    items.push({ id: obj, kind, x, y, width, height, command, expanded, capturing, popupHeight, hovered, customEditor, customRects, customHsv, scrollUp, scrollDown, sliderValue, sliderCount,
      thumbX: thumbX === undefined ? undefined : x + thumbX });
  }
  return true;
  };
  if (!collect(window)) return null;
  // The shared bottom bar belongs to the main layer, not to a modal layer above it.
  if (last - first === 4) collect(u(fe + 0xdc));
  return { window, items };
}

export function moveFocus(page: FocusPage, current: number, direction: FocusDirection): FocusItem | undefined {
  const index = page.items.findIndex(item => item.id === current);
  const next = nextFocus(page.items, index, direction);
  return next < 0 ? undefined : page.items[next];
}

/** Pick a point not covered by an earlier tab in the game's hit-test order. */
export function focusActivationPoint(page: FocusPage, item: FocusItem): { x: number; y: number } {
  const center = { x: item.x + item.width / 2, y: item.y + item.height / 2 };
  if (item.kind !== 'tab') return center;
  const index = page.items.findIndex(x => x.id === item.id);
  if (index < 0) return center;
  const earlier = page.items.slice(0, index).filter(x => x.kind === 'tab');
  let result = center, distance = Infinity;
  for (let n = 1; n < 20; n++) {
    const x = item.x + item.width * n / 20, y = center.y;
    if (earlier.some(t => x >= t.x && x < t.x + t.width && y >= t.y && y < t.y + t.height)) continue;
    const d = Math.abs(x - center.x);
    if (d < distance) { result = { x, y }; distance = d; }
  }
  return result;
}

/** Race page classes (vtables) whose generic input handler 0042A3D0 closes them on Space (code 0x19): results, info. */
const SPACE_PAGES = new Set([0x005b2a40 /* HCongrats */, 0x005b2e60 /* HRaceSummary */, 0x005b32b0 /* HTrackRecords */,
  0x005b3210 /* HTournamentStandings */, 0x005b2bfc /* HInfo */, 0x005b2c88 /* HMissionSummary (its Continue: 00429390) */]);
/** The replay bar (HReplay at [0060CA58]): 7 hit slots of [005B2EF0]x[005B2EF4] at +0x12C.. (x, y pairs; 0042D880); the
 * game asks it while the race is over ([00657424] == 2, the race page controller 00425EF0). */
const REPLAY_SLOTS = [0x12c, 0x134, 0x13c, 0x144, 0x14c, 0x154, 0x15c];

/** The race's menus use the race UI's HButton objects (pages of [0060C9C4], the first one is the one asked), not FERT windows; the
 * replay bar after the race; the network race's leave notice (HInfo) gets two buttons of the page's own (synthetic: the
 * game reads Space to go back and Esc to leave). */
export function readRaceFocusPage(buffer: ArrayBufferLike): FocusPage | null {
  const m = new DataView(buffer);
  const valid = (p: number, n: number) => p >= 0x10000 && p <= m.byteLength - n;
  const u = (p: number) => valid(p, 4) ? m.getUint32(p, true) : 0;
  const i = (p: number) => valid(p, 4) ? m.getInt32(p, true) : 0;
  const width = u(0x00619784), height = u(0x00619788);
  if (width < 320 || width > 8192 || height < 240 || height > 8192) return null;
  const vector = u(0x0060c9c4);
  let page = 0;
  if (valid(vector, 8)) {
    const begin = u(vector), end = u(vector + 4);
    // the controller (00425EF0) asks the first page; the result pages leave from the front one by one
    if (end > begin && end - begin <= 64 && (end - begin) % 8 === 0 && valid(begin, end - begin)) page = u(begin);
    if (page && (!valid(page, 0x200) || !u(page + 0x0c))) return null;
  }
  const replay = u(0x0060ca58), over = i(0x00657424) === 2 && valid(replay, 0x170);
  if (!page && !over) return null;
  // The race's menus are laid out on the HUD's virtual screen (runtime/pu_ext.c HUD_VW/HUD_VH), not on the race's mode.
  const vw = u(0x006c1f00), vh = u(0x006c1f04);
  const hudW = vw >= 320 && vw <= width ? vw : width, hudH = vh >= 240 && vh <= height ? vh : height;
  const scale = height / hudH;
  // HUD layout -> 640x480 page pixels: one scale for both axes, then the piece keeps its third of the screen after a live
  // resize (runtime/pu_ext.c hud_anchor_x; the layout's width is hudW * scale)
  const place = (x: number, y: number, w: number, h: number) => {
    const c = (x + w / 2) / hudW, side = c < 1 / 3 ? 0 : c > 2 / 3 ? 1 : 0.5;
    const sx = x * scale + (width - hudW * scale) * side, sy = y * scale;
    return { x: sx * 640 / width, y: sy * 480 / height, width: w * scale * 640 / width, height: h * scale * 480 / height };
  };
  const items: FocusItem[] = [];
  const pageType = page ? u(page) : 0;
  const button = (b: number, command?: string) => {
    if (!valid(b, 0x104) || u(b) !== 0x005b2904 || !m.getUint8(b + 0x103)) return;
    const x = i(b + 4), y = i(b + 8), w = i(b + 12), h = i(b + 16);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > hudW || y + h > hudH || items.some(t => t.id === b)) return;
    items.push({ id: b, kind: 'button', ...place(x, y, w, h), command });
  };
  if (page) {
    const known = pageType === 0x005b2d54 ? [0x120, 0x124, 0x128]          // HPause: continue, restart, quit
      : pageType === 0x005b2b0c ? [0x120, 0x124, 0x128, 0x12c]             // HExit
      : pageType === 0x005b2f8c ? [0x124, 0x128]                           // HReplaySave
      : pageType === 0x005b3554 ? [0x128, 0x12c] : null;                   // HYesNoOverlay
    if (known) for (const offset of known) {
      const back = pageType === 0x005b2d54 ? offset === 0x120 : pageType === 0x005b3554 ? offset === 0x12c : offset === 0x128;
      button(u(page + offset), back ? 'GOBACK' : undefined);
    } else for (let o = 0x10; o < 0x200; o += 4) button(u(page + o));     // other pages: their visible buttons
    // HReplaySave's name (HTextEntryVisual +0x120): it takes every typed character (0042E310), no click; key '' says so
    const entry = pageType === 0x005b2f8c ? u(page + 0x120) : 0;
    if (entry && valid(entry, 0x14) && u(entry) === 0x005b3160) {
      const x = i(entry + 4), y = i(entry + 8), w = i(entry + 12), h = i(entry + 16);
      if (w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= hudW && y + h <= hudH) items.unshift({ id: entry, kind: 'input', ...place(x, y, w, h), key: '' });
    }
  }
  const spaceCloses = SPACE_PAGES.has(pageType);
  // the network race's Esc notice ("You are about to leave the race", 00426800 for game type 3): Space goes back, Esc leaves.
  // The two buttons go on the box's footer strip: HInfo's quads (x, y, 0, 1 each) at +0x1C (title), +0x5C (footer), +0x9C (body).
  if (pageType === 0x005b2bfc && i(0x006573e8) === 3 && !over) {
    const f = (o: number) => m.getFloat32(page + o, true);
    let left = f(0x5c), right = f(0x6c), top = f(0x60), bottom = f(0x80);
    if (!(left >= 0 && right > left + 100 && right <= hudW && top >= 0 && bottom > top + 8 && bottom <= hudH)) {
      left = hudW * 0.25; right = hudW * 0.75; top = hudH * 0.66; bottom = top + 22;
    }
    const w = (right - left) / 2 - 12;
    items.push({ id: page + 1, kind: 'button', ...place(left + 8, top, w, bottom - top), key: 'Space', label: 'Back to the race' });
    items.push({ id: page + 2, kind: 'button', ...place(right - 8 - w, top, w, bottom - top), key: 'Escape', label: 'Leave the race' });
    return { window: page, items, padBack: 'Space', spaceCloses };
  }
  // the replay bar under the results (the result pages have no buttons; Space closes them) or alone after them
  if (over && (!page || spaceCloses)) {
    const sw = i(0x005b2ef0), sh = i(0x005b2ef4);
    if (sw > 0 && sh > 0 && sw < 200 && sh < 200) for (const offset of REPLAY_SLOTS) {
      const x = i(replay + offset), y = i(replay + offset + 4);
      if (x <= 0 || y <= 0 || x + sw > hudW || y + sh > hudH) continue;
      items.push({ id: replay + offset, kind: 'button', ...place(x, y, sw, sh) });
    }
  }
  if (!items.length && !spaceCloses) return null;
  return { window: page || replay, items, spaceCloses };
}
