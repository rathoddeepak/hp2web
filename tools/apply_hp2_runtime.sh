#!/usr/bin/env bash
# Materialize the HP2 runtime tree: pristine nfsu2-web runtime @31da468 + HP2 edits + runtime/*.c overlays.
# Output: work/rt/runtime (regenerated every run; never edit it by hand).
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(git -C nfsu2-web rev-parse HEAD)" = 31da468582c3de14fd6f4233831f62816e2df83d ] || { echo "nfsu2-web not at pinned commit"; exit 1; }
rm -rf work/rt/runtime && mkdir -p work/rt && cp -R nfsu2-web/runtime work/rt/runtime
R=work/rt/runtime
# HP2 .text extent (0x401000 + 0x25a34e) for the dispatch table and coverage
sed -i '' 's/#define DISPATCH_TEXT_SIZE 0x00381af1u/#define DISPATCH_TEXT_SIZE 0x0025c000u/; s/0x00401000u + (3676913u)/0x00401000u + (0x0025c000u)/' $R/cpu.h
sed -i '' 's/#define COV_SIZE 3676913u/#define COV_SIZE 0x0025c000u/' $R/debug.c
# HP2 image end = 0x400000 + SizeOfImage 0x460000
sed -i '' 's/0x00932000u/0x00860000u/g' $R/heap.c
# heap call-site attribution ranges: HP2 .text; CRT range unknown yet -> empty
sed -i '' 's/#define TEXT_HI 0x00782AF1u/#define TEXT_HI 0x0065D000u/; s/#define CRT_LO  0x0075B000u/#define CRT_LO  0u/; s/#define CRT_HI  0x00772000u/#define CRT_HI  0u/' $R/heap.c
# NFSU2 D3DX effect-epoch bump for guest calls into 0x650000..0x690000 (NFSU2 D3DX code; HP2 has game code there)
sed -i '' 's/if (addr - 0x650000u < 0x40000u \&\& addr != 0x0066acd2u \&\& addr != 0x0066ac17u \&\& addr != 0x0065adcbu) g_fx_epoch++;/(void)g_fx_epoch;/' $R/dispatch.c
grep -q "(void)g_fx_epoch;" $R/dispatch.c || { echo "dispatch.c edit failed"; exit 1; }
grep -q "0x0025c000u" $R/cpu.h || { echo "cpu.h edit failed"; exit 1; }
# win32.c: HP2 HLE hook after the runtime's own layers; exe name in GetModuleFileNameA / command line
sed -i '' 's|^int win32_ext_call(CPU\*, const char\*, uint32_t\*);|&\
int hp2_win32_call(CPU*, const char*, uint32_t*);|' $R/win32.c
sed -i '' 's|    else if (win32_ext_call(c, n, \&r)) { /\* second-round layer \*/ }|&\
    else if (hp2_win32_call(c, n, \&r)) { /* HP2 bring-up HLE (runtime/hp2_win32.c) */ }|' $R/win32.c
sed -i '' 's|C:\\\\\\\\NFSU2\\\\\\\\SPEED2.EXE|C:\\\\\\\\NFSU2\\\\\\\\NFSHP2.EXE|; s|"SPEED2.EXE");|"NFSHP2.EXE");|' $R/win32.c
grep -q "hp2_win32_call(c, n" $R/win32.c && grep -q "^int hp2_win32_call" $R/win32.c || { echo "win32.c hook edit failed"; exit 1; }
grep -q 'NFSHP2.EXE"; char ms' $R/win32.c && grep -q '"NFSHP2.EXE");' $R/win32.c || { echo "win32.c exe-name edit failed"; exit 1; }
# com.c: D3D8 interfaces (iface >= 0x100) go to runtime/hp2_d3d8.c; D3D8 vtables are valid objects
sed -i '' 's|^    uint32_t iface=(magic>>16)\&0x1FFFu, m=magic\&0xFFFFu;|&\
    if (iface >= 0x100u){ extern int hp2_d3d8_call(CPU*, uint32_t); return hp2_d3d8_call(c, magic); }|' $R/com.c
sed -i '' 's|    return i > 0 \&\& i < NEL(g_if) \&\& g_vtab\[i\] \&\& MR32(o) == g_vtab\[i\];|    extern int hp2_d3d8_is_vtab(uint32_t, uint32_t);\
    return i > 0 \&\& i < NEL(g_if) \&\& g_vtab[i] \&\& (MR32(o) == g_vtab[i] \|\| hp2_d3d8_is_vtab(i, MR32(o)));|' $R/com.c
