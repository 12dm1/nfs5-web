// nfs5-web sound in the browser: the AudioWorklet mixes the game's DirectSound buffers itself, straight from guest memory
// (runtime/pu_com.c publishes the AUD2 table: magic, version, seq, n, muted, ds_level, bufs[256]).
//  - A streamed buffer (looping, written in parts: the game's whole mix goes through one 32 kHz buffer) is read just
//    behind the game's last write (BUF_WHEAD, set at each Unlock). What is heard lags what the game wrote by the margin
//    its writer's timing needs (a few ms, kept by a slow rate trim), not by the game's own write-ahead of 50-300 ms
//    (sub_0058F8F0 keeps that lead and raises it after every stall) plus a ring.
//  - Any other playing buffer is read at its play cursor on the host clock, as the C mixer did (pu_audio_render).
// The old layout (AUD1: a ring the C thread fills, ?env=NFS5_AUDIOMIX=ring) is still played as before.
declare class AudioWorkletProcessor { port: MessagePort; constructor(); }
declare function registerProcessor(name: string, c: unknown): void;
declare const sampleRate: number;
declare const currentTime: number;

const AUD1 = 0x41554431, AUD2 = 0x41554432;
// DSBUF fields (pu_com.c BUF_*), byte offsets from the object
const B_SIZE = 4, B_DATA = 8, B_FLAGS = 12, B_BPS = 16, B_ALIGN = 20, B_STATE = 24, B_POS = 28, B_T0LO = 32, B_T0HI = 36,
  B_FMT = 44, B_VOL = 68, B_PAN = 72, B_WHEAD = 80, B_WCNT = 84;
const SBUF_MAX = 256;
const SAFE_MS = 4;          // the least margin to keep before the game's next write (2 ms gave a short underrun every few seconds in races)
const START_MS = 10;        // margin to start with, and after running into the writer
const WAKE_MS = 14;
const EXTRA_MAX_MS = 60;    // the most margin a run of stalls (a loading screen, the menus loading a car) can add
const CALM_S = 3;           // no underrun for this long: the added margin goes back, 3 ms a second (the rate trim follows at 5)         // below this margin over the least one (more than a burst: Chrome renders ~4 quanta at once), the game's sound thread is woken to write more

const dbGain = (v: number) => v >= 0 ? 1 : v <= -10000 ? 0 : Math.pow(10, v / 2000);

/** what the reader keeps per streamed buffer */
interface Stream {
  data: number; size: number; ms: number;   // source frames per ms
  rd: number;                               // read position, source frames (fractional)
  holding: boolean;                         // silent until the margin is back (start, after running into the writer)
  adj: number;                              // rate trim, +-0.5 %
  extra: number;                            // margin added after underruns, source frames
  minFill: number; win: number;
  quiet: number;                            // host us where the current silence began (-1: playing)
  why: string;
  lastUnder: number;                        // context time of the last underrun
  fadeIn: boolean;                          // the first quantum after a silence ramps up (no click)
}
interface Fmt { data: number; al: number; ch: number; bits: number; nfr: number }

class Mixer extends AudioWorkletProcessor {
  mode = 0;
  // AUD1 ring
  head: Uint32Array | null = null; ring: Float32Array | null = null; cap = 0;
  // AUD2 table + guest memory
  u32: Uint32Array | null = null; i32: Int32Array | null = null; u16: Uint16Array | null = null; i16: Int16Array | null = null; u8: Uint8Array | null = null;
  tab = 0; list: number[] = [];
  streams = new Map<number, Stream>();
  hostUs = 0; ctxAt = 0; st = -1;            // host clock: hostUs at ctxAt (context seconds), from the page
  stat = false; tStat = 0;
  s = Mixer.stats();
  static stats() { return { quanta: 0, underruns: 0, skips: 0, least: Infinity, fillSum: 0, fillN: 0, cursorQuanta: 0, maxAdj: 0, wakes: 0, sq: 0, sqN: 0 }; }

