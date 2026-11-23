// nfs5-web: on-screen driving controls for touch screens (phones, tablets). Shown only while driving (the game's pad mode 2) and
// only on a touch screen (primary pointer coarse, or once a finger has touched the page; ?touch=1 forces them, ?touch=0 hides
// them), and not while a real pad is in use. They act as a second pad (input.ts touchPad), merged with a real one by the same
// code: steering from a thumb stick on the left, gas and brake on the right, hand brake, gears, camera, look back, reset, pause.
// In the menus and the race menu a tap is a click on the picture (input.ts).
import { touchPad, padState } from './input';

// Gamepad API standard mapping bits, as runtime/pu_ext.c pu_pad_input reads them
const BIT = { A: 0, B: 1, X: 2, Y: 3, LB: 4, RB: 5, START: 9 };
const STEER_RANGE = 0.11;     // thumb travel for full lock, of the screen's height
const STEER_DEAD = 0.04;
const PAD_IDLE_MS = 8000;     // a real pad that has not moved for this long gives the screen back to the touch controls

const CSS = `
#touch { position: fixed; inset: 0; z-index: 10; pointer-events: none; user-select: none; -webkit-user-select: none; touch-action: none; display: none; }
#touch.on { display: block; }
#touch .t { position: absolute; pointer-events: auto; touch-action: none; display: flex; align-items: center; justify-content: center;
  border: 2px solid rgba(255,255,255,.45); background: rgba(20,20,20,.28); color: rgba(255,255,255,.85); border-radius: 2.2vh;
  font: 600 2.6vh system-ui, sans-serif; letter-spacing: .05em; -webkit-tap-highlight-color: transparent; box-sizing: border-box; }
#touch .t.down { background: rgba(255,255,255,.35); border-color: rgba(255,255,255,.9); }
#touch .round { border-radius: 50%; }
#touch #steer { left: 0; bottom: 0; width: 46vw; height: 78vh; border: none; background: none; border-radius: 0; }
#touch #steer .hint { position: absolute; left: 50%; bottom: 22%; transform: translate(-50%, 50%); width: 30vh; height: 30vh;
  border-radius: 50%; border: 2px dashed rgba(255,255,255,.3); display: flex; align-items: center; justify-content: space-between;
  padding: 0 3vh; box-sizing: border-box; font-size: 4vh; color: rgba(255,255,255,.5); }
#touch #steer.active .hint { display: none; }
#touch .base, #touch .knob { position: absolute; border-radius: 50%; transform: translate(-50%, -50%); display: none; pointer-events: none; }
#touch .base { width: 26vh; height: 26vh; border: 2px solid rgba(255,255,255,.35); background: rgba(20,20,20,.2); }
#touch .knob { width: 11vh; height: 11vh; background: rgba(255,255,255,.45); }
#touch #steer.active .base, #touch #steer.active .knob { display: block; }
#touch #gas { right: 3vw; bottom: 5vh; width: 19vh; height: 38vh; }
#touch #brake { right: calc(3vw + 22vh); bottom: 5vh; width: 19vh; height: 26vh; }
#touch #hand { right: calc(3vw + 22vh); bottom: 34vh; width: 19vh; height: 11vh; font-size: 2.2vh; }
#touch #up { right: 3vw; bottom: 46vh; width: 19vh; height: 11vh; }
#touch #downg { right: calc(3vw + 22vh); bottom: 48vh; width: 19vh; height: 9vh; }
#touch .top { top: 2.5vh; width: 11vh; height: 8vh; font-size: 2vh; }
#touch #pause { right: 2vw; }
#touch #cam { right: calc(2vw + 13vh); }
#touch #look { right: calc(2vw + 26vh); }
#touch #reset { right: calc(2vw + 39vh); }
`;

interface Btn { id: string; label: string; cls?: string; bit?: number; rt?: boolean; lt?: boolean }
const BUTTONS: Btn[] = [
  { id: 'gas', label: 'GAS', rt: true },
  { id: 'brake', label: 'BRAKE', lt: true },
  { id: 'hand', label: 'HAND BRAKE', bit: BIT.A },
  { id: 'up', label: 'GEAR +', bit: BIT.RB },
  { id: 'downg', label: 'GEAR −', bit: BIT.LB },
  { id: 'pause', label: '❚❚', cls: 'top', bit: BIT.START },
  { id: 'cam', label: 'CAMERA', cls: 'top', bit: BIT.B },
  { id: 'look', label: 'LOOK', cls: 'top', bit: BIT.X },
  { id: 'reset', label: 'RESET', cls: 'top', bit: BIT.Y },
];

