#!/usr/bin/env node
/** Minimal Node host for HP2 Wasm bring-up (from nfsu1web tools/run_wasm_node.mjs, MIT). Not the browser host. */
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const wasmPath = process.argv[2]
  ?? path.join(root, 'out/hp2.wasm');
const DATA = process.env.HP2_DATA ?? path.join(root, 'work/data');
const SAVE = process.env.HP2_SAVE ?? path.join(root, 'work/save');

const mod = isMainThread
  ? new WebAssembly.Module(fs.readFileSync(wasmPath))
  : workerData.mod;
const memory = isMainThread
  ? new WebAssembly.Memory({ initial: 16384, maximum: 65536, shared: true })
  : workerData.memory;
const ctl = isMainThread
  ? new Int32Array(new SharedArrayBuffer(16))
  : new Int32Array(workerData.ctl);

// Keep the diagnostic environment identical in every Wasm instance/thread.
// Do not expose unrelated host variables (credentials, shell configuration).
const diagnosticNames = [
  'VSDUMP', 'BT', 'COMTRACE', 'TRACE_MAX', 'VERBOSE', 'WATCH_ADDR', 'FILELIST', 'PRESENTBT',
  'NFSU1_DIAGNOSTICS', 'NFSU1_DIAGNOSTIC_WORDS', 'NFSU2_WATCHDOG',
  'NOWIDE', 'NFSNATIVE', 'NFSRES',
  'NFSU1_DYNAMIC_RESOLUTION', 'NFSU1_GRAPHICS', 'NFSU1_INTRO',
  'NFSU2_WIDTH', 'NFSU2_HEIGHT', 'RESETAT', 'HP2_D3D8TRACE', 'NFSU2_SAVEDIR', 'COMTRACE', 'REGLOG', 'HP2_RING', 'NFSU2_HEAPCHECK', 'HP2_FILEDBG', 'HP2_FFLOG', 'HP2_TEXPEEK', 'NFSU2_INPUT_SCRIPT',
];
const guestEnvironment = isMainThread
  ? diagnosticNames.filter((name) => process.env[name] !== undefined)
    .map((name) => `${name}=${process.env[name]}`)
  : workerData.guestEnvironment;
const environmentBytes = guestEnvironment.map((entry) => Buffer.from(`${entry}\0`));

// The supervisor never enters Wasm, so a guest busy loop cannot block exits or
// the optional deadline. Shared status also reaches it from nested pthreads
// whose immediate parent is blocked inside Wasm and cannot relay messages.
const finish = (code, message) => {
  if (Atomics.compareExchange(ctl, 2, 0, 1) !== 0) return;
  if (message) fs.writeSync(2, `${message}\n`);
  Atomics.store(ctl, 3, code);
  Atomics.store(ctl, 2, 2);
  parentPort?.postMessage({ type: 'finished' });
};
const spawnGuest = (role, extra = {}) => {
  const worker = new Worker(new URL(import.meta.url), {
    workerData: { mod, memory, ctl: ctl.buffer, guestEnvironment, role, ...extra },
    argv: process.argv.slice(2),
  });
  worker.on('error', (error) => finish(134, `[host] ${role} worker error: ${error.stack ?? error}`));
  worker.on('exit', (code) => {
    if (code !== 0 || role === 'main') {
      finish(code || 134, `[host] ${role} worker exited unexpectedly (${code})`);
    }
  });
  return worker;
};

const preopens = [['/data', DATA], ['/save', SAVE]];
const fds = new Map();
preopens.forEach(([name, dir], i) => fds.set(3 + i, { dir, name, pre: true }));
let nextFd = 3 + preopens.length;
const dv = () => new DataView(memory.buffer);
const u8 = () => new Uint8Array(memory.buffer);
const td = new TextDecoder();
const E = { OK: 0, BADF: 8, NOENT: 44, NOSYS: 52, INVAL: 28, EXIST: 20 };
let inst;

