# NFS: Hot Pursuit 2 — Web Deployment Handover

This document describes how **NFS: Hot Pursuit 2 (browser edition)** is deployed
and how to operate, update, and re-create the deployment. The game is a
WebAssembly recompilation (WebGPU renderer, Web Audio) served as a **static
Cloudflare Pages site + Cloudflare R2** for the large assets.

> The repo it was originally built from assumes a Rust web server (`work/web/web/server-rs`,
> `work/web/web/deploy/`). This deployment is a **serverless adaptation** of that
> design — no server runs; R2 replaces the server's `/data` and module endpoints.

---

## 1. Live URLs

| What | URL |
|------|-----|
| Game (production) | https://hp2web.pages.dev |
| R2 public base | https://pub-faa9a71f0ba54b3b9c026a9290b09605.r2.dev |
| — game module (WASM, 55 MB) | `…/hp2.wasm` |
| — game data (574 MB, 1239 files) | `…/data/…` |

**Browser requirement:** Chrome / Edge (or any browser with **WebGPU**). It will
not run without WebGPU. The page is **cross-origin isolated** (needs
`SharedArrayBuffer`).

---

## 2. Cloudflare resources

- **Account:** `deepak.rathod@abeya.co` (id `9fd3df4de0a958d836770f08d5a5f63f`)
- **Pages project:** `hp2web` (production branch `main`)
- **R2 bucket:** `hp2web-assets`
  - Public access via the r2.dev managed URL is **enabled**.
  - **CORS** is set (GET/HEAD; allows the `range` request header; exposes
    `Content-Length, Content-Range, Accept-Ranges, ETag, Content-Type`).
    Required because the cross-origin-isolated page fetches the wasm and game
    data from R2 with CORS + HTTP Range.

---

## 3. How it works (the serverless glue)

The web loader already supports hosting large assets off-origin — no code changes
to the game were needed, only configuration:

1. **`game.json`** (served by Pages) carries a **`moduleUrl`** field pointing the
   55 MB wasm at R2. The loader fetches the module from there instead of
   same-origin `/game.wasm`. (The wasm is 55 MB; Cloudflare Pages rejects any file
   over **25 MiB**, which is why it must live on R2.)

2. **`/data-manifest`** (served by Pages) is a JSON index of every game file
   (`{ files: [{ p, s, m }], dataBase }`). Its **`dataBase`** points the game's
   I/O worker at R2's `…/data/` prefix. The worker reads each file in 1 MiB
   blocks using **HTTP `Range` requests**; R2 answers with `206 Partial Content`.

3. **`_headers`** (Cloudflare Pages) supplies:
   - **Cross-origin isolation** — `Cross-Origin-Opener-Policy: same-origin`,
     `Cross-Origin-Embedder-Policy: require-corp`, `Cross-Origin-Resource-Policy:
     same-origin`. Needed for `SharedArrayBuffer`.
   - A **Content-Security-Policy** widened so `connect-src` and `media-src`
     include the R2 origin. (The server's stock CSP is `'self'`-only and would
     block all R2 fetches.)
   - `Permissions-Policy`, `X-Content-Type-Options`, etc., mirroring the server.
   - `no-cache` on `game.json` / `data-manifest`; `Service-Worker-Allowed: /` on
     `sw.js`.

4. **Service worker (`sw.js`)** caches the page + module for offline play. It is
   built to skip `Range` requests and cross-origin caching, and it explicitly
   understands a cross-origin `moduleUrl` — so R2 hosting is a first-class path.

---

## 4. Source of the deployed files

| Deployed file | Came from |
|---------------|-----------|
| Page shell (`index.html`, `main.js`, `main.css`, `sw.js`, `host/`, `audio/`, `test/`) | `work/web/web/dist/` (built by `cd work/web/web && node build.mjs`) |
| `game.json` | `out/game.json` + `moduleUrl` added |
| `hp2.wasm` | `out/hp2.wasm` |
| `/data-manifest` + `/data/` tree | generated/synced from `work/data/` (the retail install) |

> `work/data/` (the retail game) and `out/hp2.wasm` are **git-ignored** — they are
> copyrighted and/or large build artifacts. They live only locally and on R2.

---

## 5. Re-creating or updating the deployment

