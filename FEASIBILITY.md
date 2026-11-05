# HP2 → browser (WASM) feasibility notes — 2026-11-05

Workspace: ~/dev/hp2web (hp2wsfix, nfsu1web, nfsu2-web, pcrecomp, nfsu1-recompiled)

## Pipeline used by nfsu1web
1. pcrecomp (Python, capstone/pefile): recursive-descent disasm of Speed.exe → function catalog (NFSU1: 23,617 functions).
2. Lift catalog blocks to C (explicit CPU struct), compile with WASI SDK 34 → .wasm.
3. Runtime from th0mk/nfsu2-web: HLE Win32 (win32.c), COM/D3D9 (com.c ~4.9k lines),
   CPU vertex-shader interpreter (vsemu.c), WebGPU backend (web/gpu_web.c), DInput, audio.
4. Rust local server serves game files; user supplies their own install.
5. Pinned to one exe hash (NFSU1 1.4).

## Reusable for HP2
- pcrecomp catalog + lifter: game-agnostic.
- Lifted-CPU runtime, dispatch, heap, file I/O, DInput, WebGPU backend: mostly reusable.

## Gaps for HP2
- **D3D8**: nothing in pcrecomp / nfsu2-web / nfsu1web handles it. Need an IDirect3D8/Device8 HLE
  (map onto existing D3D9 COM layer; D3D8 shader bytecode is close to vs_1_1, handled by vsemu).
- **Game-specific code**: nfsu2-web runtime has ~573 hardcoded NFSU2 addresses (overrides,
  widescreen, menus). These must be stripped/rewritten for HP2.
