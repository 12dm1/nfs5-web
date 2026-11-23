/** Geometry used by the frontend focus controller. Coordinates are in game pixels. */
export interface FocusRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export type FocusDirection = 'up' | 'down' | 'left' | 'right';

/** Return the nearest candidate in the requested direction, or -1 at an edge. */
export function nextFocus(rects: readonly FocusRect[], current: number, direction: FocusDirection): number {
  if (!rects.length) return -1;
  if (current < 0 || current >= rects.length) return 0;
  const source = rects[current];
  const sx = source.x + source.width / 2, sy = source.y + source.height / 2;
  const horizontal = direction === 'left' || direction === 'right';
  const sign = direction === 'left' || direction === 'up' ? -1 : 1;
  let best = -1, bestScore = Infinity;
  for (let i = 0; i < rects.length; i++) {
    if (i === current) continue;
    const target = rects[i];
    const dx = target.x + target.width / 2 - sx, dy = target.y + target.height / 2 - sy;
    const forward = sign * (horizontal ? dx : dy), side = Math.abs(horizontal ? dy : dx);
    if (forward <= 0) continue;
    const a0 = horizontal ? source.y : source.x;
    const a1 = a0 + (horizontal ? source.height : source.width);
    const b0 = horizontal ? target.y : target.x;
    const b1 = b0 + (horizontal ? target.height : target.width);
    const gap = Math.max(0, Math.max(a0, b0) - Math.min(a1, b1));
    const score = forward + side * 0.5 + gap * 2;
    if (score < bestScore) { best = i; bestScore = score; }
  }
  return best;
}
