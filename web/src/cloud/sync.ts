// CLOUD1: cloud saves on the game page. docs/tasks/CLOUD1-cloud-saves.md "Sync" has the rules; in short:
//
// What syncs: the files of the save folder's SaveData (the career profiles, records, player.lst). The rest of the save
// folder (registry.txt: this device's settings, the key bindings, logs) stays on the device.
//
// Before the game starts (beforeGame): the account's save and the one here are compared with what this browser last
// synced (its state: the revision and the content hash, in OPFS cloud/state.json):
//   - same content: nothing to do
//   - only the cloud's changed: it replaces the saves here
//   - only these changed (played offline, or the upload did not finish): they are sent
//   - both changed: the newer one wins (the player's choice, 2026-11-23). What loses is kept: the saves here as a
//     local backup (OPFS cloud/backup, the newest MAX_LOCAL_BACKUPS), the cloud's as a cloud backup; both can be
//     downloaded on the start page
//   - saves here with no profile never replace a cloud save that has one
// While the game runs: every save the game writes (the I/O worker's "saved"/"removed") is sent a few seconds later; a
// conflict then means another device sent meanwhile, and the save made just now is the newer one.
// The game must not run while the saves here are replaced: the page holds the Web Lock "nfs5-game" from before that
// until it closes; a second tab with the game does not sync at all.
import { api, CloudError, type Me } from './api';
import { contentHash, unzip, zip, type ZipEntry } from './zip';

export const SAVE_SUB = 'SaveData';
const CLOUD_DIR = 'cloud', BACKUP_DIR = 'backup', STATE_FILE = 'state.json';
const MAX_LOCAL_BACKUPS = 5;
const PUSH_DELAY_MS = 3000;
const MAX_ZIP = 960 << 10;            // the server's (web/cloud/zipcheck.mjs)
const MAX_FILE = 1 << 20;

interface State { user: string; rev: number | null; hash: string | null }

async function opfs(): Promise<FileSystemDirectoryHandle> { return navigator.storage.getDirectory(); }
async function cloudDir(sub?: string): Promise<FileSystemDirectoryHandle> {
  const d = await (await opfs()).getDirectoryHandle(CLOUD_DIR, { create: true });
  return sub ? d.getDirectoryHandle(sub, { create: true }) : d;
}
async function saveData(saveDir: string, create: boolean): Promise<FileSystemDirectoryHandle | null> {
  try { return await (await (await opfs()).getDirectoryHandle(saveDir, { create })).getDirectoryHandle(SAVE_SUB, { create }); }
  catch (e) { if ((e as DOMException)?.name === 'NotFoundError') return null; throw e; }
}
type Iterable = { entries(): AsyncIterable<[string, FileSystemHandle]> };

async function writeFile(d: FileSystemDirectoryHandle, name: string, data: Uint8Array) {
  const w = await (await d.getFileHandle(name, { create: true })).createWritable();
  try { await w.write(data as FileSystemWriteChunkType); await w.close(); }
  catch (e) { await w.abort().catch(() => {}); throw e; }
}
async function readFile(d: FileSystemDirectoryHandle, name: string): Promise<Uint8Array | null> {
  try { return new Uint8Array(await (await (await d.getFileHandle(name)).getFile()).arrayBuffer()); }
  catch (e) { if ((e as DOMException)?.name === 'NotFoundError') return null; throw e; }
}

