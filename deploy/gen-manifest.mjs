import { readdirSync, statSync, writeFileSync } from 'node:fs';
import { join, posix } from 'node:path';

const ROOT = process.argv[2];              // work/data
const DATA_BASE = process.argv[3];         // R2 public /data/ prefix
const OUT = process.argv[4];

const files = [];
function walk(absDir, rel) {
  const entries = readdirSync(absDir, { withFileTypes: true })
    // byte-sorted like the server (libuv scandir)
    .sort((a, b) => Buffer.from(a.name).compare(Buffer.from(b.name)));
  for (const e of entries) {
    if (e.name.startsWith('.')) continue;          // skip dotfiles (.DS_Store)
    if (e.isSymbolicLink()) continue;              // never follow symlinks
    const abs = join(absDir, e.name);
    const r = rel ? posix.join(rel, e.name) : e.name;
    if (e.isDirectory()) {
      files.push({ p: r, d: 1 });
      walk(abs, r);
    } else if (e.isFile()) {
      const st = statSync(abs);
      files.push({ p: r, s: st.size, m: Math.floor(st.mtimeMs) });
    }
  }
}
walk(ROOT, '');

const manifest = { files, dataBase: DATA_BASE };
writeFileSync(OUT, JSON.stringify(manifest));
const fileCount = files.filter(f => !f.d).length;
const total = files.reduce((a, f) => a + (f.s || 0), 0);
console.log(`manifest: ${files.length} entries (${fileCount} files, ${(total/1048576).toFixed(1)} MiB), dataBase=${DATA_BASE}`);