  constructor() {
    super();
    this.port.onmessage = (e: MessageEvent) => {
      const m = e.data as { sab?: SharedArrayBuffer; ptr?: number; stat?: boolean; hostUs?: number; ctx?: number };
      if (m.sab && m.ptr !== undefined) {
        const magic = new Uint32Array(m.sab, m.ptr, 1)[0];
        if (magic === AUD1) {
          this.head = new Uint32Array(m.sab, m.ptr, 5);
          this.cap = this.head[1];
          this.ring = new Float32Array(m.sab, m.ptr + 20, this.cap * 2);
          this.mode = 1;
        } else if (magic === AUD2) {
          this.u32 = new Uint32Array(m.sab); this.i32 = new Int32Array(m.sab); this.u16 = new Uint16Array(m.sab);
          this.i16 = new Int16Array(m.sab); this.u8 = new Uint8Array(m.sab);
          this.tab = m.ptr >>> 2;
          this.mode = 2;
        }
        this.stat = !!m.stat;
      }
      if (m.hostUs !== undefined && m.ctx !== undefined) { this.hostUs = m.hostUs; this.ctxAt = m.ctx; }
    };
  }

  process(_in: Float32Array[][], out: Float32Array[][]) {
    const l = out[0][0], r = out[0][1] ?? l;
    l.fill(0); if (r !== l) r.fill(0);
    if (this.mode === 1) this.playRing(l, r);
    else if (this.mode === 2) this.mix(l, r);
    return true;
  }

  playRing(l: Float32Array, r: Float32Array) {
    const head = this.head!, data = this.ring!;
    const wr = Atomics.load(head, 3); let rd = Atomics.load(head, 4);
    const have = (wr - rd) >>> 0, m = this.cap - 1;
    for (let i = 0; i < l.length && i < have; i++) { const k = rd & m; l[i] = data[k * 2]; r[i] = data[k * 2 + 1]; rd = (rd + 1) >>> 0; }
    Atomics.store(head, 4, rd);
  }

  /** the buffer list, copied while seq is even and unchanged (else last quantum's) */
  readList() {
    const u = this.u32!, t = this.tab;
    const s1 = Atomics.load(u, t + 2);
    if (s1 & 1) return;
    const n = Math.min(u[t + 3], SBUF_MAX), list: number[] = [];
    for (let i = 0; i < n; i++) list.push(u[t + 6 + i]);
    if (Atomics.load(u, t + 2) === s1) this.list = list;
  }

  /** one sample of channel c (0 left, 1 right) at source frame i */
  sample(f: Fmt, i: number, c: number) {
    const k = c && f.ch === 2 ? 1 : 0;
    return f.bits === 16 ? this.i16![(f.data + i * f.al + k * 2) >>> 1] / 32768 : (this.u8![f.data + i * f.al + k] - 128) / 128;
  }

  mix(l: Float32Array, r: Float32Array) {
    const u = this.u32!, t = this.tab, frames = l.length;
    this.readList();
    const muted = u[t + 4] !== 0, level = u[t + 5];
    // host time of this quantum's first frame, us (the play cursors' clock), followed gently as the C mixer did
    let now = -1;
    if (this.ctxAt) {
      now = this.hostUs + (currentTime - this.ctxAt) * 1e6;
      if (this.st < 0 || Math.abs(this.st - now) > 40000) this.st = now; else this.st += (now - this.st) * 0.003;
    }
    const dt = 1e6 / sampleRate;
    const live = new Set<number>();
    for (const b of this.list) {
      if (b & 3) continue;
      const w = b >>> 2;
      const st = u[w + B_STATE / 4], sz = u[w + B_SIZE / 4], data = u[w + B_DATA / 4];
      if (!(st & 1) || !sz || !data) continue;
      if ((u[w + B_FLAGS / 4] & 1) && level !== 3) continue;           // primary: only with WRITEPRIMARY
      const fmt = b + B_FMT;
      const ch = this.u16![(fmt + 2) >>> 1] === 2 ? 2 : 1, bits = this.u16![(fmt + 14) >>> 1] === 8 ? 8 : 16;
      const al = u[w + B_ALIGN / 4] || ch * bits / 8, bps = u[w + B_BPS / 4];
      const nfr = Math.floor(sz / al);
      if (!nfr || !bps || (bits === 16 && ((data | al) & 1))) continue;
      const f: Fmt = { data, al, ch, bits, nfr };
      const fps = bps / al;
      const g = dbGain(u[w + B_VOL / 4] | 0), pan = u[w + B_PAN / 4] | 0;
      let gl = g * (pan > 0 ? dbGain(-pan) : 1), gr = g * (pan < 0 ? dbGain(pan) : 1);
      if (muted) { gl = 0; gr = 0; }
      const loop = (st & 2) !== 0;
      if (loop && u[w + B_WCNT / 4] >= 4) { live.add(b); this.stream(b, f, fps, gl, gr, l, r); continue; }
      // a buffer the game does not stream into: at its play cursor
      if (now < 0 || (gl <= 0 && gr <= 0)) continue;
      this.s.cursorQuanta++;
      let hi = u[w + B_T0HI / 4], lo = u[w + B_T0LO / 4];
      if (hi !== u[w + B_T0HI / 4]) { hi = u[w + B_T0HI / 4]; lo = u[w + B_T0LO / 4]; }
      const t0 = hi * 4294967296 + lo, pos0 = u[w + B_POS / 4] / al;
      for (let i = 0; i < frames; i++) {
        const tt = this.st + i * dt;
        if (tt < t0) continue;
        let x = (tt - t0) * 1e-6 * fps + pos0;
        if (!loop && x >= nfr - 1) break;
        x %= nfr;
        const i0 = Math.floor(x), i1 = (i0 + 1) % nfr, w1 = x - i0;
        const l0 = this.sample(f, i0, 0), l1 = this.sample(f, i1, 0), r0 = this.sample(f, i0, 1), r1 = this.sample(f, i1, 1);
        l[i] += gl * (l0 + (l1 - l0) * w1);
        r[i] += gr * (r0 + (r1 - r0) * w1);
      }
    }
    for (const k of this.streams.keys()) if (!live.has(k)) this.streams.delete(k);
    if (this.st >= 0) this.st += frames * dt;
    for (let i = 0; i < frames; i++) {
      l[i] = l[i] > 1 ? 1 : l[i] < -1 ? -1 : l[i];
      if (r !== l) r[i] = r[i] > 1 ? 1 : r[i] < -1 ? -1 : r[i];
    }
    this.s.quanta++;
    if (this.stat) { for (let i = 0; i < frames; i++) this.s.sq += l[i] * l[i] + r[i] * r[i]; this.s.sqN += 2 * frames; }
    if (this.stat && currentTime - this.tStat >= 10) { this.tStat = currentTime; this.report(); }
  }

