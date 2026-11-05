// The save overlay as the page sees it: the OPFS directory the I/O worker
// mounts as /save (web/src/host/io-worker.ts).  Main-thread, async API only;
// nothing here ever touches /data (the game folder is served read-only by
// the local server and never written).
//
// Writes from the page happen only while no game runs in this tab (the
// shell enforces it), so they never race the I/O worker's sync handles.

import { BLOCK, CACHE_DIR } from '../host/datacache';
export const PROFILES_DIR = 'NFS Underground 2';   // what the game creates under the save root
export const NFSU1_PROFILES_DIR = 'nfsu1';

async function root(): Promise<FileSystemDirectoryHandle> { return navigator.storage.getDirectory(); }

export async function saveRoot(saveDir: string, create = true): Promise<FileSystemDirectoryHandle> {
  return (await root()).getDirectoryHandle(saveDir, { create });
}

function parts(path: string): string[] {
  const p = path.split('/').filter(x => x && x !== '.');
  if (p.some(x => x === '..')) throw new Error(`bad path ${path}`);
  return p;
}

async function dirOf(base: FileSystemDirectoryHandle, path: string[], create: boolean) {
  let d = base;
  for (const p of path) d = await d.getDirectoryHandle(p, { create });
  return d;
}

/** The text of a file in the save dir, or null if it does not exist. */
export async function readText(saveDir: string, path: string): Promise<string | null> {
  try {
    const p = parts(path);
    const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), false);
    return await (await (await d.getFileHandle(p[p.length - 1])).getFile()).text();
  } catch { return null; }
}

export async function exists(saveDir: string, path: string): Promise<boolean> {
  try {
    const p = parts(path);
    const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), false);
    await d.getFileHandle(p[p.length - 1]);
    return true;
  } catch { return false; }
}

/** Write a whole file (creating its folders). */
export async function writeFile(saveDir: string, path: string, data: string | Uint8Array): Promise<void> {
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), true);
  const w = await (await d.getFileHandle(p[p.length - 1], { create: true })).createWritable();
  await w.write(data as FileSystemWriteChunkType); await w.close();
}

export interface Entry { path: string; size: number; mtime: number }

/** Every file under the save dir, depth first, sorted by path. */
export async function listFiles(saveDir: string): Promise<Entry[]> {
  const out: Entry[] = [];
  async function walk(d: FileSystemDirectoryHandle, rel: string) {
    for await (const [name, h] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
      const r = rel ? rel + '/' + name : name;
      if (h.kind === 'directory') await walk(h as FileSystemDirectoryHandle, r);
      else { const f = await (h as FileSystemFileHandle).getFile(); out.push({ path: r, size: f.size, mtime: f.lastModified }); }
    }
  }
  try { await walk(await saveRoot(saveDir, false), ''); } catch { /* no save dir yet */ }
  return out.sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
}

export async function readBytes(saveDir: string, path: string): Promise<Uint8Array> {
  const p = parts(path);
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), false);
  return new Uint8Array(await (await (await d.getFileHandle(p[p.length - 1])).getFile()).arrayBuffer());
}

/** Remove a file, or a folder with everything in it; nothing if it is not there. */
export async function removePath(saveDir: string, path: string): Promise<void> {
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  let d: FileSystemDirectoryHandle;
  try { d = await dirOf(await saveRoot(saveDir, false), p.slice(0, -1), false); } catch { return; }
  try { await d.removeEntry(p[p.length - 1], { recursive: true }); }
  catch (e) { if ((e as DOMException).name !== 'NotFoundError') throw e; }
}

/** The save dir's file functions, bound to it (profile.ts turnOnAutosave, renameProfile). */
export function filesOf(saveDir: string) {
  return {
    listFiles: () => listFiles(saveDir), readBytes: (path: string) => readBytes(saveDir, path),
    readText: (path: string) => readText(saveDir, path), writeFile: (path: string, data: string | Uint8Array) => writeFile(saveDir, path, data),
    remove: (path: string) => removePath(saveDir, path),
  };
}

/** Remove everything inside the save dir (the dir itself stays). */
export async function clearSaveDir(saveDir: string): Promise<void> {
  const d = await saveRoot(saveDir);
  const names: string[] = [];
  for await (const [name] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) names.push(name);
  for (const n of names) await d.removeEntry(n, { recursive: true });
}

