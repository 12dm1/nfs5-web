// nfs5-web WEB3: plays the ring runtime/web/audio_web.c fills. Layout: magic, cap, rate, wr, rd (u32), then floats.
declare class AudioWorkletProcessor { port: MessagePort; constructor(); }
declare function registerProcessor(name: string, c: unknown): void;

class Ring extends AudioWorkletProcessor {
  head: Uint32Array | null = null; data: Float32Array | null = null; cap = 0;
  constructor() {
    super();
    this.port.onmessage = (e: MessageEvent<{ sab: SharedArrayBuffer; ptr: number }>) => {
      const { sab, ptr } = e.data;
      this.head = new Uint32Array(sab, ptr, 5);
      this.cap = this.head[1];
      this.data = new Float32Array(sab, ptr + 20, this.cap * 2);
    };
  }
  process(_in: Float32Array[][], out: Float32Array[][]) {
    const l = out[0][0], r = out[0][1] ?? l;
    if (!this.head || !this.data) return true;
    const wr = Atomics.load(this.head, 3); let rd = Atomics.load(this.head, 4);
    const have = (wr - rd) >>> 0, m = this.cap - 1;
    for (let i = 0; i < l.length; i++) {
      if (i < have) { const k = rd & m; l[i] = this.data[k * 2]; r[i] = this.data[k * 2 + 1]; rd = (rd + 1) >>> 0; }
      else { l[i] = 0; r[i] = 0; }
    }
    Atomics.store(this.head, 4, rd);
    return true;
  }
}
registerProcessor('nfs5-ring', Ring);
