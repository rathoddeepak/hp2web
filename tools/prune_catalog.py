#!/usr/bin/env python3
"""Keep only catalog functions reachable from real roots; drop SafeDisc filler.

  prune_catalog.py <exe> <catalog.json> <out_catalog.json> [--report out_report.json]

NFSHP2.exe (v242) has SafeDisc anti-disassembly filler between functions:
`call <trampoline>` stubs (push ecx; push eax; call T; T: mov eax,K; pop ecx; jmp [ecx+K])
into an absent SafeDisc DLL, plus copied code fragments and junk opcodes. Filler is only
referenced by other filler, so it falls out of a reachability pass.

Roots: PE entry point, every dword in a non-code section that points into .text
(vtables, callback/EH tables), every `mov/push imm32` into .text from reachable code.
Edges: rel32/rel8 call/jmp/jcc targets, decoded over each reachable function's extent.
"""
import bisect, json, struct, sys, collections
import capstone, pefile


def main(argv):
    exe, catp, outp = argv[:3]
    rep = argv[argv.index("--report") + 1] if "--report" in argv else None
    keepf = argv[argv.index("--keep-seeds") + 1] if "--keep-seeds" in argv else None
    pe = pefile.PE(exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    cat = json.load(open(catp))
    cs, ce = cat["code_start"], cat["code_end"]
    fns = sorted(cat["functions"], key=lambda f: f["address"])
    A = [f["address"] for f in fns]

    def owner(va):
        # aliases overlap their parents: walk back to every function that could contain va,
        # prefer the outermost one (smallest start)
        i = bisect.bisect_right(A, va) - 1
        best = None
        steps = 0
        while i >= 0 and va - fns[i]["address"] <= 0x40000 and steps < 64:
            steps += 1
            f = fns[i]
            if f["address"] <= va < max(f["end"], f["address"] + 1):
                best = i
            i -= 1
        return best

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    def decode(f):
        """Instructions of f: its saved blocks when the catalog has them (extents can be
        clamped short of the real body), else a linear decode of [address, end)."""
        spans = [(bl[0], bl[1]) for bl in f["blocks"]] if f.get("blocks") else [(f["address"], f["end"])]
        for lo, hi in spans:
            if 0 < hi - lo <= 0x40000:
                yield from md.disasm(img[lo - base:hi - base], lo)

    roots = collections.Counter()
    entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    roots[entry] = 1
    for s in pe.sections:
        sva = base + s.VirtualAddress
        if sva <= cs < sva + s.Misc_VirtualSize:
            continue
        lo = s.VirtualAddress
        hi = lo + min(s.Misc_VirtualSize, s.SizeOfRawData)
        for k in range(lo, hi - 3, 4):
            v = struct.unpack_from("<I", img, k)[0]
            if cs <= v < ce:
                roots[v] += 1

    start_idx = {f["address"]: i for i, f in enumerate(fns)}
    # explicit seeds (jump-table arms, _initterm entries, code immediates) are roots: an indirect
    # jmp [table] inside .text is not followed by the edge scan
    if keepf:
        for e in json.load(open(keepf)):
            if e.get("note", "").split("@")[0] in ("jumptable", "initterm", "code immediate", "switch arm", "branch-out", "vtable") or e.get("note", "").startswith("runtime"):
                roots[e["address"]] += 1
    reach = set()
    work = [i for v in roots for i in (start_idx.get(v), owner(v)) if i is not None]
    edges = 0

    def grow(work):
      nonlocal edges
      while work:
          i = work.pop()
          if i in reach:
              continue
          reach.add(i)
          f = fns[i]
          for ins in decode(f):
              for op in ins.operands:
                  if op.type == capstone.x86.X86_OP_IMM:
                      v = op.imm & 0xFFFFFFFF
                      if cs <= v < ce and (ins.group(capstone.x86.X86_GRP_JUMP) or
                                           ins.group(capstone.x86.X86_GRP_CALL) or ins.size >= 5):
                          for j in (start_idx.get(v), owner(v)):
                              if j is not None and j not in reach:
                                  edges += 1
                                  work.append(j)

    grow(work)

    # Keep unreferenced-but-plausible functions too (dead code or computed calls we can't see):
    # 16-byte aligned (Intel C++ aligns every function), not a SafeDisc stub, no junk opcodes.
    BAD = {"hlt", "in", "out", "insb", "insd", "outsb", "outsd", "les", "lds", "into", "iretd",
           "arpl", "bound", "popfd", "pushfd", "sahf", "lahf", "aaa", "aas", "daa", "das",
           "aam", "aad", "salc", "int1", "cli", "sti", "retf", "enter"}
    dead = set()
    for i, f in enumerate(fns):
        if i in reach or f["address"] % 16 or f["entry_kind"] != "start":
            continue
        a = f["address"]
        ins = list(md.disasm(img[a - base:a - base + 32], a))[:4]
        if not ins or (ins[0].mnemonic == "call" and img[a - base] == 0xE8):
            continue
        if any(x.mnemonic in BAD for x in ins):
            continue
        dead.add(i)

    # entries nested inside reachable code (aliases, switch arms, EH funclets) are dispatch
    # entries of live code: keep them
    rmerged = []
    for f in sorted((fns[i] for i in reach), key=lambda f: f["address"]):
        spans = [(bl[0], bl[1]) for bl in f["blocks"]] if f.get("blocks") else [(f["address"], f["end"])]
        for lo, hi in sorted(spans):
            if hi <= lo:
                continue
            if rmerged and lo <= rmerged[-1][1]:
                rmerged[-1][1] = max(rmerged[-1][1], hi)
            else:
                rmerged.append([lo, hi])
    rmerged.sort()
    rstarts = [m[0] for m in rmerged]
    nested = set()
    for i, f in enumerate(fns):
        if i in reach:
            continue
        j = bisect.bisect_right(rstarts, f["address"]) - 1
        if j >= 0 and rmerged[j][0] <= f["address"] < rmerged[j][1]:
            nested.add(i)
    # anything kept must have its callees kept: grow again from dead + nested (reach is shared)
    before = set(reach)
    grow(list(dead | nested))
    grown = reach - before
    keep = reach | dead | nested
    kept = [fns[i] for i in sorted(keep)]

    # union byte coverage + unresolved edges from reachable code
    # union of kept extents for membership
    merged = []
    spans = sorted(sp for f in kept for sp in
                   ([(bl[0], bl[1]) for bl in f["blocks"]] if f.get("blocks") else [(f["address"], f["end"])])
                   if sp[1] > sp[0])
    for lo, hi in spans:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    mstarts = [m[0] for m in merged]
    def kowner(v):
        i = bisect.bisect_right(mstarts, v) - 1
        return i >= 0 and merged[i][0] <= v < merged[i][1]
    unresolved = collections.Counter()
    for i in sorted(keep):
        f = fns[i]
        for ins in decode(f):
            if ins.group(capstone.x86.X86_GRP_JUMP) or ins.group(capstone.x86.X86_GRP_CALL):
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_IMM:
                        v = op.imm & 0xFFFFFFFF
                        if cs <= v < ce and not kowner(v):
                            unresolved[v] += 1
    dropped = [fns[i] for i in range(len(fns)) if i not in keep]
    out = dict(cat)
    out["functions"] = kept
    covered = sum(hi - lo for lo, hi in merged)
    out["stats"] = dict(cat.get("stats", {}), functions=len(kept), covered_bytes=covered,
                        pruned_from=len(fns))
    json.dump(out, open(outp, "w"))
    stub = 0
    for f in dropped:
        d = img[f["address"] - base:f["address"] - base + 5]
        if d[:1] == b"\xe8":
            stub += 1
    al = collections.Counter("a16" if f["address"] % 16 == 0 else "other" for f in dropped)
    print(f"[*] callees of kept dead/nested code added: {len(grown)}")
    print(f"[*] reachable {len(reach)}, nested entries {len(nested)}, plausible-dead kept {len(dead)}; "
          f"unresolved edges from reachable code: {len(unresolved)} targets "
          f"{[hex(v) for v, _ in unresolved.most_common(8)]}")
    print(f"[*] roots {len(roots)}; kept {len(kept)} / {len(fns)} functions; "
          f"dropped {len(dropped)} (start with CALL: {stub}; alignment {dict(al)}); "
          f"kept bytes (union) {covered:,} / {ce - cs:,} = {100 * covered / (ce - cs):.1f}% of .text")
    if rep:
        json.dump({"unresolved": [f"0x{v:08X}" for v in sorted(unresolved)],
                   "dropped": [{"address": f"0x{f['address']:08X}", "end": f"0x{f['end']:08X}",
                                "entry_kind": f["entry_kind"]} for f in dropped]},
                  open(rep, "w"), indent=0)


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(sys.argv[1:])