grep -q "return hp2_d3d8_call(c, magic)" $R/com.c && grep -q "hp2_d3d8_is_vtab(i, MR32(o))" $R/com.c || { echo "com.c D3D8 edit failed"; exit 1; }
# dispatch.c: optional ring buffer of indirect transfers (runtime/hp2_ring.c, HP2_RING=n)
sed -i '' 's|^void dispatch_indirect(CPU \*c, uint32_t addr){|&\
    { extern void hp2_ring_note(CPU*, uint32_t); hp2_ring_note(c, addr); }|' $R/dispatch.c
grep -q "hp2_ring_note(c, addr)" $R/dispatch.c || { echo "dispatch.c ring edit failed"; exit 1; }
# win32.c: guest backtrace at ExitProcess (bring-up diagnostic)
sed -i '' 's|        fprintf(stderr,"\\n\*\*\* guest ExitProcess(%u) - exiting\\n",ARG(c,0));|&\
        { extern void guest_backtrace(CPU*, int); fprintf(stderr, "    ExitProcess called from 0x%08x\\n", c->eip); guest_backtrace(c, 24); }|' $R/win32.c
grep -q "ExitProcess called from" $R/win32.c || { echo "win32.c ExitProcess bt edit failed"; exit 1; }
# web/gpu_web.c: 16-bit texture formats (expanded to A8R8G8B8 on upload)
.venv/bin/python -I tools/patch_gpu16.py $R >/dev/null || { echo "gpu_web.c 16-bit edit failed"; exit 1; }
# com.c: QueryInterface on DirectInput objects returns the same object (DI7 games ask for
# IDirectInputDevice2A/7A; the DI8 vtable is a superset)
sed -i '' 's/if ((iface==IF_DS || iface==IF_DSBUF) \&\& ppv){/if ((iface==IF_DS || iface==IF_DSBUF || iface==IF_DI8 || iface==IF_DIDEV) \&\& ppv){/' $R/com.c
grep -q "iface==IF_DI8 || iface==IF_DIDEV) && ppv" $R/com.c || { echo "com.c DI QI edit failed"; exit 1; }
# win32.c GetProcAddress: HP2 host exports (runtime/hp2_overrides.c hp2_getproc) before the IAT search
sed -i '' 's|            const char \*want = (const char\*)(g_mem + pname);|&\
            { extern uint32_t hp2_getproc(const char*); r = hp2_getproc(want); }|' $R/win32.c
grep -q "r = hp2_getproc(want);" $R/win32.c || { echo "win32.c GetProcAddress edit failed"; exit 1; }
# watchdog.c: dump the HP2 indirect-transfer ring with every hang report
.venv/bin/python -I tools/patch_watchdog.py $R || { echo "watchdog.c ring edit failed"; exit 1; }
# winsock.c: no LAN hub -> loopback sandbox instead of failing WSAStartup (HP2 asserts on that)
sed -i '' 's|    if (g_ws_mode == NET_VLAN){ int e = vlan_up(); if (e) return WSASYSNOTREADY; }|    if (g_ws_mode == NET_VLAN){ int e = vlan_up(); if (e){ ws_log("HP2: no LAN hub, using the loopback sandbox"); g_ws_mode = NET_LOOP; } }|' $R/winsock.c
grep -q "HP2: no LAN hub" $R/winsock.c || { echo "winsock.c vlan fallback edit failed"; exit 1; }
# win32.c: real-time clock for HP2 (host_us)
.venv/bin/python -I tools/patch_clock.py $R || { echo "win32.c clock edit failed"; exit 1; }
# dispatch.c cpuid: no SSE (bit 25) for HP2, so its PACODE math dispatch takes the x87 paths
# (the SSE / 3DNow! variants in PACODE mix instructions the lifter does not emit)
sed -i '' 's/case 1: c->eax=0x000006d8; c->ebx=0; c->ecx=0; c->edx=0x0383fbff; break;/case 1: c->eax=0x000006d8; c->ebx=0; c->ecx=0; c->edx=0x0183fbffu; break;   \/* HP2: no SSE *\//' $R/dispatch.c
grep -q "0x0183fbffu" $R/dispatch.c || { echo "dispatch.c cpuid edit failed"; exit 1; }
# HP2 overlays
for f in runtime/*.c runtime/*.h; do [ -e "$f" ] && cp "$f" $R/; done
# hp2wsfix per-element HUD layout table, generated from the hp2wsfix checkout (MIT)
.venv/bin/python -I tools/gen_ws_hud.py hp2wsfix/dllmain.cpp $R/hp2ws_hud.inc >/dev/null || { echo "hp2ws HUD table generation failed"; exit 1; }
echo "HP2 runtime ready: $R"
