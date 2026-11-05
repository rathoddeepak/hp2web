#!/usr/bin/env python3
"""seed_from_logs.py <log>... : add "[UNRESOLVED INDIRECT CALL] target=0x..." targets that decode as code
(inside .text/PACODE) to profiles/hp2-seeds.json (note "runtime: ..."), which step 1 and prune keep."""
import json, re, sys, pathlib
import capstone, pefile
ROOT = pathlib.Path(__file__).resolve().parents[1]
pe = pefile.PE(str(ROOT / "work/data/NFSHP2.exe")); b = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
p = ROOT / "profiles/hp2-seeds.json"; seeds = json.load(open(p)); have = {e["address"] for e in seeds}
new = []
for log in sys.argv[1:]:
    for t, ret in re.findall(r"UNRESOLVED INDIRECT (?:CALL|JUMP|TAIL)\] target=0x([0-9a-f]{8})\s+return=0x([0-9a-f]{8})", open(log, errors="replace").read()):
        a = int(t, 16)
        if not (0x401000 <= a < 0x65d000) or a in have: continue
        ins = list(md.disasm(img[a - b:a - b + 32], a))
        if len(ins) < 3: continue
        have.add(a); new.append({"address": a, "address_hex": f"0x{a:08X}", "note": f"runtime: unresolved indirect call (return 0x{ret})"})
seeds += new; json.dump(seeds, open(p, "w"), indent=1)
print(f"[*] {len(new)} runtime seeds added: {[e['address_hex'] for e in new]}")
