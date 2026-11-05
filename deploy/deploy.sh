#!/usr/bin/env bash
# Assemble the Cloudflare Pages bundle and deploy it.
#
# Deploys the PAGE only (shell + game.json + data-manifest + _headers).
# It does NOT upload to R2 — do that separately when the wasm or game data
# changes (see DEPLOYMENT.md §5c–5d).
#
#   bash deploy/deploy.sh [--data <game-install-dir>] [--no-build]
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R2_BASE="https://pub-faa9a71f0ba54b3b9c026a9290b09605.r2.dev"
PROJECT="hp2web"
DATA="$ROOT/work/data"
BUILD=1

while [ $# -gt 0 ]; do
  case "$1" in
    --data) DATA="$2"; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1"; exit 2 ;;
  esac
done

WEB="$ROOT/work/web/web"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

if [ "$BUILD" = 1 ]; then
  echo "▶ building page…"
  ( cd "$WEB" && node build.mjs >/dev/null )
fi

echo "▶ assembling bundle…"
cp -R "$WEB/dist/." "$STAGE/"
cp "$ROOT/deploy/_headers" "$STAGE/_headers"

# game.json + moduleUrl
node -e '
  const fs=require("fs");
  const g=JSON.parse(fs.readFileSync(process.argv[1],"utf8"));
  g.moduleUrl=process.argv[2]+"/hp2.wasm";
  fs.writeFileSync(process.argv[3],JSON.stringify(g));
' "$ROOT/out/game.json" "$R2_BASE" "$STAGE/game.json"

# data-manifest
node "$ROOT/deploy/gen-manifest.mjs" "$DATA" "$R2_BASE/data/" "$STAGE/data-manifest"

echo "▶ deploying to Cloudflare Pages ($PROJECT)…"
( cd "$WEB" && npx wrangler pages deploy "$STAGE" \
    --project-name "$PROJECT" --branch main --commit-dirty=true )

echo "✔ done → https://$PROJECT.pages.dev"