export function attachTouch() {
  const q = new URLSearchParams(location.search);
  const force = q.get('touch');
  if (force === '0') return;
  let touchScreen = force === '1' || matchMedia('(pointer: coarse)').matches;
  window.addEventListener('pointerdown', e => { if (e.pointerType === 'touch') touchScreen = true; }, { capture: true });

  const style = document.createElement('style'); style.textContent = CSS; document.head.appendChild(style);
  const root = document.createElement('div'); root.id = 'touch';
  root.addEventListener('contextmenu', e => e.preventDefault());
  document.body.appendChild(root);

  // which button each finger holds; the stick has its own finger
  const held = new Map<number, Btn>();
  const els = new Map<string, HTMLElement>();
  const update = () => {
    let bits = 0, rt = 0, lt = 0;
    for (const b of held.values()) { if (b.bit !== undefined) bits |= 1 << b.bit; if (b.rt) rt = 1; if (b.lt) lt = 1; }
    touchPad.buttons = bits; touchPad.rt = rt; touchPad.lt = lt;
    for (const b of BUTTONS) els.get(b.id)!.classList.toggle('down', [...held.values()].includes(b));
  };
  for (const b of BUTTONS) {
    const e = document.createElement('div');
    e.id = b.id; e.className = 't ' + (b.cls ?? ''); e.textContent = b.label;
    e.addEventListener('pointerdown', ev => { ev.preventDefault(); e.setPointerCapture(ev.pointerId); held.set(ev.pointerId, b); update(); });
    const up = (ev: PointerEvent) => { if (held.get(ev.pointerId) === b) { held.delete(ev.pointerId); update(); } };
    e.addEventListener('pointerup', up); e.addEventListener('pointercancel', up); e.addEventListener('lostpointercapture', up);
    root.appendChild(e); els.set(b.id, e);
  }

  // the stick: where the thumb lands is the centre, sideways is the wheel
  const steer = document.createElement('div'); steer.id = 'steer'; steer.className = 't';
  steer.innerHTML = '<div class="hint"><span>◀</span><span>▶</span></div><div class="base"></div><div class="knob"></div>';
  const base = steer.querySelector('.base') as HTMLElement, knob = steer.querySelector('.knob') as HTMLElement;
  let stickId: number | null = null, x0 = 0, y0 = 0;
  const setSteer = (x: number, y: number) => {
    const r = STEER_RANGE * innerHeight;
    let v = Math.max(-1, Math.min(1, (x - x0) / r));
    if (Math.abs(v) < STEER_DEAD) v = 0;
    touchPad.lx = v;
    const box = steer.getBoundingClientRect();
    knob.style.left = (x0 - box.left + v * r) + 'px'; knob.style.top = (y0 - box.top) + 'px';
    void y;
  };
  steer.addEventListener('pointerdown', ev => {
    ev.preventDefault();
    if (stickId !== null) return;
    stickId = ev.pointerId; steer.setPointerCapture(ev.pointerId);
    x0 = ev.clientX; y0 = ev.clientY;
    const box = steer.getBoundingClientRect();
    base.style.left = (x0 - box.left) + 'px'; base.style.top = (y0 - box.top) + 'px';
    steer.classList.add('active');
    setSteer(ev.clientX, ev.clientY);
  });
  steer.addEventListener('pointermove', ev => { if (ev.pointerId === stickId) setSteer(ev.clientX, ev.clientY); });
  const stickUp = (ev: PointerEvent) => {
    if (ev.pointerId !== stickId) return;
    stickId = null; touchPad.lx = 0; steer.classList.remove('active');
  };
  steer.addEventListener('pointerup', stickUp); steer.addEventListener('pointercancel', stickUp); steer.addEventListener('lostpointercapture', stickUp);
  root.insertBefore(steer, root.firstChild);

  const releaseAll = () => {
    held.clear(); stickId = null; steer.classList.remove('active');
    touchPad.buttons = 0; touchPad.lx = 0; touchPad.rt = 0; touchPad.lt = 0;
    update();
  };
  // a phone vibrates on impacts while its screen drives (input.ts passes the game's rumble on)
  padState.rumble = ms => { if (touchPad.active && navigator.vibrate) navigator.vibrate(Math.max(15, Math.min(200, ms | 0))); };

  let shown = false;
  const tick = () => {
    requestAnimationFrame(tick);
    const show = touchScreen && padState.mode === 2 && performance.now() - padState.padAt > PAD_IDLE_MS;
    if (show === shown) return;
    shown = show;
    root.classList.toggle('on', show);
    touchPad.active = show;
    if (!show) releaseAll();
  };
  requestAnimationFrame(tick);

  // a phone plays in full screen and landscape: asked on the first touch (browsers allow both only from a gesture)
  let asked = false;   // once: a player who leaves full screen stays out of it
  const full = () => {
    if (asked || !touchScreen || document.fullscreenElement || !document.documentElement.requestFullscreen) return;
    asked = true;
    document.documentElement.requestFullscreen({ navigationUI: 'hide' }).then(() => {
      const o = screen.orientation as unknown as { lock?: (k: string) => Promise<void> };
      return o.lock?.('landscape');
    }).catch(() => { });
  };
  window.addEventListener('pointerup', e => { if (e.pointerType === 'touch') full(); });
}
