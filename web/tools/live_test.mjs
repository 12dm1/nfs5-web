// Opens the public site in headless Chrome (muted, fresh profile): the PLAY page, a click on PLAY, then the game for
// --seconds. Prints the PLAY page's note, console errors, the game's own log lines that matter, and how much came
// from the server. No server of ours is started.
//   node web/tools/live_test.mjs [--site https://nfs5-reforged.duckdns.org] [--seconds 60]
import { chromium } from 'playwright-core';
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const SITE = arg('site', 'https://nfs5-reforged.duckdns.org'), SECONDS = +arg('seconds', 60);
const browser = await chromium.launch({
  executablePath: arg('chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe'), headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
});
const ctx = await browser.newContext({ viewport: { width: 1280, height: 960 } });
const page = await ctx.newPage();
let bytes = 0, reqs = 0, bad = 0;
page.on('response', async r => { reqs++; if (r.status() >= 400) { bad++; console.log(`[http ${r.status()}] ${r.url()}`); } const l = +(r.headers()['content-length'] || 0); bytes += l; });
page.on('console', m => {
  const t = m.text();
  if (m.type() === 'error' || m.type() === 'warning' || /TRAP|MESSAGEBOX|page error|\[fps\]|\[lan\]|\[exit\]|Initializing|renderer ready|CANNOT OPEN/.test(t)) console.log(`[${m.type()}] ${t.slice(0, 200)}`);
});
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
await page.goto(SITE + '/');
console.log('PLAY page note: ' + await page.textContent('#note'));
await Promise.all([page.waitForURL('**/game'), page.click('#play')]);
console.log('after PLAY: ' + page.url() + ', crossOriginIsolated=' + await page.evaluate(() => crossOriginIsolated));
await page.waitForTimeout(SECONDS * 1000);
console.log(`status line: ${await page.textContent('#status').catch(() => '?')}`);
console.log(`${reqs} responses, ${bad} failed, ${(bytes / 1e6).toFixed(1)} MB by Content-Length (gzip and chunked not counted)`);
await browser.close();
