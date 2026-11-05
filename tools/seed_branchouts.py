#!/usr/bin/env python3
"""seed_branchouts.py <exe> <lift-report.json> <seeds.json> : add the targets of "branch-out" lift failures.

A function that jumps (jmp/jcc) into code outside its own catalogued blocks cannot be lifted until the
target is a catalogued entry (then the jump becomes DISPATCH_TAIL). Appends the targets to <seeds.json>
(note "branch-out", kept by prune_catalog --keep-seeds)."""
import json, re, sys
import capstone, pefile
exe, rep, seedp = sys.argv[1:4]
pe = pefile.PE(exe); b = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
seeds = json.load(open(seedp)); have = {e["address"] for e in seeds}
new = []
for f in json.load(open(rep))["failures"]:
    for va in re.findall(r"branch-out@([0-9a-f]+)", f["error"]):
        a = int(va, 16)
        i = next(md.disasm(img[a - b:a - b + 16], a), None)
        if not i or not i.op_str.startswith("0x"): continue
        t = int(i.op_str, 16)
        if t not in have: have.add(t); new.append({"address": t, "address_hex": f"0x{t:08X}", "note": "branch-out"})
seeds += new
json.dump(seeds, open(seedp, "w"), indent=1)
print(f"[*] {len(new)} branch-out targets added -> {seedp}")
