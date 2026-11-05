#!/usr/bin/env bash
# Step 3: build the HP2 runtime + link with the lifted shards (gen/hp2/obj) into out/hp2.wasm.
# Link flags follow nfsu1web tools/build_nfsu1_wasm.sh.
set -uo pipefail
cd "$(dirname "$0")/.."
true
ROOT="$PWD"; GEN="$ROOT/gen/hp2"; OUT="$ROOT/out"; mkdir -p "$OUT/rt"
WASI="$ROOT/work/toolchain/wasi-sdk-34.0-arm64-macos"; CC="$WASI/bin/clang"; R="$ROOT/work/rt/runtime"
OPT="${OPT:--O1}"; GUEST_END=671088640; INITIAL_MB="${INITIAL_MB:-1024}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DNFS_ENHANCED=0"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include $R/web/compat/wasm_plat.h -I$R/web/compat -I$R -I$GEN -w"
RT="${RT:-mem dispatch genexec heap watchdog win32 win32_ext com debug res fileio lifecycle module d3dx_hle vsemu dinput_kbd dinput_pad dikeys keyboard winsock modheap autotest horn carsfx hp2_overrides hp2_link_stubs hp2_win32 hp2_d3d8 hp2_ring hp2_main_web}"
WEBSKIP=" main_web roam_web "
WEB="$(cd $R/web && ls *.c | sed 's/\.c$//')"
rm -f "$OUT"/rt/*.o; fail=0; pids=""
for f in $RT; do src="$R/$f.c"; [ -f "$src" ] || src="$R/web/$f.c"
  ( $CC $CFLAGS -c -o "$OUT/rt/$f.o" "$src" 2> "$OUT/rt/$f.err" || echo "COMPILE FAIL $f" ) & pids="$pids $!"; done
for f in $WEB; do case "$WEBSKIP" in *" $f "*) continue;; esac
  case "$f" in thread_exit) fl="$TARGET $OPT $FEAT";; *) fl="$CFLAGS";; esac
  ( $CC $fl -c -o "$OUT/rt/web_$f.o" "$R/web/$f.c" 2> "$OUT/rt/web_$f.err" || echo "COMPILE FAIL web/$f" ) & pids="$pids $!"; done
wait
find "$OUT/rt" -name '*.err' -size 0 -delete
# HP2_WRAP="4cf7a0 5fffa0 ...": log entry/exit of those guest functions (link-time --wrap, diagnostic)
WRAPFLAGS=()
if [ -n "${HP2_WRAP:-}" ]; then
  { echo '#include "cpu.h"'; echo '#include <stdio.h>'; echo "#define HP2_WRAP_MAX ${HP2_WRAP_MAX:-50}"; echo "#define PEEK ${HP2_WRAP_PEEK:-0}u"
    POPS=" $(.venv/bin/python -I tools/ret_pops.py work/data/NFSHP2.exe work/analysis/catalog_blocks.final.json $HP2_WRAP) "
    for a in $HP2_WRAP; do f="sub_$(printf %08x 0x$a)"; pop=$(echo "$POPS" | tr ' ' '\n' | grep "^$a:" | cut -d: -f2)
      echo "void __real_$f(CPU*); static int n_$f;"
      echo "void __wrap_$f(CPU *c){ int k=++n_$f; if(k<=HP2_WRAP_MAX) fprintf(stderr,\"[wrap] -> $f #%d esp=%08x ecx=%08x ret=%08x a=%08x %08x %08x\\n\",k,c->esp,c->ecx,MR32(c->esp),MR32(c->esp+4),MR32(c->esp+8),MR32(c->esp+12));"
      echo "  if(${HP2_WRAP_DUMP:-0} && k<=HP2_WRAP_MAX){ uint32_t p=MR32(c->esp+4); fprintf(stderr,\"[wrap]    $f arg0 %08x:\",p); for(int q=0;q<24;q++) fprintf(stderr,\" %08x\",MR32(p+4u*q)); fprintf(stderr,\"\\n\"); }"
      echo "  uint32_t e0=c->esp, r0=MR32(c->esp), sb=c->ebx,ss=c->esi,sd=c->edi,sp=c->ebp; __real_$f(c); if(k<=HP2_WRAP_MAX) fprintf(stderr,\"[wrap] <- $f #%d eax=%08x peek=%08x\\n\",k,c->eax,PEEK?MR32(PEEK):0);"
      echo "  if(($pop)>=0 && c->esp-e0!=(uint32_t)($pop)) fprintf(stderr,\"[wrap] !! $f #%d STACK: popped %d bytes, its ret pops %d (called from %08x)\\n\",k,(int)(c->esp-e0),$pop,r0);"
      echo "  if(sb!=c->ebx||ss!=c->esi||sd!=c->edi||sp!=c->ebp) fprintf(stderr,\"[wrap] !! $f #%d CLOBBERED callee-saved: ebx %08x->%08x esi %08x->%08x edi %08x->%08x ebp %08x->%08x\\n\",k,sb,c->ebx,ss,c->esi,sd,c->edi,sp,c->ebp); }"
      WRAPFLAGS+=("-Wl,--wrap=$f"); done; } > "$OUT/rt/hp2_wraps.c"
  $CC $CFLAGS -c -o "$OUT/rt/hp2_wraps.o" "$OUT/rt/hp2_wraps.c" || exit 1
else rm -f "$OUT/rt/hp2_wraps.o"; fi
OBJS=("$GEN"/obj/*.o "$OUT"/rt/*.o)
rm -f "$OUT/hp2.wasm"
$CC $TARGET $OPT $FEAT -o "$OUT/hp2.wasm" "${OBJS[@]}" \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=$(( INITIAL_MB * 1048576 )),--global-base=$GUEST_END \
  -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first -Wl,--error-limit=0 "${WRAPFLAGS[@]}" \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks > "$OUT/link.log" 2>&1
if [ -f "$OUT/hp2.wasm" ]; then echo "link OK: $OUT/hp2.wasm ($(( $(wc -c < "$OUT/hp2.wasm") / 1048576 )) MB)"
else echo "link FAILED: $(grep -c 'undefined symbol' "$OUT/link.log") undefined symbols, $(grep -c 'duplicate symbol' "$OUT/link.log") duplicates (see out/link.log)"; fi