Prerequisites: `wrangler` (logged in to the Cloudflare account) and the AWS CLI
(for bulk R2 upload). All commands run from the repo root unless noted.

### 5a. Build the page (only if source changed)
```bash
cd work/web/web && node build.mjs          # outputs work/web/web/dist/
```

### 5b. Assemble the Pages bundle
Copy `dist/` to a staging dir, then add three files:
- **`game.json`** — copy `out/game.json`, add
  `"moduleUrl": "https://pub-faa9a71f0ba54b3b9c026a9290b09605.r2.dev/hp2.wasm"`.
- **`data-manifest`** — generated from `work/data/` (see `deploy/gen-manifest.mjs`;
  format `{files:[{p,s,m}],dataBase}`, `dataBase` = R2 `…/data/`).
- **`_headers`** — the isolation + CSP header file (see `deploy/_headers`).

### 5c. Upload the WASM to R2 (only when the module is rebuilt)
```bash
npx wrangler r2 object put hp2web-assets/hp2.wasm \
  --file out/hp2.wasm --content-type application/wasm --remote
```
> Note the `--remote` flag — without it, wrangler writes to the local simulator.

### 5d. Upload the game data to R2 (only when game files change)
Uses R2's S3-compatible API via the AWS CLI (parallel, fast). You need an **R2
S3 API token** (Cloudflare dashboard → R2 → Manage API Tokens → *Object Read &
Write*).
```bash
export AWS_ACCESS_KEY_ID=<r2-access-key-id>
export AWS_SECRET_ACCESS_KEY=<r2-secret>
export AWS_DEFAULT_REGION=auto
ENDPOINT=https://9fd3df4de0a958d836770f08d5a5f63f.r2.cloudflarestorage.com
aws configure set default.s3.max_concurrent_requests 32
cd work/data
aws s3 sync . s3://hp2web-assets/data/ --endpoint-url "$ENDPOINT" \
  --exclude ".*" --exclude "*/.*"
```
> `cacheKey` includes each file's size and mtime, so a **changed** file is a new
> cache entry — re-sync after any edit, and regenerate the manifest.

### 5e. Deploy the page to Cloudflare Pages
```bash
npx wrangler pages deploy <staging-dir> \
  --project-name hp2web --branch main --commit-dirty=true
```
> First-time project creation needed `wrangler pages project create hp2web
> --production-branch main --force` (the `--force` bypasses Cloudflare's
> redirect of `pages project create` to Workers). Not needed again.

Helper scripts live in **`deploy/`** (see that folder).

---

## 6. Operational notes

- **Bandwidth / cost:** every fresh visitor streams the 55 MB module + whatever
  game data they reach from R2. R2 **egress is free**; storage for ~630 MB is
  ~$0.01/month. Fine for personal / small-group use.
- **Offline:** after a first successful load, the service worker lets the page
  (and already-downloaded data) run offline.
- **Verify a deploy quickly:**
  ```bash
  curl -sI https://hp2web.pages.dev/ | grep -i cross-origin   # isolation headers
  curl -s  https://hp2web.pages.dev/game.json                 # moduleUrl present
  curl -s -r 0-3 https://pub-faa9a71f0ba54b3b9c026a9290b09605.r2.dev/data/NFSHP2.exe -D - -o /dev/null | grep -i content-range
  ```

---

## 7. Security

- **No secrets are committed.** R2 S3 credentials are supplied at upload time via
  environment variables only (never written to the repo). `.gitignore` also
  blocks `*.env`, `*secret*`, `*.key`, etc. as defense in depth.
- If an R2 S3 API token was created for a bulk upload, **rotate/delete it** in the
  dashboard once the upload is done.
- The `hp2web-assets` bucket is **publicly readable** at its r2.dev URL (needed so
  browsers can fetch the game). Don't put anything private in it.

---

## 8. What is NOT in this repo

Git-ignored (local-only or re-creatable): `work/data/` (retail game),
`work/toolchain/` (downloaded SDKs), `out/`, `gen/`, build caches
(`work/objcache`, `work/browser28`, `work/analysis`), `*.o`/`*.wasm` artifacts,
`*.log`, `node_modules/`, `.venv/`, `.wrangler/` local state. See `.gitignore`.