/** Remove the whole save dir (test runs: &fresh). */
export async function removeSaveDir(saveDir: string): Promise<void> {
  try { await (await root()).removeEntry(saveDir, { recursive: true }); } catch { /* absent */ }
}

export interface Profile { name: string; size: number; mtime: number; files: number }

/** The NFSU1 driver's name represented by a profile, options or career save. */
export function nfsu1ProfileName(path: string): string | null {
  const file = path.split('/').pop() ?? '';
  const dot = file.lastIndexOf('.');
  if (dot <= 0) return null;
  const ext = file.slice(dot + 1).toLowerCase();
  let name = file.slice(0, dot);
  if (ext === 'ugd') {
    if (/_autosave$/i.test(name)) name = name.slice(0, -9);
    else if (/[0-7]$/.test(name)) name = name.slice(0, -1);
  } else if (ext !== 'cfg' && ext !== 'opt') return null;
  return /^[\x20-\x7e]{1,12}$/.test(name) && !/[\\/:]/.test(name) ? name : null;
}

/** Names recorded in the NFSU1 profile index (count byte, then 32-byte records). */
export function nfsu1NamesFromIndex(data: Uint8Array): string[] {
  if (!data.length) return [];
  const count = Math.min(data[0], Math.floor((data.length - 1) / 32));
  const names = new Set<string>();
  for (let i = 0; i < count; i++) {
    const start = 1 + i * 32;
    let end = start;
    while (end < start + 32 && data[end] !== 0) end++;
    const name = String.fromCharCode(...data.subarray(start, end));
    if (/^[\x20-\x7e]{1,12}$/.test(name) && !/[\\/:]/.test(name)) names.add(name);
  }
  return [...names];
}

/** Profiles NFSU1 saved below the isolated save mount, including its Profiles.usr index. */
export async function listProfiles(saveDir: string, files?: Entry[]): Promise<Profile[]> {
  files ??= await listFiles(saveDir);
  const by = new Map<string, Profile>();
  for (const f of files) {
    if (!f.path.toLowerCase().startsWith(`${NFSU1_PROFILES_DIR}/`)) continue;
    if (f.path.split('/').pop()?.toLowerCase() !== 'profiles.usr') continue;
    try {
      for (const name of nfsu1NamesFromIndex(await readBytes(saveDir, f.path))) {
        const key = name.toLowerCase();
        if (!by.has(key)) by.set(key, { name, size: 0, mtime: f.mtime, files: 0 });
      }
    } catch { /* an unreadable index does not hide profile files */ }
  }
  for (const f of files) {
    if (!f.path.toLowerCase().startsWith(`${NFSU1_PROFILES_DIR}/`)) continue;
    const name = nfsu1ProfileName(f.path);
    if (!name) continue;
    const key = name.toLowerCase();
    const pr = by.get(key) ?? { name, size: 0, mtime: 0, files: 0 };
    pr.files++;
    pr.size += f.size;
    pr.mtime = Math.max(pr.mtime, f.mtime);
    by.set(key, pr);
  }
  return [...by.values()].sort((a, b) => b.mtime - a.mtime);
}

/**
 * Game data the browser keeps locally (OPFS "cache/", host/datacache.ts), of
 * any version of the server's files: every file there is sparse, with a
 * ".have" map of one byte per 1 MiB block, so the cached amount is the number
 * of blocks present (the last block of a file may be shorter; the file size
 * bounds it).
 */
export async function cacheBytes(): Promise<{ bytes: number; files: number }> {
  let bytes = 0, files = 0;
  try {
    const d = await (await root()).getDirectoryHandle(CACHE_DIR);
    for await (const [name, h] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
      if (h.kind !== 'file' || !name.endsWith('.have')) continue;
      const have = new Uint8Array(await (await (h as FileSystemFileHandle).getFile()).arrayBuffer());
      let n = 0; for (const b of have) n += b ? 1 : 0;
      if (!n) continue;
      let size = n * BLOCK;
      try { size = Math.min(size, (await (await d.getFileHandle(name.slice(0, -5))).getFile()).size); } catch { /* data file gone */ }
      bytes += size; files++;
    }
  } catch { /* no cache yet */ }
  return { bytes, files };
}

export async function clearCache(): Promise<void> {
  try { await (await root()).removeEntry(CACHE_DIR, { recursive: true }); } catch { /* absent */ }
}
