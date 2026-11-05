#!/usr/bin/env python3
"""seed_jumptables.py <exe> <catalog_blocks.json> <out_seeds.json> [--merge seeds.json ...]

Every `jmp dword ptr [reg*4 + table]` in catalogued blocks: read table entries while they point into
.text within 64 KB of the jump (max 256) and seed each one that is not already a function entry.
nfsu1-recompiled's code_seeds.py only accepts arms placed before the table; the CRT memcpy/memset
(hand-written asm, NFSHP2 0x5c6xxx) keeps its tables inside the body with the arms after them.
"""
import json, struct, sys
import capstone, pefile
from capstone.x86 import X86_OP_MEM

exe, catp, out = sys.argv[1:4]
merge = sys.argv[sys.argv.index("--merge") + 1:] if "--merge" in sys.argv else []
pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
cat = json.load(open(catp)); cs, ce = cat["code_start"], cat["code_end"]
entries = {f["address"] for f in cat["functions"]}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
found, tables, seen_t = {}, 0, set()

def scan(ins_iter):
    """collect table arms from jmp [reg*4+disp] instructions; return newly found arms"""
    global tables
    new = []
    for i in ins_iter:
        if i.mnemonic != "jmp" or not i.operands: continue
        op = i.operands[0]
        if op.type != X86_OP_MEM or op.mem.scale != 4 or not op.mem.disp or op.mem.base: continue
        t = op.mem.disp & 0xFFFFFFFF
        if t in seen_t or not (base <= t < base + len(img)): continue
        seen_t.add(t); tables += 1
        for k in range(1, 64):                   # negative indices: MSVC memcpy uses [ecx*4 + table] with ecx < 0
            off = t - base - 4 * k
            if off < 0: break
            v = struct.unpack_from("<I", img, off)[0]
            if not (cs <= v < ce and abs(v - i.address) < 0x10000): break
            if v not in entries and v not in found:
                found[v] = f"jumptable@0x{t:08x}-"; new.append(v)
        for k in range(256):
            off = t - base + 4 * k
            if off + 4 > len(img): break
            v = struct.unpack_from("<I", img, off)[0]
            if not (cs <= v < ce and abs(v - i.address) < 0x10000):
                if k == 0: continue      # CRT memcpy: index 0 is never used and overlaps the jmp itself
                break
            if v not in entries and v not in found:
                found[v] = f"jumptable@0x{t:08x}"; new.append(v)
    return new

def linear(a, n=96):
    """straight-line decode from an arm until ret/unconditional jmp (arms of asm routines)"""
    for i in md.disasm(img[a - base:a - base + 16 * n], a):
        yield i
        if i.mnemonic in ("ret", "jmp", "int3", "hlt"): return

work = []
for f in cat["functions"]:
    for bl in f["blocks"]:
        work += scan(md.disasm(img[bl[0] - base:bl[1] - base], bl[0]))
while work:                                  # closure: arms that dispatch through further tables
    work = [v for a in work for v in scan(linear(a))]
seeds = [{"address": v, "address_hex": f"0x{v:08X}", "note": n} for v, n in sorted(found.items())]
have = set(found)
for p in merge:
    for e in json.load(open(p)):
        if e["address"] not in have: have.add(e["address"]); seeds.append(e)
json.dump(seeds, open(out, "w"), indent=1)
print(f"[*] {tables} jump tables, {len(found)} arms not yet entries; total seeds {len(seeds)} -> {out}")
