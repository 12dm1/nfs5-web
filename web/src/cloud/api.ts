// CLOUD1: the page's side of the site's /cloud/ API (web/cloud/cloud.mjs). Every request that changes something sends
// X-NFS5: 1 (the server refuses one without it: a page of another site cannot send it). The session is an HttpOnly
// cookie: no script here ever sees it.

/** at: when the server got it; made: when those saves were last changed (the device's newest file, server clock) */
export interface SaveInfo { rev: number; at: number; made: number; size: number; hash: string; dev: string; profiles: number }
export interface BackupInfo { id: string; at: number; made: number; size: number; dev: string; profiles: number }
export interface Me { user: string | null; save?: SaveInfo | null; backups?: BackupInfo[]; now: number }

export class CloudError extends Error {
  constructor(public status: number, public code: string, message: string, public save?: SaveInfo | null, public retry?: number) { super(message); }
}

/** what the account's saves were made on, as the account page lists it: "Chrome on Windows" (no version, nothing more) */
export function deviceLabel(): string {
  const ua = navigator.userAgent;
  const browser = /Edg\//.test(ua) ? 'Edge' : /OPR\//.test(ua) ? 'Opera' : /Firefox\//.test(ua) ? 'Firefox'
    : /Chrome\//.test(ua) ? 'Chrome' : /Safari\//.test(ua) ? 'Safari' : 'Browser';
  const os = /iPhone/.test(ua) ? 'iPhone' : /iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1) ? 'iPad'
    : /Android/.test(ua) ? 'Android' : /Windows/.test(ua) ? 'Windows' : /Mac OS X/.test(ua) ? 'Mac' : /CrOS/.test(ua) ? 'ChromeOS'
    : /Linux/.test(ua) ? 'Linux' : '';
  return os ? `${browser} on ${os}` : browser;
}

async function call(method: string, path: string, body?: unknown, headers: Record<string, string> = {}): Promise<Response> {
  const init: RequestInit = { method, cache: 'no-store', credentials: 'same-origin', headers: { ...headers } };
  if (method !== 'GET') (init.headers as Record<string, string>)['X-NFS5'] = '1';
  if (body instanceof Uint8Array) { init.body = body as BodyInit; (init.headers as Record<string, string>)['Content-Type'] = 'application/zip'; }
  else if (body !== undefined) { init.body = JSON.stringify(body); (init.headers as Record<string, string>)['Content-Type'] = 'application/json'; }
  const r = await fetch(path, init);
  if (r.ok) return r;
  let j: { error?: string; message?: string; save?: SaveInfo | null } = {};
  try { j = await r.json(); } catch { /* not JSON: nginx's or the network's */ }
  const retry = Number(r.headers.get('Retry-After')) || undefined;
  throw new CloudError(r.status, j.error ?? 'http', j.message ?? `The server answered ${r.status}`, j.save, retry);
}
const json = async <T>(r: Promise<Response>) => (await r).json() as Promise<T>;

export const api = {
  /** the signed-in account, { user: null } for none; null: this server has no cloud saves */
  async me(): Promise<Me | null> {
    try { return await json<Me>(call('GET', '/cloud/me')); }
    catch (e) { if (e instanceof CloudError && e.status === 404) return null; throw e; }
  },
  register: (user: string, password: string) => json<Me>(call('POST', '/cloud/register', { user, password, device: deviceLabel() })),
  login: (user: string, password: string) => json<Me>(call('POST', '/cloud/login', { user, password, device: deviceLabel() })),
  logout: () => json<Me>(call('POST', '/cloud/logout', {})),
  changePassword: (old: string, password: string) => json<{ ok: boolean }>(call('POST', '/cloud/password', { old, password })),
  remove: (password: string) => json<Me>(call('POST', '/cloud/delete', { password })),
  taken: async (user: string) => (await json<{ taken: boolean }>(call('GET', `/cloud/taken?u=${encodeURIComponent(user)}`))).taken,
  /** the current save's zip, or a backup's */
  async download(backup?: string): Promise<Uint8Array> {
    const r = await call('GET', backup ? `/cloud/save?b=${encodeURIComponent(backup)}` : '/cloud/save');
    return new Uint8Array(await r.arrayBuffer());
  },
  /** ifMatch: the revision these saves were made from (null: none in the cloud yet); overwrite: replace whatever is
   *  there (the server keeps that as a backup) */
  upload: (zipped: Uint8Array, o: { ifMatch?: number | null; overwrite?: boolean; madeAt: number }) =>
    json<Me>(call('PUT', '/cloud/save', zipped, {
      'X-Device': deviceLabel(), 'X-Made-At': String(Math.round(o.madeAt)),
      ...(o.overwrite ? { 'X-Overwrite': '1' } : o.ifMatch != null ? { 'If-Match': `"${o.ifMatch}"` } : { 'If-None-Match': '*' }),
    })),
};

/** "5 min ago", "yesterday 18:20", "2026-11-03" */
export function ago(ms: number, now = Date.now()): string {
  const s = Math.max(0, (now - ms) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  const d = new Date(ms), t = d.toTimeString().slice(0, 5);
  if (s < 86400 && new Date(now).getDate() === d.getDate()) return `today ${t}`;
  if (s < 2 * 86400) return `yesterday ${t}`;
  return d.toISOString().slice(0, 10);
}
