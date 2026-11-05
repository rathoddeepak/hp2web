#!/usr/bin/env python3
"""Lift the pruned NFSHP2.exe catalog to web-ABI C (nfsu2-web Lifter via nfsu1web's catalog bridge).

  hp2_lift.py --catalog work/analysis/catalog_blocks.final.json --output-dir gen/hp2 [--limit N]

Adapted from nfsu1web tools/lift_catalog.py with the NFSU1 specifics removed (profile,
font/widescreen operand sites). Failures are recorded; with --link-stubs every failed or
unresolved entry becomes an abort stub so the module links (bring-up only).
Output is game-derived: keep it under gen/ (never commit/distribute).
"""
import argparse, ast, hashlib, json, os, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UP = ROOT / "nfsu2-web"
sys.path[:0] = [str(ROOT / "nfsu1web/tools"), str(UP / "tools")]
EXE = ROOT / "work/data/NFSHP2.exe"
EXE_SHA = "3a08f1051319dc83341a63e61e7184496d3b92f7843712b195456d86b7f262c8"
TERMINATORS = ('ret', 'retf', 'jmp', 'int3', 'hlt', 'ud2', 'iret', 'iretd')


def known_import_counts():
    tree = ast.parse((UP / "tools/argcount.py").read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == "KNOWN" for t in node.targets):
            return ast.literal_eval(node.value)
    raise ValueError("KNOWN table not found")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", type=Path, required=True)
    ap.add_argument("--output-dir", type=Path, default=ROOT / "gen/hp2")
    ap.add_argument("--imports", type=Path, default=ROOT / "profiles/hp2-imports.json")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--only", help="comma-separated hex VAs to lift (diagnostic)")
    ap.add_argument("--shard-size", type=int, default=200)
    ap.add_argument("--link-stubs", action="store_true")
    args = ap.parse_args()
    out = args.output_dir.resolve()
    if not out.is_relative_to(ROOT / "gen"):
        ap.error("output must be under gen/")
    if out.exists() and any(out.iterdir()):
        ap.error("output dir not empty")
    image = EXE.read_bytes()
    if hashlib.sha256(image).hexdigest() != EXE_SHA:
        ap.error("NFSHP2.exe is not the expected v242 build")
    catalog = json.loads(args.catalog.read_text())

    os.environ["FLAGOPT"] = "0"
    os.environ["X87PC"] = "1"
    from catalog_bridge import attach_catalog, load_web_lifter
    module = load_web_lifter(UP)
    os.environ["JTSWITCH"] = "1"
    lifter = module.Lifter(str(EXE), comments=False, hooks=[])
    # NFSHP2's code is .text (0x401000..0x65b34e) plus PACODE (0x65c000..0x65cc8c, the CPU-specific math
    # dispatch): treat both as one text range, or every jump inside PACODE is a "branch-out"
    lifter.TSZ = 0x0065d000 - lifter.TVA
    import pefile as _pe
    _p = _pe.PE(str(EXE)); _img = _p.get_memory_mapped_image(); _b = _p.OPTIONAL_HEADER.ImageBase
    lifter.TDATA = bytes(_img[lifter.TVA - _b:lifter.TVA - _b + lifter.TSZ])
    plan = attach_catalog(lifter, catalog, route_catalog_tails=True)
    sys.path.insert(0, str(ROOT / "tools"))
    import hp2_instructions
    hp2_instructions.attach(lifter)
    import hp2_widescreen                      # hp2wsfix sites (runtime/hp2_widescreen.c)
    ws_seen = hp2_widescreen.attach(lifter)
    addresses = [f["address"] for f in plan["functions"]]
    available = set(addresses)

    import pefile
    pe = pefile.PE(data=image)
    entry = pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    if entry not in available:
        raise SystemExit(f"catalog omits entry point 0x{entry:08x}")
    counts = known_import_counts()
    if args.imports.exists():
        for e in json.loads(args.imports.read_text())["entries"]:
            counts[e["name"]] = e["nargs"]
    imports, unknown = [], []
    for dll in pe.DIRECTORY_ENTRY_IMPORT:
        d = dll.dll.decode("latin1")
        for imp in dll.imports:
            name = imp.name.decode("latin1") if imp.name else f"#{imp.ordinal}"
            key = name if imp.name else f"{d.lower()}{name}"
            n = counts.get(key, counts.get(name) if imp.name else None)
            if n is None:
                unknown.append(f"{d}!{name}")
            imports.append((imp.address, d, name, n))

    out.mkdir(parents=True, exist_ok=True)
    selected = addresses[:args.limit] if args.limit else addresses
    if args.only:
        selected = [int(x, 16) for x in args.only.split(",")]
    report = {"catalog_functions": len(addresses), "selected": len(selected), "emitted": 0,
              "failures": [], "unknown_import_argcounts": unknown}
    bodies, shard, unresolved, emitted = [], 0, set(), set()
    # shards are address ranges (16 KB of .text each), so adding a function only changes its own
    # shard and the compile cache (tools/compile_shards.sh) keeps every other object
    cur_key = None
    iso = set()
    isof = ROOT / "profiles/hp2-isolate.txt"
    if isof.exists():
        for line in isof.read_text().splitlines():
            t = line.split("#")[0].split()
            if t: iso.add(int(t[0], 16))
    t0 = time.monotonic()

    def flush():
        nonlocal bodies, shard
        if bodies:
            (out / f"shard_{shard:08x}.c").write_text(
                '/* GAME-DERIVED: local output, DO NOT DISTRIBUTE. */\n'
                '#include "cpu.h"\n#include "decls.h"\n#include <math.h>\n\n' + "\n\n".join(bodies) + "\n")
            bodies = []
            if args.only: shard += 1

    for i, a in enumerate(selected):
        if not args.only and a in iso:
            flush(); cur_key = None; shard = a                      # own shard
        elif not args.only and (a >> 14) != cur_key:
            flush(); cur_key = a >> 14; shard = a                   # named after its first function
        lifter.traps = lifter.truncated = 0
        oc, ob = getattr(lifter, "oob_calls", 0), getattr(lifter, "oob_branch", 0)
        try:
            decoded, _ = lifter.discover(a)
            for ins_list in decoded.values():
                last = ins_list[-1]
                if last.mnemonic not in TERMINATORS and last.address + last.size not in decoded:
                    raise ValueError(f"missing fallthrough block 0x{last.address + last.size:08x}")
            code, targets = lifter.lift(a)
            if lifter.traps or lifter.truncated:
                raise ValueError(f"traps={lifter.traps}, truncated={lifter.truncated}")
            if 'unsupported_insn(' in code:
                import re as _re
                bad = sorted(set(_re.findall(r'unsupported_insn\(c, *0x([0-9a-f]+)u?, *"([^"]*)"', code)))
                raise ValueError("unsupported instruction: " + ", ".join(f"{m}@{va}" for va, m in bad[:6]))
            if getattr(lifter, "oob_calls", 0) != oc or getattr(lifter, "oob_branch", 0) != ob:
                raise ValueError("out-of-text direct transfer")
            unresolved.update(t for t in targets if lifter.in_text(t) and t not in available)
            bodies.append(code); emitted.add(a); report["emitted"] += 1
            if a in iso and not args.only:
                flush(); cur_key = None      # next function starts a new shard (named by its address)
            if args.only and len(bodies) >= args.shard_size:
                flush()
        except Exception as e:  # noqa: BLE001
            report["failures"].append({"address": f"0x{a:08x}", "error": str(e)[:200]})
        if (i + 1) % 2000 == 0:
            print(f"{i + 1}/{len(selected)} lifted; {len(report['failures'])} failed", flush=True)
    flush()
    ws_missing = hp2_widescreen.expected() - ws_seen
    ws_ini = sum(1 for x in ws_seen if isinstance(x, tuple))
    report["widescreen_sites"] = len(hp2_widescreen.expected()) - len(ws_missing)
    report["widescreen_ini_calls"] = ws_ini
    # a site in code the catalog never reached is only reported; one inside lifted code must match
    in_code = {x for x in ws_missing if any(b[0] <= x < b[1] for f in catalog["functions"] for b in f["blocks"])}
    report["widescreen_unreached"] = [f"0x{x:08x}" for x in sorted(ws_missing - in_code)]
    if ws_missing - in_code:
        print(f"hp2ws: sites outside the catalog (never lifted): {report['widescreen_unreached']}")
    if not args.only and not args.limit and (in_code or ws_ini < 12):
        raise SystemExit(f"hp2ws: sites not lifted: {sorted(hex(x) for x in in_code)}, ini calls {ws_ini}")
    report["unresolved_direct_targets"] = [f"0x{x:08x}" for x in sorted(unresolved)]
    if args.link_stubs:
        available |= unresolved
        stubs = sorted(available - emitted)
        (out / "stubs.c").write_text("\n".join(
            ['/* GAME-DERIVED bring-up stubs. DO NOT DISTRIBUTE. */', '#include "cpu.h"', '#include "decls.h"'] +
            [f'void sub_{x:08x}(CPU *c){{ unsupported_insn(c, 0x{x:08x}u, "lift-failed"); }}' for x in stubs]) + "\n")
        report["stubbed"] = len(stubs)
    decl = ['#ifndef HP2_DECLS_H', '#define HP2_DECLS_H', '#include "cpu.h"',
            'void unsupported_insn(CPU *, uint32_t, const char *);', 'void cpuid_shim(CPU *);',
            'uint64_t rdtsc_shim(void);'] + [f"void {h}(CPU *);" for h in hp2_widescreen.HOOKS] + ['extern int g_hp2ws_on;'] + [f"void sub_{x:08x}(CPU *);" for x in sorted(available)] + ['#endif']
    (out / "decls.h").write_text("\n".join(decl) + "\n")
    tab = ['#include "decls.h"', f'const uint32_t g_fn_count = {len(available)};', 'const uint32_t g_fn_addr[] = {']
    tab += [f'0x{x:08x}u,' for x in sorted(available)] + ['};', 'const guest_fn_t g_fn_ptr[] = {']
    tab += [f'sub_{x:08x},' for x in sorted(available)] + ['};']
    (out / "fntab.c").write_text("\n".join(tab) + "\n")
    imp = ['#include <stdint.h>', 'typedef struct { uint32_t iat; const char *dll, *name; int nargs; } ImpEnt;',
           f'const uint32_t g_imp_count = {len(imports)};', 'const ImpEnt g_imp[] = {']
    imp += [f'{{0x{va:08x}u,{json.dumps(d)},{json.dumps(n)},{-1 if c is None else c}}},' for va, d, n, c in imports]
    (out / "impargs.c").write_text("\n".join(imp + ['};']) + "\n")
    report["seconds"] = round(time.monotonic() - t0, 1)
    (out / "lift-report.json").write_text(json.dumps(report, indent=1) + "\n")
    print(json.dumps({k: (v if not isinstance(v, list) else len(v)) for k, v in report.items()}, indent=1))


if __name__ == "__main__":
    main()
