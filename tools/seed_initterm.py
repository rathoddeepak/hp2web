#!/usr/bin/env python3
"""Seed every function pointer in the CRT _initterm tables (C/C++ static initializers).

  seed_initterm.py <exe> <out_seeds.json> [--merge seeds.json ...]

_initterm (NFSHP2 v242: 0x5c52fd) is called as `push end; push start; call _initterm`; each
non-null table entry is a function the CRT calls at startup. pcrecomp's body probe rejects some
of them (e.g. ones starting with an x87 fld), so they are seeded without probing.
"""
import json, re, struct, sys
import capstone, pefile

INITTERM = 0x5C52FD


def main(argv):
    exe, out = argv[0], argv[1]
    merge = argv[argv.index("--merge") + 1:] if "--merge" in argv else []
    pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
    text = pe.sections[0]; t0 = text.VirtualAddress; t = img[t0:t0 + text.Misc_VirtualSize]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    tables = set()
    for m in re.finditer(rb"\xe8", t):
        k = m.start()
        if k + 5 > len(t):
            break
        a = base + t0 + k
        if (a + 5 + struct.unpack_from("<i", t, k + 1)[0]) & 0xFFFFFFFF != INITTERM:
            continue
        pushes = [i for i in md.disasm(img[a - base - 10:a - base + 5], a - 10)
                  if i.mnemonic == "push" and i.op_str.startswith("0x")]
        if len(pushes) >= 2:
            tables.add((int(pushes[-1].op_str, 16), int(pushes[-2].op_str, 16)))
    seeds, have = [], set()
    for lo, hi in sorted(tables):
        for k in range(lo, hi, 4):
            v = struct.unpack_from("<I", img, k - base)[0]
            if v and v not in have:
                have.add(v); seeds.append({"address": v, "address_hex": f"0x{v:08X}", "note": "initterm"})
    n = len(seeds)
    for path in merge:
        for e in json.load(open(path)):
            if e["address"] not in have:
                have.add(e["address"]); seeds.append(e)
    json.dump(seeds, open(out, "w"), indent=1)
    print(f"[*] {len(tables)} _initterm tables, {n} initializers; total seeds {len(seeds)} -> {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