  /** a streamed buffer, read behind the game's last write */
  stream(b: number, f: Fmt, fps: number, gl: number, gr: number, l: Float32Array, r: Float32Array) {
    const u = this.u32!, w = b >>> 2, frames = l.length, nfr = f.nfr, ms = fps / 1000;
    const headF = Atomics.load(u, w + B_WHEAD / 4) / f.al;
    const wrap = (x: number) => ((x % nfr) + nfr) % nfr;
    let v = this.streams.get(b);
    if (!v || v.data !== f.data || v.size !== nfr * f.al || v.ms !== ms) {
      v = { data: f.data, size: nfr * f.al, ms, rd: wrap(headF - START_MS * ms), holding: true, adj: 0, extra: 0, minFill: Infinity, win: 0, quiet: this.st, why: 'start', lastUnder: -1e9, fadeIn: true };
      this.streams.set(b, v);
    }
    const target = SAFE_MS * ms + v.extra;
    let fill = wrap(headF - v.rd);
    if (fill > nfr / 2) fill -= nfr;                       // past the writer
    const step = fps / sampleRate * (1 + v.adj), need = frames * step;
    if (fill - need < target + WAKE_MS * ms) this.wake();
    if (v.holding) {
      if (fill < target + START_MS * ms) return;           // wait for a margin again
      v.holding = false;
      // the game's writer stalled for about as long as this silence: keep that much more in hand, so the next stall
      // of a run (a load reads and decodes for seconds) is heard as nothing
      if (v.why !== 'start' && v.quiet >= 0) v.extra = Math.min(v.extra + (this.st - v.quiet) / 1000 * ms, EXTRA_MAX_MS * ms);
      this.heard(v, fill / ms);
      v.fadeIn = true;
    }
    if (fill - need < 1) {                                 // this quantum would read what the game has not written yet
      this.s.underruns++;
      // what there is, faded out: a cut to silence clicks
      let rd = v.rd;
      const k = fill > 1 ? Math.min(frames, Math.floor((fill - 1) / step)) : 0;
      if (gl > 0 || gr > 0) for (let i = 0; i < k; i++) {
        const i0 = Math.floor(rd), i1 = i0 + 1 >= nfr ? 0 : i0 + 1, x = rd - i0, g = 1 - i / k;
        const l0 = this.sample(f, i0, 0), l1 = this.sample(f, i1, 0), r0 = this.sample(f, i0, 1), r1 = this.sample(f, i1, 1);
        l[i] += g * gl * (l0 + (l1 - l0) * x);
        r[i] += g * gr * (r0 + (r1 - r0) * x);
        rd += step; if (rd >= nfr) rd -= nfr;
      }
      if (k > 0) v.rd = gl > 0 || gr > 0 ? rd : wrap(v.rd + k * step);
      v.extra = Math.min(v.extra + 3 * ms, EXTRA_MAX_MS * ms);
      v.lastUnder = currentTime;
      v.holding = true;
      v.quiet = this.st; v.why = fill < 0 ? `past the writer by ${(-fill / ms).toFixed(1)} ms` : 'ran into the writer';
      return;
    }
    if (fill > target + 60 * ms) {                         // far behind (the game wrote a long stretch at once): catch up
      this.s.skips++;
      v.rd = wrap(headF - target - START_MS * ms);
      fill = target + START_MS * ms;
    }
    let rd = v.rd;
    const fade = v.fadeIn; v.fadeIn = false;
    if (gl > 0 || gr > 0) {
      for (let i = 0; i < frames; i++) {
        const i0 = Math.floor(rd), i1 = i0 + 1 >= nfr ? 0 : i0 + 1, x = rd - i0, g = fade ? i / frames : 1;
        const l0 = this.sample(f, i0, 0), l1 = this.sample(f, i1, 0), r0 = this.sample(f, i0, 1), r1 = this.sample(f, i1, 1);
        l[i] += g * gl * (l0 + (l1 - l0) * x);
        r[i] += g * gr * (r0 + (r1 - r0) * x);
        rd += step; if (rd >= nfr) rd -= nfr;
      }
    } else { rd += need; if (rd >= nfr) rd -= nfr; }
    v.rd = rd;
    const left = fill - need;
    if (left < v.minFill) v.minFill = left;
    this.s.fillSum += fill / ms; this.s.fillN++;
    if (left / ms < this.s.least) this.s.least = left / ms;
    // every half second: trim the rate so the least margin comes to the target (+-0.5 %: 9 cents at most)
    v.win += frames;
    if (v.win >= sampleRate / 2) {
      const err = v.minFill - target;
      v.adj = Math.max(-0.005, Math.min(0.005, err / (fps / 2)));
      if (Math.abs(v.adj) > this.s.maxAdj) this.s.maxAdj = Math.abs(v.adj);
      v.extra = Math.max(0, v.extra - (currentTime - v.lastUnder > CALM_S ? 1.5 : 0.1) * ms);   // calm: 3 ms a second, else 1 ms every 5 s
      v.minFill = Infinity; v.win = 0;
    }
  }

