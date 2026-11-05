# deploy/ — Cloudflare Pages + R2 deployment helpers

See the top-level **[`DEPLOYMENT.md`](../DEPLOYMENT.md)** for the full handover.

| File | Purpose |
|------|---------|
| `_headers` | Cloudflare Pages headers: cross-origin isolation (COOP/COEP/CORP) + CSP allowing the R2 origin. Copied into the deployed bundle root. |
| `gen-manifest.mjs` | Generates `/data-manifest` from a game-install folder. Usage below. |
| `r2-cors.json` | CORS policy applied to the R2 bucket (`wrangler r2 bucket cors set hp2web-assets --file r2-cors.json`). |
| `deploy.sh` | One-shot: assembles the Pages bundle and deploys. Does **not** upload to R2 (do that only when the wasm or game data changes). |

### Generate the manifest
```bash
node deploy/gen-manifest.mjs \
  <game-install-dir> \
  https://pub-faa9a71f0ba54b3b9c026a9290b09605.r2.dev/data/ \
  <out>/data-manifest
```

### Full deploy
```bash
bash deploy/deploy.sh          # build page + bundle + deploy to Pages
```
R2 uploads (wasm / game data) are manual — see `DEPLOYMENT.md` §5c–5d.
