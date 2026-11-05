#!/usr/bin/env node
// Local HTTP server for playing HP2 in the browser (same routes as nfsu1web's Rust server):
//   /            the page (work/web/web/dist)
//   /game.json   build metadata (out/game.json)       /game.wasm  the module (out/hp2.wasm)
//   /data-manifest  listing of the game folder         /data/*     game files, case-insensitive, Range
// Every response carries COOP/COEP/CORP so the page is cross-origin isolated (WebAssembly threads).
// Binds 127.0.0.1 only: the module and the data are your private copy, never serve them publicly.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const DIST = path.join(ROOT, 'work/web/web/dist');
const WASM = process.env.HP2_WASM ?? path.join(ROOT, 'out/hp2.wasm');
const META = path.join(ROOT, 'out/game.json');
const DATA = path.resolve(process.env.HP2_DATA ?? path.join(ROOT, 'work/data'));
const PORT = Number(process.env.PORT ?? 8765);

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.png': 'image/png',
  '.ico': 'image/x-icon', '.txt': 'text/plain' };
const ISO = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin' };

function manifest() {
  const files = [];
  const walk = (dir, rel) => {
    const ents = fs.readdirSync(dir, { withFileTypes: true }).filter(e => !e.isSymbolicLink());
    const by = (a, b) => a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1;
    if (rel) files.push({ p: rel, d: true });
    for (const e of ents.filter(e => e.isFile()).sort(by)) {
      const st = fs.statSync(path.join(dir, e.name));
      files.push({ p: rel ? `${rel}/${e.name}` : e.name, s: st.size, m: Math.floor(st.mtimeMs / 1000) });
    }
    for (const e of ents.filter(e => e.isDirectory()).sort(by)) walk(path.join(dir, e.name), rel ? `${rel}/${e.name}` : e.name);
  };
  walk(DATA, '');
  return { files };
}

// case-insensitive lookup under DATA, refusing anything that escapes it
function resolveData(parts) {
  let cur = DATA;
  for (const raw of parts) {
    if (!raw || raw === '.' || raw === '..' || raw.includes('\0')) return null;
    if (fs.existsSync(path.join(cur, raw))) { cur = path.join(cur, raw); continue; }
    let hit = null;
    try { hit = fs.readdirSync(cur).find(n => n.toLowerCase() === raw.toLowerCase()); } catch { return null; }
    if (!hit) return null;
    cur = path.join(cur, hit);
  }
  return cur.startsWith(DATA) ? cur : null;
}

function sendFile(req, res, file, type, cache = 'no-store') {
  let st;
  try { st = fs.statSync(file); } catch { return notFound(res); }
  if (!st.isFile()) return notFound(res);
  const h = { ...ISO, 'Content-Type': type, 'Accept-Ranges': 'bytes', 'Cache-Control': cache,
    'Last-Modified': st.mtime.toUTCString() };
  const m = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range ?? '');
  if (m) {
    let start = m[1] === '' ? st.size - Number(m[2]) : Number(m[1]);
    let end = m[1] !== '' && m[2] !== '' ? Number(m[2]) : st.size - 1;
    if (start < 0 || start >= st.size || end < start) {
      res.writeHead(416, { ...h, 'Content-Range': `bytes */${st.size}` }); return res.end();
    }
    end = Math.min(end, st.size - 1);
    res.writeHead(206, { ...h, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Content-Length': end - start + 1 });
    if (req.method === 'HEAD') return res.end();
    return fs.createReadStream(file, { start, end }).pipe(res);
  }
  res.writeHead(200, { ...h, 'Content-Length': st.size });
  if (req.method === 'HEAD') return res.end();
  fs.createReadStream(file).pipe(res);
}
function notFound(res) { res.writeHead(404, { ...ISO, 'Content-Type': 'text/plain' }); res.end('not found'); }
function json(req, res, obj) {
  const b = Buffer.from(typeof obj === 'string' ? obj : JSON.stringify(obj));
  res.writeHead(200, { ...ISO, 'Content-Type': 'application/json', 'Content-Length': b.length, 'Cache-Control': 'no-store' });
  res.end(req.method === 'HEAD' ? undefined : b);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = decodeURIComponent(url.pathname);
  if (p === '/game.json'){
    const meta = JSON.parse(fs.readFileSync(META, 'utf8'));
    meta.size = fs.statSync(WASM).size;                  // always the module on disk now
    return json(req, res, meta);
  }
  if (p === '/game.wasm') return sendFile(req, res, WASM, 'application/wasm');
  if (p === '/data-manifest') return json(req, res, manifest());
  if (p === '/healthz') return json(req, res, { ok: true });
  if (p === '/data' || p.startsWith('/data/')) {
    const f = resolveData(p.slice(6).split('/').filter(Boolean));
    return f ? sendFile(req, res, f, MIME[path.extname(f).toLowerCase()] ?? 'application/octet-stream') : notFound(res);
  }
  const rel = p === '/' ? 'index.html' : p.replace(/^\/+/, '');
  const f = path.resolve(DIST, rel);
  if (!f.startsWith(DIST)) return notFound(res);
  sendFile(req, res, f, MIME[path.extname(f).toLowerCase()] ?? 'application/octet-stream');
});
server.listen(PORT, '127.0.0.1', () => {
  console.log(`HP2: open http://127.0.0.1:${PORT}/   (data: ${DATA})`);
});