/** the files of SaveData here (none when there is no such folder) */
export async function readLocal(saveDir: string): Promise<ZipEntry[]> {
  const d = await saveData(saveDir, false);
  if (!d) return [];
  const out: ZipEntry[] = [];
  for await (const [name, h] of (d as unknown as Iterable).entries()) {
    if (h.kind !== 'file') continue;
    const f = await (h as FileSystemFileHandle).getFile();
    out.push({ name, data: new Uint8Array(await f.arrayBuffer()), mtime: f.lastModified });
  }
  return out;
}
/** SaveData here becomes exactly these files (only while no game runs: withGameLock / holdGameLock) */
export async function restoreLocal(saveDir: string, files: ZipEntry[]) { await writeLocal(saveDir, files); }
async function writeLocal(saveDir: string, files: ZipEntry[]) {
  const d = (await saveData(saveDir, true))!;
  const keep = new Set(files.map(f => f.name));
  for (const f of files) await writeFile(d, f.name, f.data);
  const gone: string[] = [];
  for await (const [name, h] of (d as unknown as Iterable).entries()) if (h.kind === 'file' && !keep.has(name)) gone.push(name);
  for (const name of gone) await d.removeEntry(name);
}

export const profilesIn = (files: { name: string }[]) => files.filter(f => /\.sav$/i.test(f.name) && !/^xxdefxx\.sav$/i.test(f.name)).length;

// ---- local backups: zips in OPFS cloud/backup, named by time
export interface LocalBackup { name: string; at: number; size: number }
export async function keepLocalBackup(files: ZipEntry[], why: string): Promise<void> {
  if (!files.length) return;
  const d = await cloudDir(BACKUP_DIR);
  await writeFile(d, `${Date.now()}-${why}.zip`, await zip(files));
  const list = await localBackups();
  for (const b of list.slice(MAX_LOCAL_BACKUPS)) await d.removeEntry(b.name).catch(() => {});
}
export async function localBackups(): Promise<LocalBackup[]> {
  const d = await cloudDir(BACKUP_DIR), out: LocalBackup[] = [];
  for await (const [name, h] of (d as unknown as Iterable).entries()) {
    const m = /^(\d+)-[a-z-]+\.zip$/.exec(name);
    if (h.kind === 'file' && m) out.push({ name, at: +m[1], size: (await (h as FileSystemFileHandle).getFile()).size });
  }
  return out.sort((a, b) => b.at - a.at);
}
export async function readLocalBackup(name: string): Promise<Uint8Array | null> { return readFile(await cloudDir(BACKUP_DIR), name); }

// ---- the sync state of this browser
async function loadState(): Promise<State | null> {
  try {
    const b = await readFile(await cloudDir(), STATE_FILE);
    const s = b && JSON.parse(new TextDecoder().decode(b));
    return s && typeof s.user === 'string' ? s : null;
  } catch { return null; }
}
async function storeState(s: State) { await writeFile(await cloudDir(), STATE_FILE, new TextEncoder().encode(JSON.stringify(s))); }