const wasi = {
  args_sizes_get: (c, s) => { dv().setUint32(c, 0, true); dv().setUint32(s, 0, true); return 0; },
  args_get: () => 0,
  environ_sizes_get: (c, s) => {
    dv().setUint32(c, environmentBytes.length, true);
    dv().setUint32(s, environmentBytes.reduce((total, entry) => total + entry.length, 0), true);
    return 0;
  },
  environ_get: (pointers, buffer) => {
    const d = dv();
    for (const [index, entry] of environmentBytes.entries()) {
      d.setUint32(pointers + 4 * index, buffer, true);
      u8().set(entry, buffer);
      buffer += entry.length;
    }
    return 0;
  },
  clock_res_get: (_i, p) => { dv().setBigUint64(p, 1000n, true); return 0; },
  clock_time_get: (id, _pr, p) => {
    dv().setBigUint64(p, id === 0 ? BigInt(Date.now()) * 1000000n : process.hrtime.bigint(), true);
    return 0;
  },
  random_get: (p, n) => { u8().set(crypto.randomBytes(n), p); return 0; },
  sched_yield: () => 0,
  proc_exit: (c) => {
    finish(c, `\n[host] proc_exit(${c})`);
    process.exit(c);
  },
  poll_oneoff: (inp, outp, n, pn) => {
    let w = -1n;
    const d = dv();
    for (let i = 0; i < n; i++) {
      const s = inp + 48 * i;
      if (d.getUint8(s + 8) !== 0) continue;
      const t = d.getBigUint64(s + 24, true);
      if (w < 0n || t < w) w = t;
    }
    if (w > 0n) Atomics.wait(ctl, 1, 0, Number(w / 1000000n));
    for (let i = 0; i < n; i++) {
      const s = inp + 48 * i, o = outp + 32 * i;
      d.setBigUint64(o, d.getBigUint64(s, true), true);
      d.setUint16(o + 8, 0, true);
      d.setUint8(o + 10, d.getUint8(s + 8));
    }
    d.setUint32(pn, n, true);
    return 0;
  },
  fd_write: (fd, iov, n, pw) => {
    const d = dv();
    let t = 0;
    for (let i = 0; i < n; i++) {
      const b = d.getUint32(iov + 8 * i, true), l = d.getUint32(iov + 8 * i + 4, true);
      const buf = Buffer.from(u8().slice(b, b + l));
      if (fd === 1 || fd === 2) fs.writeSync(fd, buf);
      else {
        const f = fds.get(fd);
        if (!f) return E.BADF;
        fs.writeSync(f.h, buf, 0, buf.length, f.pos);
        f.pos += buf.length;
      }
      t += l;
    }
    d.setUint32(pw, t, true);
    return 0;
  },
  fd_read: (fd, iov, n, pr) => {
    const f = fds.get(fd);
    if (!f || f.h === undefined) return fd === 0 ? (dv().setUint32(pr, 0, true), 0) : E.BADF;
    const d = dv();
    let t = 0;
    for (let i = 0; i < n; i++) {
      const b = d.getUint32(iov + 8 * i, true), l = d.getUint32(iov + 8 * i + 4, true);
      const buf = Buffer.alloc(l);
      const r = fs.readSync(f.h, buf, 0, l, f.pos);
      u8().set(buf.subarray(0, r), b);
      f.pos += r;
      t += r;
      if (r < l) break;
    }
    d.setUint32(pr, t, true);
    return 0;
  },
  fd_pread: () => E.NOSYS,
  fd_seek: (fd, off, wh, p) => {
    const f = fds.get(fd);
    if (!f || f.h === undefined) return E.BADF;
    off = Number(off);
    f.pos = wh === 0 ? off : wh === 1 ? f.pos + off : fs.fstatSync(f.h).size + off;
    dv().setBigUint64(p, BigInt(f.pos), true);
    return 0;
  },
  fd_tell: (fd, p) => {
    const f = fds.get(fd);
    if (!f) return E.BADF;
    dv().setBigUint64(p, BigInt(f.pos ?? 0), true);
    return 0;
  },
  fd_close: (fd) => {
    const f = fds.get(fd);
    if (!f) return E.BADF;
    if (f.h !== undefined) fs.closeSync(f.h);
    fds.delete(fd);
    return 0;
  },
  fd_sync: () => 0, fd_datasync: () => 0, fd_advise: () => 0,
  fd_fdstat_set_rights: () => 0, fd_fdstat_set_flags: () => 0,
  fd_fdstat_get: (fd, p) => {
    const d = dv();
    if (fd <= 2) {
      d.setUint8(p, 2); d.setUint16(p + 2, 0, true);
      d.setBigUint64(p + 8, 0xFFFFFFFFn, true); d.setBigUint64(p + 16, 0n, true);
      return 0;
    }
    const f = fds.get(fd);
    if (!f) return E.BADF;
    d.setUint8(p, f.dir ? 3 : 4); d.setUint16(p + 2, 0, true);
    d.setBigUint64(p + 8, 0x1FFFFFFFn, true); d.setBigUint64(p + 16, 0x1FFFFFFFn, true);
    return 0;
  },
  fd_prestat_get: (fd, p) => {
    const f = fds.get(fd);
    if (!f || !f.pre) return E.BADF;
    dv().setUint8(p, 0);
    dv().setUint32(p + 4, Buffer.byteLength(f.name), true);
    return 0;
  },
  fd_prestat_dir_name: (fd, p, l) => {
    const f = fds.get(fd);
    u8().set(Buffer.from(f.name).subarray(0, l), p);
    return 0;
  },
  fd_filestat_get: (fd, p) => {
    const f = fds.get(fd);
    if (!f) return E.BADF;
    const s = f.h !== undefined ? fs.fstatSync(f.h) : fs.statSync(f.dir);
    const d = dv();
    for (let i = 0; i < 64; i += 8) d.setBigUint64(p + i, 0n, true);
    d.setUint8(p + 16, f.dir ? 3 : 4);
    d.setBigUint64(p + 24, 1n, true);
    d.setBigUint64(p + 32, BigInt(s.size), true);
    return 0;
  },
  path_open: (dfd, _dfl, pp, pl, of, _rb, _ri, ff, pfd) => {
    const base = fds.get(dfd);
    if (!base?.dir) return E.BADF;
    const rel = td.decode(u8().slice(pp, pp + pl));
    const full = path.join(base.dir, rel);
    let fl = (of & 1) ? fs.constants.O_CREAT : 0;
    if (of & 4) fl |= fs.constants.O_EXCL;
    if (of & 8) fl |= fs.constants.O_TRUNC;
    fl |= fs.constants.O_RDWR;
    try {
      if (base.name === '/data') fl = fs.constants.O_RDONLY;
      const st = fs.existsSync(full) && fs.statSync(full);
      if (st && st.isDirectory()) fds.set(nextFd, { dir: full, name: rel });
      else fds.set(nextFd, { h: fs.openSync(full, fl), pos: 0 });
      dv().setUint32(pfd, nextFd++, true);
      return 0;
    } catch (e) {
      return e.code === 'ENOENT' ? E.NOENT : E.INVAL;
    }
  },
  // WASI fd_readdir: dirent = d_next u64, d_ino u64, d_namlen u32, d_type u8 (+3 pad) then the name.
  // The cookie is the index of the next entry; a truncated last entry tells libc the buffer was full.
  fd_readdir: (fd, buf, len, cookie, pused) => {
    const f = fds.get(fd);
    if (!f || !f.dir) return E.INVAL;
    if (!f.ents || cookie === 0n) {
      f.ents = ['.', '..', ...fs.readdirSync(f.dir)].map(n => {
        let t = 4; try { const st = fs.lstatSync(path.join(f.dir, n)); t = st.isDirectory() ? 3 : st.isSymbolicLink() ? 7 : 4; } catch {}
        return { n: Buffer.from(n), t };
      });
    }
    const m = u8(), d = dv(); let used = 0;
    for (let i = Number(cookie); i < f.ents.length && used < len; i++) {
      const e = f.ents[i], rec = new Uint8Array(24 + e.n.length), rv = new DataView(rec.buffer);
      rv.setBigUint64(0, BigInt(i + 1), true); rv.setBigUint64(8, BigInt(i + 1), true);
      rv.setUint32(16, e.n.length, true); rv.setUint8(20, e.t); rec.set(e.n, 24);
      const take = Math.min(rec.length, len - used);
      m.set(rec.subarray(0, take), buf + used); used += take;
    }
    d.setUint32(pused, used, true);
    return 0;
  },
  path_filestat_get: (dfd, _f, pp, pl, out) => {
    const base = fds.get(dfd);
    const full = path.join(base.dir, td.decode(u8().slice(pp, pp + pl)));
    try {
      const s = fs.statSync(full);
      const d = dv();
      for (let i = 0; i < 64; i += 8) d.setBigUint64(out + i, 0n, true);
      d.setUint8(out + 16, s.isDirectory() ? 3 : 4);
      d.setBigUint64(out + 24, 1n, true);
      d.setBigUint64(out + 32, BigInt(s.size), true);
      return 0;
    } catch { return E.NOENT; }
  },
  path_create_directory: (dfd, pp, pl) => {
    const base = fds.get(dfd);
    if (!base?.dir) return E.BADF;
    const full = path.join(base.dir, td.decode(u8().slice(pp, pp + pl)));
    try {
      fs.mkdirSync(full, { recursive: true });
      return 0;
    } catch (e) {
      return e.code === 'EEXIST' ? E.EXIST : E.INVAL;
    }
  },
};

