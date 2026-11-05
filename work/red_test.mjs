import { createRequire } from 'node:module';
const require = createRequire('/Users/clufter/dev/hp2web/work/toolchain/pptr/node_modules/');
const p = require('puppeteer-core');
const b = await p.launch({ executablePath: '/Applications/Brave Browser.app/Contents/MacOS/Brave Browser', headless: 'new',
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-angle=metal'] });
const pg = await b.newPage(); await pg.goto('http://127.0.0.1:8765/test/red.html'); await new Promise(r => setTimeout(r, 1500));
await pg.screenshot({ path: '/Users/clufter/dev/hp2web/work/red.png', clip: { x: 0, y: 0, width: 400, height: 300 } }); await b.close();
