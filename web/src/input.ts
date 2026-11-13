// nfs5-web WEB3: keyboard and mouse into the mailbox of runtime/web/input_web.c.
// Layout (bytes): magic 0, msg_head 4, msg_tail 8, key_head 12, key_tail 16, mx 20, my 24, msgs 28 (256 * 12),
// keys 3100 (256 * 16), dik 7196, vk 7452, then the pad: on 7708, buttons 7712, lx, rt, lt (floats) 7716; mode 7728 (from the
// game: 0 frontend, 1 race menu, 2 driving), rumble seq 7732, low, high (floats) 7736, ms 7744.
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
  let cursorX = 0.5, cursorY = 0.5;
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
  window.addEventListener('mousemove', e => msg(WM.MOVE, wParam(e), pos(e)));
  el.addEventListener('mousedown', e => { e.preventDefault(); el.focus(); msg(btn(e.button, true), wParam(e), pos(e)); });
  window.addEventListener('mouseup', e => msg(btn(e.button, false), wParam(e), pos(e)));
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
  const B = { A: 0, B: 1, LT: 6, RT: 7, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
  const REPEAT_DELAY = 400, REPEAT_PERIOD = 80, CURSOR_SPEED = 1.2, CURSOR_CURVE = 1.8, WHEEL_NOTCHES = 8, STICK_DEAD = 0.10, TRIGGER_DEAD = 0.05;
  const press = (code: string, down: boolean, repeat = false) =>
    key({ code, key: '', altKey: false, ctrlKey: false, repeat, preventDefault() { } } as unknown as KeyboardEvent, down);
  const padKeys: Record<string, { down: boolean, next: number }> = {};
  const setKey = (code: string, down: boolean, now: number) => {
    const st = padKeys[code] ??= { down: false, next: 0 };
    if (down === st.down) {
      if (down && code !== 'Escape' && now >= st.next) { press(code, true, true); st.next = now + REPEAT_PERIOD; }   // Escape would close the page it opened
      return;
    }
    press(code, down);
    st.down = down; st.next = now + REPEAT_DELAY;
  };
  const cursorParam = () => (Math.floor(cursorX * 65536) | Math.floor(cursorY * 65536) << 16) >>> 0;
  let mode = -1, latched = 0, padMouse = false, wheelRest = 0, lastTime = 0, rumbleSeen = 0, wasOn = false;
  const setMouse = (down: boolean) => {
    if (down === padMouse) return;
    msg(down ? 0x201 : 0x202, down ? 1 : 0, cursorParam());
    padMouse = down;
  };
  const releaseAll = (now: number) => {
    for (const code in padKeys) setKey(code, false, now);
    setMouse(false);
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
    if (menu) {
      const m = Math.min(1, Math.hypot(lx, ly));
      if (m > 0) {
        const speed = CURSOR_SPEED * Math.pow(m, CURSOR_CURVE) * seconds / Math.hypot(lx, ly);    // screen heights per second
        cursorX = Math.max(0, Math.min(65535 / 65536, cursorX + lx * speed * 0.75));
        cursorY = Math.max(0, Math.min(65535 / 65536, cursorY + ly * speed));
        Atomics.store(i32, 5, Math.floor(cursorX * 65536)); Atomics.store(i32, 6, Math.floor(cursorY * 65536));
        msg(WM.MOVE, padMouse ? 1 : 0, cursorParam());
      }
      setMouse(is(B.A));
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
