// Bundles the page and its workers (TypeScript) into web/dist with esbuild.
import * as esbuild from 'esbuild';
import fs from 'node:fs';
const entries = {
  'main': 'src/main.ts',
  'host/game-worker': 'src/host/game-worker.ts',
  'host/io-worker': 'src/host/io-worker.ts',
  'audio/worklet': 'src/audio/worklet.ts',
};
for (const [k, v] of Object.entries(entries)) if (!fs.existsSync(v)) delete entries[k];
await esbuild.build({ entryPoints: entries, outdir: 'dist', bundle: true, format: 'esm', target: ['chrome120'], sourcemap: true, logLevel: 'warning', loader: { '.txt': 'text' } });
fs.copyFileSync('src/index.html', 'dist/index.html');
fs.copyFileSync('src/play.html', 'dist/play.html');
console.log('web/dist OK');
