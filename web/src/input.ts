// nfs5-web WEB3: keyboard and mouse into the mailbox of runtime/web/input_web.c.
// Layout (bytes): magic 0, msg_head 4, msg_tail 8, key_head 12, key_tail 16, mx 20, my 24, msgs 28 (256 * 12),
// keys 3100 (256 * 16), dik 7196, vk 7452.
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

export function attachInput(memory: WebAssembly.Memory, ptr: number, el: HTMLElement) {
  const sab = memory.buffer as unknown as SharedArrayBuffer;
  const i32 = new Int32Array(sab, ptr, 4096 / 4 + 2048), u32 = new Uint32Array(sab, ptr, 4096 / 4 + 2048);
  const dik = new Uint8Array(sab, ptr + 7196, 256), vk = new Uint8Array(sab, ptr + 7452, 256);
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
  const key = (e: KeyboardEvent, down: boolean) => {
    const k = K[e.code];
    if (!k) return;
    // the browser's own keys stay with the browser
    if (e.ctrlKey && (e.code === 'KeyR' || e.code === 'KeyW' || e.code === 'KeyT' || e.code === 'KeyN' || e.code === 'KeyL')) return;
    e.preventDefault();
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
  window.addEventListener('keydown', e => key(e, true));
  window.addEventListener('keyup', e => key(e, false));
  window.addEventListener('blur', () => {
    for (let i = 0; i < 256; i++) { if (dik[i]) { keyEvent(i, false); dik[i] = 0; } vk[i] = 0; }
  });
  const pos = (e: MouseEvent) => {
    const r = el.getBoundingClientRect();
    const x = Math.max(0, Math.min(65535, Math.floor((e.clientX - r.left) / r.width * 65536)));
    const y = Math.max(0, Math.min(65535, Math.floor((e.clientY - r.top) / r.height * 65536)));
    Atomics.store(i32, 5, x); Atomics.store(i32, 6, y);
    return (x | y << 16) >>> 0;
  };
  const wParam = (e: MouseEvent) => ((e.buttons & 1) ? 1 : 0) | ((e.buttons & 2) ? 2 : 0) | ((e.buttons & 4) ? 0x10 : 0);
  // Window messages for the button: left 0x201/2, right 0x204/5, middle 0x207/8
  const btn = (b: number, down: boolean) => (b === 0 ? 0x201 : b === 2 ? 0x204 : 0x207) + (down ? 0 : 1);
  window.addEventListener('mousemove', e => msg(WM.MOVE, wParam(e), pos(e)));
  el.addEventListener('mousedown', e => { e.preventDefault(); el.focus(); msg(btn(e.button, true), wParam(e), pos(e)); });
  window.addEventListener('mouseup', e => msg(btn(e.button, false), wParam(e), pos(e)));
  el.addEventListener('contextmenu', e => e.preventDefault());
  el.addEventListener('wheel', e => { e.preventDefault(); msg(WM.WHEEL, ((e.deltaY > 0 ? -120 : 120) << 16) >>> 0, pos(e)); }, { passive: false });
}