  /** the stream plays again after a silence: AUDIOSTAT logs the long ones (host time, as the game's [audiostat] lines) */
  heard(v: Stream, fill: number) {
    const d = (this.st - v.quiet) / 1000;
    if (this.stat && v.quiet >= 0 && d >= 15) this.port.postMessage({ stat: `silent ${d.toFixed(1)} ms (${v.why}), back at t=${((this.st % 1e9) / 1e6).toFixed(3)} with ${fill.toFixed(1)} ms` });
    v.quiet = -1;
  }

  /** wakes the game's sound thread (its Sleep waits on this word: runtime/win32_ext.c, pu_audio_wake) */
  wake() {
    const k = this.tab + 6 + SBUF_MAX;
    Atomics.add(this.i32!, k, 1); Atomics.notify(this.i32!, k);
    this.s.wakes++;
  }

  report() {
    const s = this.s, v = [...this.streams.values()][0];
    this.port.postMessage({
      stat: `quanta ${s.quanta}, rms ${Math.sqrt(s.sq / Math.max(1, s.sqN)).toFixed(3)}, stream: avg margin ${(s.fillSum / Math.max(1, s.fillN)).toFixed(1)} ms, least ${s.least === Infinity ? '-' : s.least.toFixed(1)} ms` +
        `, underruns ${s.underruns}, catch-ups ${s.skips}, max trim ${(s.maxAdj * 100).toFixed(2)} %` +
        (v ? `, extra margin ${(v.extra / v.ms).toFixed(1)} ms` : ', no stream') + `, wakes ${s.wakes}, cursor-buffer quanta ${s.cursorQuanta}`,
    });
    this.s = Mixer.stats();
  }
}
registerProcessor('nfs5-ring', Mixer);
