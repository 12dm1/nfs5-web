// CLOUD1: a small zip writer and reader for cloud saves and save downloads, and the saves' content hash.
// After nfsu2-web's web/src/app/zip.ts (MIT, (c) 2026 th0mk): the reader is that one, the writer here deflates.
// Flat archives, UTF-8 names (flag bit 11), no zip64. The server checks every upload again (web/cloud/zipcheck.mjs).

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
export function crc32(b: Uint8Array): number {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

export interface ZipEntry { name: string; data: Uint8Array; mtime?: number }

function dosTime(ms: number): { time: number; date: number } {
  const d = new Date(ms);
  const y = Math.min(2107, Math.max(1980, d.getFullYear()));
  return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
           date: ((y - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
}

async function streamBytes(s: ReadableStream<Uint8Array>, max = Infinity): Promise<Uint8Array> {
  const rd = s.getReader(), parts: Uint8Array[] = [];
  let n = 0;
  for (;;) {
    const { done, value } = await rd.read();
    if (done) break;
    n += value.length;
    if (n > max) { void rd.cancel().catch(() => {}); throw new ZipError('larger than it says'); }
    parts.push(value);
  }
  const out = new Uint8Array(n);
  let p = 0;
  for (const x of parts) { out.set(x, p); p += x.length; }
  return out;
}
const deflate = (b: Uint8Array) => streamBytes(new Blob([b as BlobPart]).stream().pipeThrough(new CompressionStream('deflate-raw')));

/** The files as a zip; each deflated where that makes it smaller. */
export async function zip(entries: ZipEntry[]): Promise<Uint8Array> {
  const te = new TextEncoder();
  const locals: Uint8Array[] = [], centrals: Uint8Array[] = [];
  let off = 0;
  for (const e of entries) {
    const name = te.encode(e.name), crc = crc32(e.data), { time, date } = dosTime(e.mtime ?? Date.now());
    const packed = e.data.length > 64 ? await deflate(e.data) : null;
    const method = packed && packed.length < e.data.length ? 8 : 0, body = method === 8 ? packed! : e.data;
    const lh = new Uint8Array(30 + name.length), lv = new DataView(lh.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true); lv.setUint16(6, 0x0800, true); lv.setUint16(8, method, true);
    lv.setUint16(10, time, true); lv.setUint16(12, date, true); lv.setUint32(14, crc, true);
    lv.setUint32(18, body.length, true); lv.setUint32(22, e.data.length, true); lv.setUint16(26, name.length, true);
    lh.set(name, 30);
    const ch = new Uint8Array(46 + name.length), cv = new DataView(ch.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint16(4, 0x031E, true); cv.setUint16(6, 20, true); cv.setUint16(8, 0x0800, true);
    cv.setUint16(10, method, true); cv.setUint16(12, time, true); cv.setUint16(14, date, true); cv.setUint32(16, crc, true);
    cv.setUint32(20, body.length, true); cv.setUint32(24, e.data.length, true); cv.setUint16(28, name.length, true);
    cv.setUint32(38, (0o100644 << 16) >>> 0, true); cv.setUint32(42, off, true);
    ch.set(name, 46);
    locals.push(lh, body); centrals.push(ch);
    off += lh.length + body.length;
  }
  const cdSize = centrals.reduce((a, c) => a + c.length, 0);
  const end = new Uint8Array(22), ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, entries.length, true); ev.setUint16(10, entries.length, true);
  ev.setUint32(12, cdSize, true); ev.setUint32(16, off, true);
  const out = new Uint8Array(off + cdSize + end.length);
  let p = 0;
  for (const b of [...locals, ...centrals, end]) { out.set(b, p); p += b.length; }
  return out;
}

export class ZipError extends Error {}

/** A deflated entry, inflated no further than its declared size (a zip bomb stops there). */
async function inflate(raw: Uint8Array, usize: number, name: string): Promise<Uint8Array> {
  try { return await streamBytes(new Blob([raw as BlobPart]).stream().pipeThrough(new DecompressionStream('deflate-raw')), usize); }
  catch (e) { throw new ZipError(`${name}: ${e instanceof Error ? e.message : e}`); }
}

/** The file entries of a zip (folders skipped; a file in a folder as its own name: "SaveData/x.sav" -> "x.sav").
 *  Checks every CRC; accept(name, size) sees each entry's declared size before it is inflated. */
export async function unzip(buf: Uint8Array, accept: (name: string, size: number) => boolean = () => true): Promise<ZipEntry[]> {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 65535); i--) if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new ZipError('not a zip file');
  const n = v.getUint16(eocd + 10, true);
  let p = v.getUint32(eocd + 16, true);
  const td = new TextDecoder();
  const out: ZipEntry[] = [];
  for (let i = 0; i < n; i++) {
    if (p + 46 > buf.length || v.getUint32(p, true) !== 0x02014b50) throw new ZipError('damaged zip directory');
    const method = v.getUint16(p + 10, true), crc = v.getUint32(p + 16, true);
    const csize = v.getUint32(p + 20, true), usize = v.getUint32(p + 24, true);
    const nl = v.getUint16(p + 28, true), xl = v.getUint16(p + 30, true), cl = v.getUint16(p + 32, true);
    const lo = v.getUint32(p + 42, true);
    const tm = v.getUint16(p + 12, true), dt = v.getUint16(p + 14, true);
    const full = td.decode(buf.subarray(p + 46, p + 46 + nl));
    p += 46 + nl + xl + cl;
    if (full.endsWith('/')) continue;
    const name = full.split(/[\\/]/).pop()!;
    if (!accept(name, usize)) continue;
    if (lo + 30 > buf.length || v.getUint32(lo, true) !== 0x04034b50) throw new ZipError(`damaged entry ${name}`);
    const ds = lo + 30 + v.getUint16(lo + 26, true) + v.getUint16(lo + 28, true);
    if (ds + csize > buf.length) throw new ZipError(`truncated entry ${name}`);
    const raw = buf.subarray(ds, ds + csize);
    let data: Uint8Array;
    if (method === 0) { if (csize !== usize) throw new ZipError(`damaged entry ${name}`); data = raw.slice(); }
    else if (method === 8) data = usize ? await inflate(raw, usize, name) : new Uint8Array(0);
    else throw new ZipError(`entry ${name} uses compression method ${method}`);
    if (data.length !== usize || crc32(data) !== crc) throw new ZipError(`entry ${name} is damaged (CRC)`);
    const mtime = new Date(1980 + (dt >> 9), ((dt >> 5) & 15) - 1, dt & 31, tm >> 11, (tm >> 5) & 63, (tm & 31) * 2).getTime();
    out.push({ name, data, mtime });
  }
  return out;
}

/** SHA-256 hex of the files, whatever zip they come in: sorted by UTF-8 name bytes, each as name, 0, size (u32 LE),
 *  data. The server computes the same (web/cloud/zipcheck.mjs contentHash). */
export async function contentHash(entries: ZipEntry[]): Promise<string> {
  const te = new TextEncoder();
  const items = entries.map(e => ({ n: te.encode(e.name), d: e.data }));
  items.sort((a, b) => {
    for (let i = 0; i < Math.min(a.n.length, b.n.length); i++) if (a.n[i] !== b.n[i]) return a.n[i] - b.n[i];
    return a.n.length - b.n.length;
  });
  const total = items.reduce((s, x) => s + x.n.length + 5 + x.d.length, 0), buf = new Uint8Array(total), dv = new DataView(buf.buffer);
  let p = 0;
  for (const { n, d } of items) { buf.set(n, p); p += n.length; buf[p++] = 0; dv.setUint32(p, d.length, true); p += 4; buf.set(d, p); p += d.length; }
  const h = new Uint8Array(await crypto.subtle.digest('SHA-256', buf));
  return Array.from(h, b => b.toString(16).padStart(2, '0')).join('');
}
