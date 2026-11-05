#!/usr/bin/env bash
# compile_shards.sh <gen dir> : compile lifted shards to wasm objects (step 2 compile check)
# Flags copied from nfsu1web tools/build_nfsu1_wasm.sh. Runtime headers from work/rt/runtime (HP2-patched).
set -uo pipefail
cd "$(dirname "$0")/.."
GEN="$1"; OUT="$GEN/obj"; mkdir -p "$OUT"
WASI="$PWD/work/toolchain/wasi-sdk-34.0-arm64-macos"; CC="$WASI/bin/clang"; RT="$PWD/work/rt/runtime"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DNFSU2_GUEST_OBJ"
CFLAGS="$TARGET ${OPT:--O1} $FEAT $DEFS -fno-delete-null-pointer-checks -include $RT/web/compat/wasm_plat.h -I$RT/web/compat -I$RT -I$GEN -Wno-unused-label -Wno-parentheses-equality"
J="${J:-$(sysctl -n hw.ncpu)}"
CACHE="$PWD/work/objcache"; mkdir -p "$CACHE"
FLAGSUM=$( (echo "$CFLAGS"; cat "$RT/cpu.h" "$RT/web/compat/wasm_plat.h") | shasum | cut -c1-16)
export CC CFLAGS OUT CACHE FLAGSUM
ls "$GEN"/shard_*.c "$GEN"/fntab.c "$GEN"/impargs.c $( [ -f "$GEN/stubs.c" ] && echo "$GEN/stubs.c") | \
  xargs -P "$J" -n 1 tools/compile_one.sh
echo "objects: $(ls "$OUT"/*.o 2>/dev/null | wc -l)  failures: $(ls "$OUT"/*.err 2>/dev/null | wc -l)"
