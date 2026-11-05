#!/usr/bin/env node
// Headless browser check: open the local HP2 page in Brave/Chrome (WebGPU on), click "Launch game",
// record the console for N seconds, save screenshots.  Needs tools/play_server.mjs running.
//   node tools/browser_test.mjs [seconds=40] [out=work/browser]
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(path.join(ROOT, 'work/toolchain/pptr/node_modules/'));
const puppeteer = require('puppeteer-core');

const secs = Number(process.argv[2] ?? 40);
const out = path.resolve(process.argv[3] ?? path.join(ROOT, 'work/browser'));
fs.mkdirSync(out, { recursive: true });
const exe = process.env.BROWSER ?? '/Applications/Brave Browser.app/Contents/MacOS/Brave Browser';
const log = fs.createWriteStream(path.join(out, 'console.log'));
const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);

const browser = await puppeteer.launch({
  executablePath: exe, headless: 'new',
  userDataDir: path.join(out, 'profile'),
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=metal',
         '--window-size=1280,800', '--no-first-run', '--autoplay-policy=no-user-gesture-required'],
  defaultViewport: { width: 1280, height: 800 },
});
const page = await browser.newPage();
const note = s => { log.write(`[${ts()}] ${s}\n`); };
page.on('console', m => note(`console.${m.type()}: ${m.text()}`));
page.on('pageerror', e => note(`PAGEERROR: ${e.stack ?? e}`));
page.on('workercreated', w => note(`worker created: ${w.url()}`));
page.on('requestfailed', r => note(`REQFAIL ${r.url()} ${r.failure()?.errorText}`));
await page.goto('http://127.0.0.1:8765/' + (process.env.HP2_URLQ ? '?' + process.env.HP2_URLQ : ''), { waitUntil: 'domcontentloaded' });
note('loaded; webgpu=' + await page.evaluate(() => !!navigator.gpu) + ' isolated=' + await page.evaluate(() => crossOriginIsolated));
try {
  await page.waitForSelector('#play:not([disabled])', { timeout: 60000 });
  note('status: ' + await page.$eval('#status', e => e.textContent) + ' / ' + await page.$eval('#status-sub', e => e.textContent));
  await page.click('#play');
  note('clicked Launch game');
} catch (e) {
  note('play button never enabled: ' + e.message);
  note('status: ' + await page.$eval('#status', e => e.textContent).catch(() => '?') + ' / ' +
       await page.$eval('#status-sub', e => e.textContent).catch(() => '?'));
  note('problems: ' + await page.$eval('#problems', e => e.innerText).catch(() => '?'));
}
// HP2_KEYS="15:Enter,20:Escape,25:Enter" presses keys at those seconds after launch (canvas focused first)
const keys = (process.env.HP2_KEYS ?? '').split(',').filter(Boolean)
  .map(k => { const [t, key] = k.split(':'); return { t: Number(t), key }; }).sort((a, b) => a.t - b.t);
const tLaunch = Date.now();
let shot = 5;
while ((Date.now() - tLaunch) / 1000 < secs) {
  await new Promise(r => setTimeout(r, 250));
  const el = (Date.now() - tLaunch) / 1000;
  while (keys.length && keys[0].t <= el) {
    const k = keys.shift();
    const mc = /^Click@(\d+)x(\d+)$/.exec(k.key);           // "Click@870x726" clicks there instead
    if (mc) { await page.mouse.click(Number(mc[1]), Number(mc[2])); note(`clicked ${mc[1]},${mc[2]}`); continue; }
    if (!process.env.HP2_NOFOCUSCLICK) { try { await page.mouse.click(640, 400); } catch {} }
    await page.keyboard.down(k.key); await new Promise(r => setTimeout(r, 120)); await page.keyboard.up(k.key);
    note(`pressed ${k.key}`);
  }
  if (el >= shot) { await page.screenshot({ path: path.join(out, `shot-${shot}s.png`) }); note(`screenshot ${shot}s`); shot += 5; }
}
await browser.close();
log.end();
console.log(`done: ${out}/console.log, screenshots`);
