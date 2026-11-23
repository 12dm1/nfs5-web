// CLOUD1: the server's check of an uploaded save archive, and the archive's content hash.
//
// A cloud save is a zip of the files of the browser's SaveData folder (web/src/cloud/zip.ts writes it: flat, UTF-8
// names, stored or deflated). The server stores the zip as it came, after checking all of it here:
//   - the end record, the central directory and every local header are inside the buffer and agree on the name
//   - at most MAX_ENTRIES files, no folders, no duplicate names (in any letter case), names a save folder can hold
//   - stored (0) or deflated (8) only, no encryption, sizes declared and held: an entry is inflated with Node's
//     maxOutputLength at its declared size (a zip bomb stops there), and its CRC must match
//   - at most MAX_ENTRY per file and MAX_TOTAL for all of them
// contentHash() is what tells two versions apart (the same files in any zip give the same hash): SHA-256 over the
// entries sorted by their UTF-8 name bytes, each as name, 0, size (u32 LE), data. web/src/cloud/zip.ts computes the
// same on the page.
import crypto from 'node:crypto';
import zlib from 'node:zlib';

export const MAX_ZIP = 960 << 10;          // under nginx's default client_max_body_size (1 MB)
export const MAX_ENTRIES = 64;
export const MAX_ENTRY = 1 << 20;
export const MAX_TOTAL = 8 << 20;

export class ZipCheckError extends Error {}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
const crc32 = zlib.crc32 ? (b => zlib.crc32(b) >>> 0) : (b => {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
});

/** A file name the save folder may hold: one level, no control characters, none of \ / : * ? " < > |, not "." or
 *  "..", at most 64 characters. (Profile names reach the browser's storage as U+FFFD: allowed.) */
export function validSaveName(name) {
  return typeof name === 'string' && name.length >= 1 && name.length <= 64 && name !== '.' && name !== '..' &&
    !/[\u0000-\u001f\u007f\\/:*?"<>|]/.test(name) && !/^\s|\s$/.test(name);
}

const utf8 = new TextDecoder('utf-8', { fatal: true });

/** The files of a zip as [{ name, data }], or throws ZipCheckError. */
export function checkZip(buf) {
  const fail = why => { throw new ZipCheckError(why); };
  if (!Buffer.isBuffer(buf)) buf = Buffer.from(buf);
  if (buf.length < 22) fail('not a zip');
  if (buf.length > MAX_ZIP) fail('too big');
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 65535); i--) {
    if (buf.readUInt32LE(i) === 0x06054b50 && i + 22 + buf.readUInt16LE(i + 20) === buf.length) { eocd = i; break; }
  }
  if (eocd < 0) fail('not a zip');
  if (buf.readUInt16LE(eocd + 4) !== 0 || buf.readUInt16LE(eocd + 6) !== 0) fail('multi-disk zip');
  const count = buf.readUInt16LE(eocd + 10), cdSize = buf.readUInt32LE(eocd + 12), cdOff = buf.readUInt32LE(eocd + 16);
  if (count !== buf.readUInt16LE(eocd + 8)) fail('damaged end record');
  if (count > MAX_ENTRIES) fail('too many files');
  if (cdOff + cdSize > eocd) fail('damaged directory');
  const out = [], seen = new Set();
  let p = cdOff, total = 0;
  for (let i = 0; i < count; i++) {
    if (p + 46 > cdOff + cdSize || buf.readUInt32LE(p) !== 0x02014b50) fail('damaged directory');
    const flags = buf.readUInt16LE(p + 8), method = buf.readUInt16LE(p + 10), crc = buf.readUInt32LE(p + 16);
    const csize = buf.readUInt32LE(p + 20), usize = buf.readUInt32LE(p + 24);
    const nl = buf.readUInt16LE(p + 28), xl = buf.readUInt16LE(p + 30), cl = buf.readUInt16LE(p + 32);
    const lo = buf.readUInt32LE(p + 42);
    if (p + 46 + nl + xl + cl > cdOff + cdSize) fail('damaged directory');
    const nameBytes = buf.subarray(p + 46, p + 46 + nl);
    p += 46 + nl + xl + cl;
    if (flags & 1) fail('encrypted entry');
    if (method !== 0 && method !== 8) fail('unsupported compression');
    let name;
    try { name = utf8.decode(nameBytes); } catch { fail('bad file name'); }
    if (!(flags & 0x800) && /[^\x20-\x7e]/.test(name)) fail('bad file name');
    if (!validSaveName(name)) fail('bad file name');
    const lower = name.toLowerCase();
    if (seen.has(lower)) fail('duplicate file');
    seen.add(lower);
    if (usize > MAX_ENTRY) fail('file too big');
    total += usize;
    if (total > MAX_TOTAL) fail('too big');
    if (lo + 30 > cdOff || buf.readUInt32LE(lo) !== 0x04034b50) fail('damaged entry');
    const lnl = buf.readUInt16LE(lo + 26), lxl = buf.readUInt16LE(lo + 28);
    if (lnl !== nl || !buf.subarray(lo + 30, lo + 30 + lnl).equals(nameBytes)) fail('damaged entry');
    const ds = lo + 30 + lnl + lxl;
    if (ds + csize > cdOff) fail('damaged entry');
    const raw = buf.subarray(ds, ds + csize);
    let data;
    if (method === 0) {
      if (csize !== usize) fail('damaged entry');
      data = Buffer.from(raw);
    } else if (usize === 0) {
      data = Buffer.alloc(0);
    } else {
      try { data = zlib.inflateRawSync(raw, { maxOutputLength: usize }); }
      catch { fail('damaged entry'); }
      if (data.length !== usize) fail('damaged entry');
    }
    if (crc32(data) !== crc) fail('damaged entry');
    out.push({ name, data });
  }
  return out;
}

/** SHA-256 hex of the files, independent of the zip they came in (see the head of this file). */
export function contentHash(entries) {
  const items = entries.map(e => ({ n: Buffer.from(e.name, 'utf8'), d: e.data })).sort((a, b) => Buffer.compare(a.n, b.n));
  const h = crypto.createHash('sha256');
  for (const { n, d } of items) {
    const len = Buffer.alloc(4); len.writeUInt32LE(d.length, 0);
    h.update(n); h.update(Buffer.from([0])); h.update(len); h.update(d);
  }
  return h.digest('hex');
}

/** Career profiles in a save: the .sav files other than the game's default one. */
export const profileCount = entries => entries.filter(e => /\.sav$/i.test(e.name) && !/^xxdefxx\.sav$/i.test(e.name)).length;
