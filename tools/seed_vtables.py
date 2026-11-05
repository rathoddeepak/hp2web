#!/usr/bin/env python3
"""seed_vtables.py <exe> <catalog.json> <seeds.json> : seed every 16-byte-aligned code pointer stored in a
data section (vtables, callback tables). pcrecomp's data scan rejects targets that a junk (SafeDisc filler)
body covers; Intel C++ aligns every function to 16, so aligned pointers into code are kept (note "vtable")."""
import json, struct, sys
import capstone, pefile
exe, catp, seedp = sys.argv[1:4]
pe = pefile.PE(exe); b = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
entries = {f["address"] for f in json.load(open(catp))["functions"]}
seeds = json.load(open(seedp)); have = {e["address"] for e in seeds} | entries
BAD = {"hlt", "in", "out", "les", "lds", "into", "iretd", "arpl", "bound", "retf", "int1"}
new = []
for s in pe.sections:
    nm = s.Name.rstrip(b"\0")
    if nm in (b".text", b"PACODE") or not (s.Characteristics & 0x40000000): continue   # readable data only
    lo = s.VirtualAddress; hi = lo + min(s.Misc_VirtualSize, s.SizeOfRawData)
    for k in range(lo, hi - 3, 4):
        v = struct.unpack_from("<I", img, k)[0]
        if not (0x401000 <= v < 0x65d000) or v % 16 or v in have: continue
        ins = list(md.disasm(img[v - b:v - b + 24], v))[:3]
        if len(ins) < 2 or any(i.mnemonic in BAD for i in ins) or (ins[0].mnemonic == "call" and img[v - b] == 0xE8): continue
        have.add(v); new.append({"address": v, "address_hex": f"0x{v:08X}", "note": "vtable"})
json.dump(seeds + new, open(seedp, "w"), indent=1)
print(f"[*] {len(new)} aligned data-section code pointers seeded -> {seedp}")