/** A zip's files that may go into SaveData (flat names, sizes in reason) */
export async function filesOfZip(buf: Uint8Array): Promise<ZipEntry[]> {
  let total = 0;
  const ok = (name: string, size: number) => {
    total += size;
    return !!name && name !== '.' && name !== '..' && !/[\u0000-\u001f\\/:*?"<>|]/.test(name) && size <= MAX_FILE && total <= 8 * MAX_FILE;
  };
  return unzip(buf, ok);
}

/** Holds "nfs5-game" for the page's life: false when another tab already has the game (then this one does not sync). */
export function holdGameLock(): Promise<boolean> {
  if (!navigator.locks) return Promise.resolve(true);
  return new Promise(res => {
    void navigator.locks.request('nfs5-game', { ifAvailable: true }, lock => {
      res(!!lock);
      return lock ? new Promise(() => {}) : undefined;
    });
  });
}

/** fn() while no tab has the game (the lock held only for that), else false without calling it */
export function withGameLock(fn: () => Promise<void>): Promise<boolean> {
  if (!navigator.locks) return fn().then(() => true);
  return navigator.locks.request('nfs5-game', { ifAvailable: true }, async lock => { if (!lock) return false; await fn(); return true; });
}

export interface SyncDeps {
  saveDir: string;
  /** a line for the log */
  note(s: string): void;
  /** a line for the player (the status corner) */
  tell(s: string): void;
}

export class CloudSync {
  private timer: ReturnType<typeof setTimeout> | null = null;
  private running: Promise<void> | null = null;
  private again = false;
  private stopped = false;
  private failures = 0;
  private skew = 0;                         // server clock - this clock, ms
  /** beforeGame went through: these saves are what the cloud had, or newer (only then may a save overwrite) */
  synced = false;

  constructor(private user: string, private d: SyncDeps) {}

  /** Before the game: bring the saves here and the cloud's together (see the head of this file). */
  async beforeGame(me: Me): Promise<void> {
    this.skew = me.now - Date.now();
    const files = await readLocal(this.d.saveDir);
    const hash = files.length ? await contentHash(files) : null;
    let st = await loadState();
    if (!st || st.user !== this.user) st = { user: this.user, rev: null, hash: null };
    const remote = me.save ?? null;
    const localChanged = hash !== null && hash !== st.hash;
    this.d.note(`[cloud] ${this.user}: here ${files.length} files (${profilesIn(files)} profiles) ${hash?.slice(0, 12) ?? '-'}, ` +
      `cloud ${remote ? `rev ${remote.rev} ${remote.hash.slice(0, 12)} (${remote.profiles} profiles, ${remote.dev})` : 'none'}, last synced rev ${st.rev ?? '-'}`);
    if (!remote) {
      if (hash) await this.send(files, hash, null, 'first upload');
      else await storeState(st);
      return;
    }
    if (remote.hash === hash) { await storeState({ user: this.user, rev: remote.rev, hash }); return; }
    const remoteChanged = remote.rev !== st.rev;
    if (!remoteChanged && localChanged) { await this.send(files, hash!, remote.rev, 'changed here'); return; }
    if (remoteChanged && localChanged && profilesIn(files) > 0) {
      // both changed: the newer wins. Here: the newest file's time, on the server's clock
      if (this.madeAt(files) > (remote.made ?? remote.at)) {
        await this.send(files, hash!, null, 'newer here', true);
        this.d.tell(`Cloud saves: your saves here are newer than the cloud's from ${remote.dev}; that one is kept as a cloud backup`);
        return;
      }
    }
    // the cloud's version comes here; the saves here are kept as a local backup when they had anything of their own
    const z = await api.download();
    const theirs = await filesOfZip(z);
    if (!theirs.length) throw new Error('the cloud save is empty');
    if (files.length && localChanged) await keepLocalBackup(files, 'replaced');
    await writeLocal(this.d.saveDir, theirs);
    const got = await contentHash(theirs);
    await storeState({ user: this.user, rev: remote.rev, hash: got });
    this.d.note(`[cloud] took rev ${remote.rev} from the cloud (${theirs.length} files)${localChanged && files.length ? ', the saves here kept as a local backup' : ''}`);
    if (localChanged && files.length) this.d.tell(`Cloud saves: a newer career from ${remote.dev} replaced the one here; the old one is kept (start page, Account)`);
  }

  /** when these saves were last changed, on the server's clock */
  private madeAt(files: ZipEntry[]): number { return Math.max(0, ...files.map(f => f.mtime ?? 0)) + this.skew; }

  /** the game wrote or removed a file of SaveData */
  changed(): void {
    if (this.stopped) return;
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => { this.timer = null; void this.push(); }, PUSH_DELAY_MS);
  }
  /** send now what is waiting (the game ended) */
  async flush(): Promise<void> {
    if (this.timer) { clearTimeout(this.timer); this.timer = null; await this.push(); }
    else if (this.running) await this.running;
  }

  private push(): Promise<void> {
    if (this.running) { this.again = true; return this.running; }
    this.running = (async () => {
      do {
        this.again = false;
        try { await this.pushOnce(); this.failures = 0; }
        catch (e) {
          if (e instanceof CloudError && e.status === 401) {
            this.stopped = true;
            this.d.tell('Cloud saves are off: this device was signed out (password changed?). Sign in on the start page.');
            return;
          }
          this.failures++;
          const wait = Math.min(300, 5 * 3 ** Math.min(this.failures - 1, 4));
          this.d.note(`[cloud] upload failed (${e instanceof Error ? e.message : e}), again in ${wait} s`);
          if (this.failures === 2) this.d.tell('Cloud saves: cannot reach the server, trying again; your saves are safe here');
          if (!this.stopped) { if (this.timer) clearTimeout(this.timer); this.timer = setTimeout(() => { this.timer = null; void this.push(); }, wait * 1000); }
        }
      } while (this.again && !this.stopped);
    })().finally(() => { this.running = null; });
    return this.running;
  }
  private async pushOnce() {
    const files = await readLocal(this.d.saveDir);
    if (!files.length) return;
    const hash = await contentHash(files), st = await loadState();
    if (st && st.user === this.user && st.hash === hash) return;
    try { await this.send(files, hash, st && st.user === this.user ? st.rev : null, 'saved'); }
    catch (e) {
      if (!(e instanceof CloudError) || e.status !== 412) throw e;
      // Not synced before this game (the server did not answer then), or saves without a profile against a cloud
      // career: never overwrite. The next game start compares them properly.
      if (!this.synced || (profilesIn(files) === 0 && (e.save?.profiles ?? 0) > 0)) {
        this.stopped = true;
        this.d.note(`[cloud] the cloud has rev ${e.save?.rev} from ${e.save?.dev}; not synced before this game: not overwritten, syncing stops here`);
        this.d.tell('Cloud saves: the cloud has another version of your career. Your saves here are kept; start the game again to sync.');
        return;
      }
      // another device sent since: what the game saved just now is the newer one
      await this.send(files, hash, null, 'saved, newer than the cloud', true);
      this.d.tell(`Cloud saves: your save replaced a version from ${e.save?.dev ?? 'another device'}; that one is kept as a cloud backup`);
    }
  }
  private async send(files: ZipEntry[], hash: string, base: number | null, why: string, overwrite = false) {
    const z = await zip(files);
    if (z.length > MAX_ZIP) { this.stopped = true; this.d.tell('Cloud saves: your saves are too big for the cloud, they stay on this device'); return; }
    const me = await api.upload(z, { ifMatch: base, overwrite, madeAt: this.madeAt(files) });
    if (me.save && me.save.hash !== hash) this.d.note(`[cloud] the server's hash ${me.save.hash.slice(0, 12)} is not this page's ${hash.slice(0, 12)}`);
    await storeState({ user: this.user, rev: me.save?.rev ?? null, hash: me.save?.hash ?? hash });
    this.d.note(`[cloud] sent (${why}): rev ${me.save?.rev}, ${files.length} files, ${(z.length / 1024).toFixed(0)} KB`);
  }
}

