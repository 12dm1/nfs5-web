// KB2: an on-screen keyboard for the pad. The game's text fields (player name, login, chat, IP, the race's replay name) take
// typed characters only; a pad has none, and a desktop browser shows no system keyboard. The focus layer (input.ts) opens
// this one when the pad's A activates a text field: the d-pad moves over the keys, A types, X deletes, B closes. The keys
// reach the game as the phone keyboard's do (input.ts textInput). The keyboard and the mouse never open it.

export interface PadKeyboard {
  readonly open: boolean;
  show(): void;
  hide(): void;
  /** a key while the keyboard is up (the pad's keys arrive as keys): true when it was the keyboard's */
  key(code: string, down: boolean): boolean;
}

const ROWS = ['1234567890', 'qwertyuiop', 'asdfghjkl-', 'zxcvbnm._@'];
const SPECIAL = ['Shift', 'Space', 'Delete', 'Enter', 'Close'] as const;

export function createPadKeyboard(send: { char(c: string): void; key(code: string): void }, onHide: () => void): PadKeyboard {
  const panel = document.createElement('div');
  Object.assign(panel.style, {
    position: 'fixed', left: '50%', bottom: '4%', transform: 'translateX(-50%)', zIndex: '12', display: 'none',
    background: 'rgba(10, 12, 16, 0.92)', border: '1px solid rgba(255, 255, 255, 0.35)', borderRadius: '6px', padding: '8px',
    font: '15px sans-serif', color: '#fff', userSelect: 'none', pointerEvents: 'none',
  });
  document.body.appendChild(panel);
  let row = 1, col = 0, shift = true, open = false;
  const cells: HTMLDivElement[][] = [];
  const label = (r: number, c: number) => r < ROWS.length ? (shift ? ROWS[r][c].toUpperCase() : ROWS[r][c]) : SPECIAL[c] === 'Shift' ? (shift ? 'SHIFT' : 'shift') : SPECIAL[c];
  const width = (r: number) => r < ROWS.length ? ROWS[r].length : SPECIAL.length;
  const build = () => {
    panel.replaceChildren();
    cells.length = 0;
    for (let r = 0; r <= ROWS.length; r++) {
      const line = document.createElement('div');
      Object.assign(line.style, { display: 'flex', gap: '4px', marginTop: r ? '4px' : '0', justifyContent: 'center' });
      const list: HTMLDivElement[] = [];
      for (let c = 0; c < width(r); c++) {
        const cell = document.createElement('div');
        Object.assign(cell.style, { minWidth: r < ROWS.length ? '30px' : '62px', padding: '5px 4px', textAlign: 'center', borderRadius: '3px', border: '1px solid rgba(255, 255, 255, 0.25)' });
        line.appendChild(cell);
        list.push(cell);
      }
      panel.appendChild(line);
      cells.push(list);
    }
    const hint = document.createElement('div');
    hint.textContent = 'A: type   X: delete   B: close';
    Object.assign(hint.style, { marginTop: '6px', fontSize: '12px', opacity: '0.7', textAlign: 'center' });
    panel.appendChild(hint);
  };
  const paint = () => {
    cells.forEach((list, r) => list.forEach((cell, c) => {
      cell.textContent = label(r, c);
      const on = r === row && c === col;
      cell.style.background = on ? '#fff36a' : 'rgba(255, 255, 255, 0.08)';
      cell.style.color = on ? '#000' : '#fff';
    }));
  };
  build();
  const kb: PadKeyboard = {
    get open() { return open; },
    show() { if (open) return; open = true; row = 1; col = 0; shift = true; panel.style.display = 'block'; paint(); },
    hide() { if (!open) return; open = false; panel.style.display = 'none'; onHide(); },
    key(code, down) {
      if (!open) return false;
      const nav: Record<string, [number, number]> = { ArrowUp: [-1, 0], ArrowDown: [1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1] };
      if (nav[code]) {
        if (down) {
          const [dr, dc] = nav[code];
          if (dr) {
            const was = width(row);
            row = (row + dr + ROWS.length + 1) % (ROWS.length + 1);
            col = Math.min(width(row) - 1, Math.round(col * (width(row) - 1) / Math.max(1, was - 1)));
          } else col = (col + dc + width(row)) % width(row);
          paint();
        }
        return true;
      }
      if (code === 'Escape') { if (down) kb.hide(); return true; }
      if (code === 'Backspace') { if (down) send.key('Backspace'); return true; }
      if (code === 'Enter' || code === 'NumpadEnter') {
        if (!down) return true;
        if (row < ROWS.length) {
          send.char(label(row, col));
          if (shift && /[a-z]/i.test(ROWS[row][col])) { shift = false; paint(); }
        } else {
          const s = SPECIAL[col];
          if (s === 'Shift') { shift = !shift; paint(); }
          else if (s === 'Space') send.char(' ');
          else if (s === 'Delete') send.key('Backspace');
          else if (s === 'Enter') { kb.hide(); send.key('Enter'); }
          else kb.hide();
        }
        return true;
      }
      return code === 'Tab';
    },
  };
  return kb;
}