const proxy = new Proxy(wasi, {
  get: (t, k) => t[k] ?? (() => {
    process.stderr.write(`[host] unimplemented wasi ${String(k)}\n`);
    return E.NOSYS;
  }),
});
const threadSpawn = (arg) => {
  const t = Atomics.add(ctl, 0, 1) + 1;
  spawnGuest('pthread', { tid: t, arg });
  return t;
};
const imports = {
  env: { memory },
  wasi_snapshot_preview1: proxy,
  wasi: { 'thread-spawn': threadSpawn },
  nfsu2: {
    register: () => 0,
    now_ms: () => Date.now(),
    blocked_ms: () => 0,
    stack_frames: () => 0,
  },
};
for (const im of WebAssembly.Module.imports(mod)) {
  const ns = (imports[im.module] ??= {});
  if (im.kind === 'function' && !(im.name in ns)) {
    ns[im.name] = (..._a) => {
      process.stderr.write(`[host] unimplemented import ${im.module}.${im.name}\n`);
      return 0;
    };
  }
}
if (isMainThread) {
  const timeout = process.env.NFSU1_TIMEOUT_MS === undefined
    ? undefined : Number(process.env.NFSU1_TIMEOUT_MS);
  if (timeout !== undefined && (!Number.isInteger(timeout) || timeout <= 0 || timeout > 2147483647)) {
    throw new Error('NFSU1_TIMEOUT_MS must be an integer from 1 to 2147483647');
  }
  const checkFinished = () => {
    if (Atomics.load(ctl, 2) === 2) process.exit(Atomics.load(ctl, 3));
  };
  const main = spawnGuest('main');
  main.on('message', checkFinished);
  setInterval(checkFinished, 10);
  if (timeout !== undefined) {
    setTimeout(() => {
      finish(124, `[host] timeout after ${timeout} ms`);
      checkFinished();
    }, timeout);
  }
} else {
  try {
    inst = await WebAssembly.instantiate(mod, imports);
    if (workerData.role === 'main') {
      inst.exports._start();
      finish(0, '[host] _start returned');
    } else {
      inst.exports.wasi_thread_start(workerData.tid, workerData.arg);
    }
  } catch (e) {
    finish(134, `[host] ${workerData.role} trap: ${e.stack ?? e}`);
  }
}
