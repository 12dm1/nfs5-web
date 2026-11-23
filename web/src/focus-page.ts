import { nextFocus, type FocusDirection, type FocusRect } from './focus-geometry';

/** A snapshot, never a retained guest pointer: the guest can replace windows on any frame. */
export interface FocusItem extends FocusRect { id: number; kind: 'button' | 'control' | 'tab' | 'input' | 'portrait' | 'combo' | 'color' | 'list' | 'slider' | 'binding'; command?: string; expanded?: boolean; capturing?: boolean; popupHeight?: number; hovered?: boolean; customEditor?: boolean; customRects?: FocusRect[]; customHsv?: number[]; scrollUp?: FocusRect; scrollDown?: FocusRect; sliderValue?: number; sliderCount?: number; }
export interface FocusPage { window: number; items: FocusItem[]; }

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
]);

/** Read the top frontend layer. Invalid or changing structures produce an empty snapshot. */
export function readFocusPage(buffer: ArrayBufferLike): FocusPage | null {
  const m = new DataView(buffer);
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
    if (kind === 'slider') {
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
    if (kind === 'button' && inRange(obj, 0x14c)) {
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
    if (selector && expanded && !customEditor && inRange(obj, 0x1bc)) {
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
    items.push({ id: obj, kind, x, y, width, height, command, expanded, capturing, popupHeight, hovered, customEditor, customRects, customHsv, scrollUp, scrollDown, sliderValue, sliderCount });
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

/** Pause uses the race UI's HButton objects, not FERT windows. */
export function readRaceFocusPage(buffer: ArrayBufferLike): FocusPage | null {
  const m = new DataView(buffer);
  const valid = (p: number, n: number) => p >= 0x10000 && p <= m.byteLength - n;
  const u = (p: number) => valid(p, 4) ? m.getUint32(p, true) : 0;
  const vector = u(0x0060c9c4);
  if (!valid(vector, 8)) return null;
  const begin = u(vector), end = u(vector + 4);
  // Only independently identified race pages are handled here.
  if (!valid(begin, 8) || end - begin !== 8) return null;
  const page = u(begin), width = u(0x00619784), height = u(0x00619788);
  if (!valid(page, 0x130) || !u(page + 0x0c) || width < 320 || width > 8192 || height < 240 || height > 8192) return null;
  const pageType = u(page);
  const offsets = pageType === 0x005b2d54 ? [0x120, 0x124, 0x128]
    : pageType === 0x005b2b0c ? [0x120, 0x124, 0x128, 0x12c]
    : pageType === 0x005b2f8c ? [0x124, 0x128]
    : pageType === 0x005b3554 ? [0x128, 0x12c] : [];
  if (!offsets.length) return null;
  const items: FocusItem[] = [];
  for (const offset of offsets) {
    const button = u(page + offset);
    if (!valid(button, 0x104) || u(button) !== 0x005b2904 || !m.getUint8(button + 0x103)) continue;
    const x = u(button + 4), y = u(button + 8), w = u(button + 12), h = u(button + 16);
    if (!w || !h || x + w > width || y + h > height) continue;
    const back = pageType === 0x005b2d54 ? offset === 0x120 : pageType === 0x005b3554 ? offset === 0x12c : offset === 0x128;
    items.push({ id: button, kind: 'button', x: x * 640 / width, y: y * 480 / height, width: w * 640 / width, height: h * 480 / height, command: back ? 'GOBACK' : undefined });
  }
  return { window: page, items };
}
