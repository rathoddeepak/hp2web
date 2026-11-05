#!/usr/bin/env bash
# Build the browser host for HP2: nfsu2-web web/ @31da468 + nfsu1web's browser patches (user-supplied
# install, local server) + HP2 names; bundle with esbuild; build nfsu1web's Rust server.
# Output: work/web/web/dist (page); serve with tools/play_server.mjs
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
rm -rf work/web && mkdir -p work/web
cp -R nfsu2-web/web work/web/web && cp -R nfsu2-web/app work/web/app
rm -rf work/web/web/node_modules work/web/web/dist work/web/web/dist-dev
while IFS= read -r -d '' p; do patch -s -p1 -d work/web -N --no-backup-if-mismatch -r - -i "$ROOT/$p"; done \
  < <(find nfsu1web/patches/nfsu2-web/web -name '*.patch' -print0 | sort -z)
cp nfsu1web/browser/menu.ts nfsu1web/browser/keyboard.ts work/web/web/src/app/
# HP2: process name + exe path handed to the wasm main()
sed -i '' "s|args: \['nfsu1', '/data/Speed.exe'\]|args: ['nfshp2', '/data/NFSHP2.exe']|" work/web/web/src/app/game.ts
grep -q "/data/NFSHP2.exe" work/web/web/src/app/game.ts || { echo "game.ts exe edit failed"; exit 1; }
# HP2 branding + its own OPFS save store
W=work/web/web
sed -i '' 's/NFS Underground | Browser Edition/NFS Hot Pursuit 2 | Browser Edition/g; s/Need for Speed: Underground/Need for Speed: Hot Pursuit 2/g; s/Need for Speed Underground, NFSU1/Need for Speed Hot Pursuit 2, NFSHP2/; s/"NFSU1"\]/"NFSHP2"]/; s|<span class="wordmark-name">Underground</span>|<span class="wordmark-name">Hot Pursuit 2</span>|' $W/index.html
grep -rl "save-nfsu1" $W/src | xargs sed -i '' 's/save-nfsu1/save-nfshp2/g'
grep -rl "Need for Speed: Underground" $W/src | xargs sed -i '' 's/Need for Speed: Underground/Need for Speed: Hot Pursuit 2/g' 2>/dev/null || true
B=work/toolchain/web-bundler
[ -x $B/node_modules/.bin/esbuild ] || npm install --prefix $B --no-audit --no-fund --ignore-scripts esbuild@0.27.4 >/dev/null
ln -s "$ROOT/$B/node_modules" work/web/web/node_modules
(cd work/web/web && node build.mjs 2>&1 | tail -3)
SIZE=$(wc -c < out/hp2.wasm | tr -d ' ')
printf '{"pages":%d,"maxPages":65536,"guestEnd":%d,"size":%d,"opt":"-O1","lift":"hp2","hdFonts":0}\n' \
  $((1024*16)) 671088640 "$SIZE" > out/game.json
mkdir -p work/web/web/dist/test && echo '{"dev":true}' > work/web/web/dist/test/dev.json   # local dev server: ?env=NAME=value accepted
echo "browser host ready. Play: node tools/play_server.mjs  ->  http://127.0.0.1:8765/"