/** On the game page: signed in? then sync before the game, and give back what to call on saves. Null: no syncing. */
export async function startCloud(d: SyncDeps): Promise<CloudSync | null> {
  let me: Me | null;
  try { me = await api.me(); }
  catch (e) { d.note(`[cloud] the server did not answer (${e instanceof Error ? e.message : e}): playing with the saves here`); d.tell('Cloud saves: offline, playing with the saves on this device'); return null; }
  if (!me || !me.user) return null;
  if (!await holdGameLock()) { d.note('[cloud] the game is open in another tab: this one does not sync'); d.tell('The game is open in another tab: cloud saves sync there'); return null; }
  const sync = new CloudSync(me.user, d);
  try { await sync.beforeGame(me); sync.synced = true; }
  catch (e) {
    d.note(`[cloud] sync before the game failed: ${e instanceof Error ? e.stack ?? e.message : e}`);
    d.tell('Cloud saves: could not sync now, playing with the saves on this device (they are sent once you save)');
  }
  return sync;
}

/** the saves here as a zip of a SaveData folder (to unpack into a game folder) */
export async function downloadZip(files: ZipEntry[]): Promise<Uint8Array> {
  return zip(files.map(f => ({ ...f, name: `${SAVE_SUB}/${f.name}` })));
}
export { CloudError };