- **Audio/movies**: HP2 uses different EA audio/video paths than NFSU — unverified.
- **Exe version**: pin to HP2 v242 (hp2wsfix's target).

## hp2wsfix port size
dllmain.cpp 2,656 lines: 66 MakeCALL, 57 WriteMemory, 11 MakeNOP, 2 MakeJMP, addresses in
HP2_242_Addresses.h (161 lines). nfsu1web already does the same for NFSU's widescreen fix
(runtime/nfsu1_widescreen.c) — same pattern applies: hooks become catalog overrides.

## Next (blocked on Speed.exe v242)
- sha256 + PE imports (confirm d3d8.dll, dinput8, dsound, etc.)
- run pcrecomp pe_analyze + disasm32, measure coverage / indirect-jump problems

## Results on the user's exe (2026-11-05)
- File: work/data/NFSHP2.exe (2,965,504 B), sha256 3a08f1051319dc83341a63e61e7184496d3b92f7843712b195456d86b7f262c8
- Not SafeDisc-encrypted (normal sections, no stxt*); extra PACODE section (0x65c000, 3 KB) — likely the no-CD patch.
- **v242 confirmed**: all 66 hp2wsfix MakeCALL sites are E8 CALLs in this exe.
- Imports: d3d8 (Direct3DCreate8), DINPUT (DirectInputCreateA — DInput 7, not 8),
  DSOUND (ordinals #1/#6), WINMM, WS2_32 (29), IMM32, KERNEL32 147, USER32 71, GDI32, ADVAPI32, SHELL32.
- pcrecomp disasm32 (2m22s): 27,954 functions, 1.67M instructions, 93.3% code coverage.
  Uncovered: ~35 KB padding + ~130 KB real code in 4,744 gaps (e.g. strlen @0x65b336,
  prologues @0x607fc6) — functions reached only via vtables/indirect calls. Needs a seeding pass
  (prologue scan / vtable scan / runtime-trace seeding, like nfsu1web's repair_catalog_callbacks.py).
- Catalog: work/analysis/catalog.json

## Step 1 — catalog coverage (done 2026-11-05)
Run: `tools/step1_catalog.sh` → `work/analysis/catalog.final.json`
- **SafeDisc filler found**: the exe is unwrapped but still carries SafeDisc anti-disassembly
  filler in inter-function padding: `call <trampoline>` stubs (families via 0x401489→0x402403 and
  0x401ec9→0x402d03, `jmp [ret+K]` into an absent DLL @0x1000xxxx), copied code fragments,
  junk opcodes. Only filler references filler; no real code reaches it → not needed at runtime.
- **Compiler**: Intel C++ (PACODE section, FPU-CW init); real functions are 16-byte aligned.
- Raw pcrecomp coverage numbers (93%→96.5% with seeds) were inflated by filler.
- After reachability pruning (roots = entry + all .text pointers in data sections + code immediates):
  - 25,521 functions kept = 19,223 reachable + 4,073 nested entries (aliases/arms/funclets)
    + 2,225 plausible dead (aligned, clean prologue, unreferenced — kept as insurance)
  - 7,247 junk entries dropped (852 SafeDisc stubs)
  - real code = 87.7% of .text; the rest is filler/padding
  - **0 unresolved direct call/jmp targets from reachable code** (2 hits were junk after a ret)
- Remaining risk: indirect calls through computed pointers not stored in data sections.
  Handle at runtime: log unresolved ICALL/ITAIL → pcrecomp seed_from_log.py → re-run.
- Caveat: pcrecomp clamps some extents past the final `ret`, so linear decoding over an extent
  can read filler. The lifter must follow blocks, not extents.

## Step 2 — lift to C + compile to wasm (done 2026-11-05)
Run: `tools/step1_catalog.sh && tools/step2_lift.sh` → `gen/hp2/` (game-derived, never distribute)
- Lifter: nfsu2-web tools/lift.py @31da468 via nfsu1web catalog_bridge (block-exact decode),
  + `tools/hp2_instructions.py` (lock prefix dropped — guest threads run one at a time;
  fnstenv/fldenv). Import argcounts: `profiles/hp2-imports.json` (74 entries).
- Catalog fix found here: pcrecomp clamps function extents short of their blocks; pruning must
  decode blocks and grow callees of everything kept → final 26,338 functions, 0 unresolved edges.
- Lift: 26,230 / 26,338 emitted (99.6%). 108 failures = junk/dead entries + AMD 3DNow!/SSE paths
  (0x626340, 0x62b7a0, 0x62bca0, PACODE 0x65c0xx). Need cpuid_shim to report no 3DNow!.
  110 abort stubs (failures + 2 direct targets that land mid-instruction: 0x447708, 0x486ba8).
- Compile: 135 objects, 0 failures, WASI SDK 34, flags from nfsu1web; runtime copy at
  work/rt/runtime with DISPATCH_TEXT_SIZE = 0x25a34e (HP2 .text).
- Symbols the lifted code needs from the runtime: dispatch_indirect, dispatch_tail, g_dtab,
  g_guest, cpuid_shim, rdtsc_shim, unsupported_insn, __indirect_function_table + libm
  (sin cos tan atan2 exp2 log2 fmod ldexp).

## Step 3 — runtime bring-up (in progress, 2026-11-05)
Build: `tools/step3_build.sh` → `out/hp2.wasm`; run headless: `NFSU1_TIMEOUT_MS=20000 node tools/run_node.mjs`
- Runtime tree = pinned nfsu2-web runtime + `tools/apply_hp2_runtime.sh` edits + `runtime/hp2_*.c`:
  - cpu.h/debug.c: dispatch/coverage sized to HP2 .text (0x25a34e); heap.c: image end 0x860000;
    dispatch.c: NFSU2 D3DX effect-epoch range removed; win32.c: HP2 HLE hook + exe name NFSHP2.EXE.
  - `hp2_main_web.c` (entry 0x005c76c1), `hp2_overrides.c` (none yet), `hp2_win32.c` (HP2-only imports),
    `hp2_link_stubs.c` (NFSU2 features off: widescreen, hfr, enhance, headlights, roam, career, server.dll).
  - Linked from upstream: mem dispatch genexec heap watchdog win32 win32_ext com debug res fileio lifecycle
    module d3dx_hle vsemu dinput_kbd dinput_pad dikeys keyboard winsock modheap autotest horn carsfx + web/*.
- First run: PE loads, 291 imports bound, CRT startup runs, reaches WinMain (0x537dc0).
  Found: 11 _initterm initializers missing from the catalog (pcrecomp probe rejects fld-first bodies)
  → `tools/seed_initterm.py` added to step 1. Crash after unimplemented OpenMutexA /
  SHGetSpecialFolderPathA / SystemParametersInfoA → implemented in hp2_win32.c.
- Second run (after initterm seeds + HLE): all 471 static initializers run; WinMain does the
  single-instance check, creates "EA Games/Need For Speed Hot Pursuit 2" (→ save overlay), starts
  thread 0x6446a3 + timeSetEvent timers (callback 0x5dba10), reads gamma/GetDeviceCaps, then calls
  **Direct3DCreate8** (from 0x5509e1, in sub_005506b0) → NULL → crash. Startup now reaches the
  graphics layer: **step 3 bring-up milestone reached; step 4 (D3D8) is the blocker.**
- Open items noted: CreateFileA("NUL") is not mapped (CRT/debug writes, harmless so far);
  cpuid_shim must report no 3DNow! (not yet verified).

## Step 4 — Direct3D 8 (in progress, 2026-11-05)
- `runtime/hp2_d3d8.c`: D3D8 COM layer over com.c's D3D9 bridge. D3D8 objects = com.c objects with a
  D3D8 vtable (magic iface 0x100+k); com_call forwards iface>=0x100, com_valid accepts D3D8 vtables
  (patched by apply_hp2_runtime.sh). Each D3D8 call → D3D9 call(s) via a pushed D3D9 frame.
  Covers IDirect3D8, Device8 (97 methods), Texture8/CubeTexture8, VB/IB, Surface8, SwapChain8:
  PRESENT_PARAMETERS/SURFACE_DESC/ADAPTER_IDENTIFIER/CAPS conversions, SetStreamSource/SetIndices
  base vertex, TSS→sampler-state split, D3D8 vertex declarations → D3DVERTEXELEMENT9 + dcl injection
  into vs.1.x bytecode, DWORD state-block tokens, GetContainer (com.c lacks it).
  Self-checks: esp balance after every D3D9 call, callee-saved register check. HP2_D3D8TRACE=n.
- Result: Direct3DCreate8 → 2× CreateDevice (640x480 probe, 800x600) → programmable VS + PS created →
  textures created/locked/converted by the static D3DX8 code → first Clear + Present.
- Bug found on the way: missing GetContainer left a stale stack pointer as the "texture" → D3DX called
  methods past the IDirect3D8 vtable → 40-byte stack imbalance → garbage list pointer → 64 MB alloc
  → EAGL fatal → exit(0). Fixed.
- Debug tooling added: `HP2_WRAP="<hex fns>"` (+HP2_WRAP_MAX, HP2_WRAP_PEEK) link-time entry/exit
  wrappers with stack-balance (vs. the function's `ret N`) and callee-saved checks; `HP2_RING=n`
  indirect-transfer ring; ExitProcess backtrace; tools/ret_pops.py.
- **Current blocker**: after the first Present the main thread spins in sub_0055d8a0's loop
  (0x55d979..0x55d9df, EAGL technique setup) — count from sub_005695c0 likely garbage. No game data file
  has been opened yet (eaglrm.o, FrontEnd, ...). Next: find where that count comes from.
- Known gaps: R5G6B5 (fmt 23) textures unsupported by com.c's texture path ("[tex] UNSUPPORTED");
  no renderer attached in Node (draws dropped) — real rendering needs the browser/WebGPU host.

## Milestone — the intro plays in the browser (2026-11-05)
`node tools/play_server.mjs` → http://127.0.0.1:8765/ (Brave/Chrome): EA logo + full HP2 intro movie render through
WebGPU, stable for 90 s+. Headless check: `node tools/browser_test.mjs 60 work/browserN` (screenshots).
Fixes on the way (all in runtime/hp2_*.c or tools/apply_hp2_runtime.sh patches):
- memcpy/memset jump tables (incl. negative-indexed, entry-0 overlapping) → tools/seed_jumptables.py closure
- ValidateDevice (1 pass), GetContainer, CreateImageSurface size/format, CopyRects (com.c has no UpdateSurface;
  done through lock buffers, DXT block-aware), 16-bit texture formats expanded to A8R8G8B8
- Win2kHook.dll exports as host functions at int3 padding (dispatch_patch), GetProcAddress hook
- WSAStartup: no LAN hub → loopback sandbox (HP2 asserts on WSA failure)
- EA task pump sub_005d8a00 isolated in its own shard + --wrap → guest_fair_yield (livelock with file threads)
- real-time host clock (NFSU2 vclock starved HP2's busy-wait sleeps)
- fixed-function vertex processing in hp2_d3d8.c: FVF draws (XYZRHW via viewport, XYZ via World*View*Proj)
  converted to a canonical clip-space declaration (com.c's no-shader path assumed NFSU2's layout)
- Node runner: fd_readdir implemented
Build loop: shards by 16 KB address range + content-hash object cache → relift+compile+link ≈ 3 min.
Next: wndproc 0x556f80 (address built at runtime) seeded; input (keys/pad) to leave the intro → menu.

## Milestone — front end renders (2026-11-05)
Intro skippable with Escape (DirectInput keyboard via the page works); title screen and the front-end
background (logo, car, frame) render correctly. Fixes since the intro milestone:
- PACODE (0x65c000..0x65cc8c) is a second code section: lifter TSZ/TDATA and runtime dispatch range
  extended to 0x65d000 (all CPU-specific math routines now lift); cpuid reports no SSE
- runtime-discovered entries: tools/seed_from_logs.py → profiles/hp2-seeds.json (wndproc 0x556f80, 0x48e6c0, ...)
- branch-out targets: tools/seed_branchouts.py / tools/converge_branchouts.sh
- com.c ignores SetViewport: XYZRHW mapped against the full target, XYZ gets the viewport folded in
- dev knobs: page ?env=NAME=value (dist/test/dev.json), HP2_FFLOG (FF draw log), NFSU2_INPUT_SCRIPT in Node
Open: front-end TEXT is invisible (vertex colour 0xff000000 → likely an unsupported texture-stage colour op in
com.c's fixed-function pixel path); a dialog with an input field waits on it. Next: inspect TSS state at text draws.

## Milestone — front-end text (2026-11-05)
Cause: com.c has no DrawIndexedPrimitiveUP (silent no-op) — HP2 draws all its text and several panels with it.
hp2_d3d8.c now expands the indices into a plain vertex list and draws with DrawPrimitiveUP (FF path,
FF DrawIndexedPrimitive from VB/IB, and programmable-shader path). Title screen ("Press Any Key", ©) and main
menu (Continue / Quick Race / Options, Exit / Back / Next) render with text.
Debug aids added: HP2_FFLOG=1|2|3 (draw log incl. TSS/RS state, 3 = whole frame every 20000 presents),
HP2_TEXPEEK=<tex> (lock-buffer alpha stats), HP2_DEFS=-D... experiments (tools/step3_build.sh).
Open: the menu's central box (likely a 3D car preview / render target) is black.

## Milestone: playable race (2026-11-05)
The user played a race locally in Brave: front end → Car Select → race. Remaining: sound check in race, gamepad (DI7 device types), ScreenToClient, widescreen (hp2wsfix patches as overrides), performance.
