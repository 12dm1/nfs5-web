// The start page (play.html): the PLAY button, and CLOUD1's account line and dialogs (docs/tasks/CLOUD1-cloud-saves.md "Pages").
// PLAY: signed in (or no cloud on this server, or "don't ask again"): straight to the game; else the dialog that offers
// an account, with "Play without an account" under it. Everything a server or a player typed is put on the page as
// text (textContent), never as markup.
import { api, ago, CloudError, type Me } from './cloud/api';
import { downloadZip, filesOfZip, keepLocalBackup, localBackups, profilesIn, readLocal, readLocalBackup, restoreLocal, withGameLock } from './cloud/sync';

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const SAVE_DIR = 'save';
const NOASK = 'nfs5.cloud.noask';
const store = {
  get: (k: string) => { try { return localStorage.getItem(k); } catch { return null; } },
  set: (k: string, v: string | null) => { try { if (v === null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch { /* private mode */ } },
};

// The game needs WebGPU and SharedArrayBuffer (the server sends the cross-origin isolation headers).
if (!('gpu' in navigator) || typeof SharedArrayBuffer === 'undefined') {
  const n = $('note');
  n.textContent = 'This browser cannot run the game: it needs WebGPU. Open this page in an up-to-date Chrome, Edge or Safari.';
  n.className = 'warn';
}

/** an element with attributes and children (strings become text) */
function el<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Record<string, string | boolean> = {}, ...kids: (Node | string | null)[]): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) { if (v === true) e.setAttribute(k, ''); else if (v !== false) e.setAttribute(k, v); }
  for (const k of kids) if (k !== null) e.append(k);
  return e;
}
function button(text: string, onClick: (b: HTMLButtonElement) => void | Promise<void>, cls = 'btn'): HTMLButtonElement {
  const b = el('button', { type: 'button', class: cls }, text);
  b.addEventListener('click', async () => {
    b.disabled = true;
    try { await onClick(b); } finally { b.disabled = false; }
  });
  return b;
}
function saveFile(name: string, data: Uint8Array | string, type: string) {
  const url = URL.createObjectURL(new Blob([data as BlobPart], { type }));
  const a = el('a', { href: url, download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30e3);
}
const day = (ms: number) => new Date(ms).toISOString().slice(0, 10);
const errText = (e: unknown) => e instanceof CloudError ? e.message : e instanceof TypeError ? 'Cannot reach the server, check your connection' : String(e instanceof Error ? e.message : e);
const plural = (n: number, one: string) => `${n} ${one}${n === 1 ? '' : 's'}`;

let me: Me | null | undefined;           // undefined: not known yet; null: no cloud saves on this server
async function refresh(): Promise<void> {
  try { me = await api.me(); } catch { me = undefined; }
  renderLine();
}
function renderLine() {
  const line = $('acct');
  line.replaceChildren();
  if (me === null) return;
  if (me === undefined) { line.append('Cloud saves: the server does not answer right now'); return; }
  if (me.user) {
    line.append('Signed in as ', el('b', {}, me.user));
    if (me.save) line.append(el('span', {}, `cloud save ${ago(me.save.at, me.now)}`));
    line.append(button('Account', () => openAccount(), 'link'));
  } else {
    line.append('Cloud saves: not signed in');
    line.append(button('Sign in or create an account', () => openSign('new'), 'link'));
    line.append(button('Saves on this device', () => openAccount(), 'link'));
  }
}

// ---------------------------------------------------------------------------------------------- PLAY ----
const play = $<HTMLAnchorElement>('play');
const go = () => { location.href = '/game'; };
play.addEventListener('click', async ev => {
  if (ev.button !== 0 || ev.ctrlKey || ev.metaKey || ev.shiftKey) return;       // a new tab: just the game
  ev.preventDefault();
  if (me === undefined) await Promise.race([refresh(), new Promise(r => setTimeout(r, 3000))]);
  if (me && !me.user && store.get(NOASK) !== '1') openSign('new');
  else go();
});

// ------------------------------------------------------------------------------- sign in / create ----
const sign = $<HTMLDialogElement>('dlg-sign');
const form = $<HTMLFormElement>('sign-form');
const fUser = $<HTMLInputElement>('f-user'), fPass = $<HTMLInputElement>('f-pass'), fPass2 = $<HTMLInputElement>('f-pass2');
const fErr = $('f-err'), fGo = $<HTMLButtonElement>('f-go');
let mode: 'new' | 'old' = 'new';
let afterSign: () => void = go;
function setMode(m: 'new' | 'old') {
  mode = m;
  $('tab-new').setAttribute('aria-selected', String(m === 'new'));
  $('tab-old').setAttribute('aria-selected', String(m === 'old'));
  $('f-pass2-row').hidden = m === 'old';
  $('f-user-hint').hidden = m === 'old';
  $('f-warn').hidden = m === 'old';
  fPass.autocomplete = m === 'new' ? 'new-password' : 'current-password';
  fGo.textContent = m === 'new' ? 'Create account and play' : 'Sign in and play';
  fErr.textContent = '';
}
function openSign(m: 'new' | 'old', then: () => void = go) {
  afterSign = then;
  setMode(m);
  $<HTMLInputElement>('f-noask').checked = store.get(NOASK) === '1';
  if (!sign.open) sign.showModal();
  fUser.focus();
}
$('tab-new').addEventListener('click', () => setMode('new'));
$('tab-old').addEventListener('click', () => setMode('old'));
$('f-guest').addEventListener('click', () => {
  store.set(NOASK, $<HTMLInputElement>('f-noask').checked ? '1' : null);
  sign.close();
  go();
});
for (const d of document.querySelectorAll('dialog')) d.querySelector('[data-close]')?.addEventListener('click', () => (d as HTMLDialogElement).close());

form.addEventListener('submit', async ev => {
  ev.preventDefault();
  fErr.textContent = '';
  const user = fUser.value.trim(), pw = fPass.value;
  if (mode === 'new') {
    if (!/^[A-Za-z0-9][A-Za-z0-9_-]{2,14}$/.test(user)) { fErr.textContent = 'The login is 3 to 15 letters, digits, _ or -, starting with a letter or digit.'; fUser.focus(); return; }
    if (pw.length < 8) { fErr.textContent = 'The password needs at least 8 characters.'; fPass.focus(); return; }
    if (pw !== fPass2.value) { fErr.textContent = 'The two passwords are not the same.'; fPass2.focus(); return; }
  } else if (!user || !pw) { fErr.textContent = 'Type your login and password.'; return; }
  fGo.disabled = true;
  try {
    me = mode === 'new' ? await api.register(user, pw) : await api.login(user, pw);
    // Chrome and Edge: offer to save the password now (other browsers see the form's submit)
    const PC = (window as unknown as { PasswordCredential?: new (d: { id: string; password: string; name: string }) => Credential }).PasswordCredential;
    if (PC) void navigator.credentials.store(new PC({ id: me.user!, password: pw, name: me.user! })).catch(() => {});
    store.set(NOASK, null);
    sign.close();
    renderLine();
    if (mode === 'new') {
      $('done-name').textContent = me.user;
      const done = $<HTMLDialogElement>('dlg-done');
      $('done-file').onclick = () => saveFile(`nfs5-reforged-login-${me!.user}.txt`,
        `NFS5 Reforged cloud saves (${location.origin}/)\r\nLogin: ${me!.user}\r\nPassword: ${pw}\r\n\r\nThere is no password recovery. Keep this file private.\r\n`, 'text/plain');
      $('done-play').onclick = () => { done.close(); afterSign(); };
      done.showModal();
    } else afterSign();
    fPass.value = fPass2.value = '';
  } catch (e) {
    fErr.textContent = errText(e);
  } finally {
    fGo.disabled = false;
  }
});

// ------------------------------------------------------------------------------------------ account ----
const acct = $<HTMLDialogElement>('dlg-acct'), body = $('acct-body');
async function openAccount() {
  await refresh();
  await renderAccount();
  if (!acct.open) acct.showModal();
}
function section(title: string, ...kids: (Node | string | null)[]) { return el('div', {}, el('h3', {}, title), ...kids); }
function row(text: string | Node, ...actions: HTMLElement[]) { return el('div', { class: 'row' }, el('span', {}, text), el('span', { class: 'inline' }, ...actions)); }
const status = el('p', { class: 'ok', role: 'status' });
const failure = el('p', { class: 'err', role: 'alert' });
function say(ok: string, err = '') { status.textContent = ok; failure.textContent = err; }

/** the cloud's zip again as a SaveData folder's zip (the layout of every download here) */
async function asFolderZip(flat: Uint8Array) { return downloadZip(await filesOfZip(flat)); }

async function renderAccount() {
  say('');
  const user = me?.user ?? null;
  $('acct-h').textContent = user ? `Account ${user}` : 'Saves on this device';
  const parts: (Node | null)[] = [];
  if (user && me) {
    const s = me.save;
    parts.push(section('Cloud save',
      s ? row(`${plural(s.profiles, 'profile')}, saved ${ago(s.made ?? s.at, me.now)} on ${s.dev}`,
        button('Download', async () => { try { saveFile(`nfs5-saves-${user}-${day(s.at)}.zip`, await asFolderZip(await api.download()), 'application/zip'); } catch (e) { say('', errText(e)); } }))
        : el('p', { class: 'dim' }, 'Nothing yet: your saves are sent when you play.')));
    if (me.backups?.length) parts.push(section('Older cloud versions',
      el('p', { class: 'dim' }, 'Kept by the server when a version is replaced (from another device, or once a day).'),
      ...me.backups.map(b => row(`${ago(b.made ?? b.at, me!.now)}, ${b.dev}, ${plural(b.profiles, 'profile')}`,
        button('Download', async () => { try { saveFile(`nfs5-saves-${user}-${day(b.at)}.zip`, await asFolderZip(await api.download(b.id)), 'application/zip'); } catch (e) { say('', errText(e)); } })))));
  }
  const here = await readLocal(SAVE_DIR).catch(() => []);
  parts.push(section('Saves on this device',
    row(here.length ? `${plural(profilesIn(here), 'profile')}` : 'None yet',
      ...(here.length ? [button('Download', async () => saveFile(`nfs5-saves-this-device-${day(Date.now())}.zip`, await downloadZip(here), 'application/zip'))] : []),
      button('Restore from a file...', () => $<HTMLInputElement>('restore-file').click())),
    el('p', { class: 'dim' }, 'A downloaded zip (or a SaveData folder of the PC game, zipped). It replaces the saves here; what was here is kept below.' +
      (user ? ' The next game sends it to the cloud.' : ''))));
  const kept = await localBackups().catch(() => []);
  if (kept.length) parts.push(section('Replaced on this device',
    ...kept.map(b => row(`${ago(b.at)} (${(b.size / 1024).toFixed(0)} KB)`,
      button('Download', async () => { const z = await readLocalBackup(b.name); if (z) saveFile(`nfs5-saves-replaced-${day(b.at)}.zip`, await asFolderZip(z), 'application/zip'); })))));
  if (user) {
    const old = el('input', { type: 'password', autocomplete: 'current-password', maxlength: '128' });
    const neu = el('input', { type: 'password', autocomplete: 'new-password', maxlength: '128' });
    const del = el('input', { type: 'password', autocomplete: 'current-password', maxlength: '128' });
    parts.push(section('Password',
      el('label', {}, 'Current password', old), el('label', {}, 'New password (8 or more characters)', neu),
      el('div', { class: 'inline' }, button('Change password', async () => {
        try { await api.changePassword(old.value, neu.value); old.value = neu.value = ''; say('Password changed. Other devices sign in again.'); }
        catch (e) { say('', errText(e)); }
      })), el('p', { class: 'dim' }, 'Other devices are signed out when the password changes.')));
    parts.push(section('Sign out',
      el('div', { class: 'inline' }, button('Sign out on this device', async () => {
        try { me = await api.logout(); renderLine(); acct.close(); } catch (e) { say('', errText(e)); }
      })), el('p', { class: 'dim' }, 'The saves stay here and in the cloud.')));
    parts.push(section('Delete account',
      el('p', { class: 'dim' }, `Deletes ${user} and its cloud saves for good. The saves on this device stay.`),
      el('label', {}, 'Password', del),
      el('div', { class: 'inline' }, button('Delete account', async () => {
        try { me = await api.remove(del.value); renderLine(); acct.close(); } catch (e) { say('', errText(e)); }
      }, 'btn danger'))));
  } else if (me) {
    parts.push(el('p', {}, button('Sign in or create an account', async () => { acct.close(); openSign('new', () => { void openAccount(); }); }, 'link')));
  }
  body.replaceChildren(status, failure, ...parts.filter((x): x is Node => x !== null));
}

$<HTMLInputElement>('restore-file').addEventListener('change', async ev => {
  const input = ev.target as HTMLInputElement, f = input.files?.[0];
  input.value = '';
  if (!f) return;
  try {
    if (f.size > 64 << 20) throw new Error('This file is too big for saves');
    const files = await filesOfZip(new Uint8Array(await f.arrayBuffer()));
    if (!profilesIn(files)) throw new Error('No career profile (.sav) in this zip');
    const done = await withGameLock(async () => {
      const here = await readLocal(SAVE_DIR);
      await keepLocalBackup(here, 'restore');
      await restoreLocal(SAVE_DIR, files);
    });
    if (!done) throw new Error('The game is open in another tab: close it first');
    await renderAccount();
    say(`Restored ${plural(profilesIn(files), 'profile')}. Start the game to play them${me?.user ? ' (and to send them to the cloud)' : ''}.`);
  } catch (e) { say('', e instanceof Error ? e.message : String(e)); }
});
void refresh();
