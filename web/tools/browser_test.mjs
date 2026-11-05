// Runs the page in headless Chrome and prints what the game logs; no window on the user's screen.
//   node web/tools/browser_test.mjs [--seconds 60] [--url '/?log=1&...'] [--port 8077]
// The game's log comes through the dev server's stdout (POST /test-log), so start this with the server as child.
import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const SECONDS = +arg('seconds', 60), PORT = +arg('port', 8077), URLPATH = arg('url', '/?log=1');
const eval_ = arg('eval', '');

const srv = spawn(process.execPath, [path.join(HERE, '../server.mjs'), '--port', String(PORT)], { stdio: ['ignore', 'inherit', 'inherit'] });
await new Promise(r => setTimeout(r, 800));
const launch = {
  executablePath: arg('chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe'),
  headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=d3d11', '--mute-audio', '--autoplay-policy=no-user-gesture-required'],
  viewport: { width: 1280, height: 960 },
};
// --profile <folder>: a browser profile that stays, and with it the game's save folder (OPFS)
const prof = arg('profile', '');
const browser = prof ? await chromium.launchPersistentContext(prof, launch) : await (await chromium.launch(launch)).newContext({ viewport: launch.viewport });
const page = await browser.newPage();
page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') console.log('[console ' + m.type() + '] ' + m.text()); });
page.on('pageerror', e => console.log('[pageerror] ' + e.message));
await page.goto(`http://127.0.0.1:${PORT}${URLPATH}`);
await new Promise(r => setTimeout(r, SECONDS * 1000));
if (eval_) console.log('[eval] ' + JSON.stringify(await page.evaluate(eval_)));
await browser.close();
srv.kill();
process.exit(0);
