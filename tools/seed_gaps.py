#!/usr/bin/env python3
"""Find function starts the pcrecomp catalog missed, for NFSHP2.exe.

  seed_gaps.py <exe> <catalog.json> <out_seeds.json> [--merge prev_seeds.json]

Candidate sources, each validated with pcrecomp's Disassembler.probes_as_function_body:
  imm   - `mov r32/m32, imm32` / `push imm32` in catalogued code whose value lands in an
          uncovered gap of .text (callbacks, thread procs, function pointers)
  gap   - first non-padding byte of each uncovered gap
  data  - dword in a non-code section pointing into an uncovered gap (vtables, tables)
"""
import bisect, json, os, struct, sys
import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.path.normpath(os.path.join(HERE, "..", "pcrecomp"))
for sub in ("pe", "disasm"):
    sys.path.insert(0, os.path.join(PCRECOMP, "tools", sub))
from pe_analyze import analyze_pe  # noqa: E402
import disasm32  # noqa: E402

PAD = {0xCC, 0x90, 0x00}


def main(argv):
    exe, catalog, out = argv[0], argv[1], argv[2]
    merge = argv[argv.index("--merge") + 1:] if "--merge" in argv else []
    data = open(exe, "rb").read()
    cat = json.load(open(catalog))
    info = analyze_pe(exe)
    base = info.image_base
    dis = disasm32.Disassembler(data, base, info.sections)
    cs, ce = info.code_start, info.code_end

    def file_off(va):
        rva = va - base
        for s in info.sections:
            vstart = s.virtual_address if hasattr(s, "virtual_address") else s["virtual_address"]
            raw = s.raw_offset if hasattr(s, "raw_offset") else s.get("raw_offset", s.get("pointer_to_raw_data"))
            rsz = s.raw_size if hasattr(s, "raw_size") else s.get("raw_size", s.get("size_of_raw_data"))
            if vstart <= rva < vstart + rsz:
                return raw + rva - vstart
        return None

    # covered intervals -> gaps
    ivs = sorted((f["address"], f["end"]) for f in cat["functions"] if f["end"] > f["address"])
    gaps, cur = [], cs
    for s, e in ivs:
        if s > cur:
            gaps.append((cur, s))
        cur = max(cur, e)
    if ce > cur:
        gaps.append((cur, ce))
    gaps = [(s, e) for s, e in gaps
            if any(b not in PAD for b in data[file_off(s):file_off(s) + (e - s)])]
    gstarts = [g[0] for g in gaps]

    def in_gap(v):
        i = bisect.bisect_right(gstarts, v) - 1
        return i >= 0 and gaps[i][0] <= v < gaps[i][1]

    catalogued = {f["address"] for f in cat["functions"]}
    cand = {}  # va -> source

    # gap starts
    for s, e in gaps:
        o = file_off(s)
        k = 0
        while k < e - s and data[o + k] in PAD:
            k += 1
        if k < e - s:
            cand.setdefault(s + k, "gap")

    # code immediates
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    for f in cat["functions"]:
        lo, hi = f["address"], f["end"]
        if not (0 < hi - lo <= 0x40000):
            continue
        o = file_off(lo)
        for ins in md.disasm(data[o:o + (hi - lo)], lo):
            if ins.mnemonic in ("mov", "push") and ins.operands and ins.size >= 5:
                op = ins.operands[-1]
                if op.type == capstone.x86.X86_OP_IMM:
                    v = op.imm & 0xFFFFFFFF
                    if cs <= v < ce and v not in catalogued and in_gap(v):
                        cand[v] = "imm"

    # data pointers in non-code sections
    for s in info.sections:
        name = s.name if hasattr(s, "name") else s["name"]
        vstart = s.virtual_address if hasattr(s, "virtual_address") else s["virtual_address"]
        if base + vstart == cs or name.strip("\0") in (".text",):
            continue
        o = file_off(base + vstart)
        if o is None:
            continue
        rsz = s.raw_size if hasattr(s, "raw_size") else s.get("raw_size", s.get("size_of_raw_data"))
        for k in range(0, rsz - 3, 4):
            v = struct.unpack_from("<I", data, o + k)[0]
            if cs <= v < ce and v not in catalogued and in_gap(v):
                cand.setdefault(v, "data")

    seeds, by = [], {}
    for v, src in sorted(cand.items()):
        if dis.probes_as_function_body(v):
            seeds.append({"address": v, "address_hex": f"0x{v:08X}", "note": src})
            by[src] = by.get(src, 0) + 1
    have = {e["address"] for e in seeds}
    for path in merge:
        for e in json.load(open(path)):
            if e["address"] not in have:
                have.add(e["address"])
                seeds.append(e)
    json.dump(seeds, open(out, "w"), indent=1)
    gap_bytes = sum(e - s for s, e in gaps)
    print(f"[*] {len(gaps)} non-padding gaps ({gap_bytes:,} B); candidates {len(cand)}; "
          f"accepted {by}; total seeds {len(seeds)} -> {out}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(sys.argv[1:])
