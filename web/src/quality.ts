// The graphics profile. 'high': as on the PC (garage car models in the race, twice the draw distance, a reflection face every
// frame, MSAA x4, up to 2160 lines). 'low', phones and tablets: a race frame of the game's own work costs a third (Coastal,
// 8 cars: 12.5 -> 3.6 ms on a desktop core): the game's own car LODs and draw distance, a reflection face every third frame;
// the picture at up to 720 lines without MSAA (tile GPUs pay for every pass of a multisampled frame in memory traffic).
// localStorage 'nfs5.gfx' = 'high' | 'low' chooses; on a development server ?gfx= too.
export type Gfx = 'high' | 'low';

const KEY = 'nfs5.gfx';
let cached: Gfx | null = null;

export function isMobile(): boolean {
  const ua = navigator.userAgent;
  return /Android|iPhone|iPad|iPod|Mobile/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
}

export function gfx(): Gfx {
  if (cached) return cached;
  let v: string | null = null;
  try { v = localStorage.getItem(KEY); } catch { /* storage off */ }
  cached = v === 'high' || v === 'low' ? v : isMobile() ? 'low' : 'high';
  return cached;
}
export function setGfxForTest(v: string | null) { if (v === 'high' || v === 'low') cached = v; }

/** the game's environment for the profile (runtime/pu_ext.c CARLOD, DRAW_X; runtime/pu_cube.c CUBE_EVERY) */
export function gfxEnv(): Record<string, string> {
  return gfx() === 'low' ? { CARLOD: '0', DRAW_X: '1', CUBE_EVERY: '3' } : {};
}
/** the most lines the picture is drawn at */
export const gfxLines = () => gfx() === 'low' ? 720 : 2160;
/** MSAA samples */
export const gfxSamples = () => gfx() === 'low' ? 1 : 4;
