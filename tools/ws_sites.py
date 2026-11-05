#!/usr/bin/env python3
"""Locate the x86 instructions that hp2wsfix patches (operand writes / NOPs / call hooks) in NFSHP2.exe v242.
Prints instruction start, bytes and operand offset. Reads the local exe only; prints nothing of the game besides
disassembly of those few sites."""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE(sys.argv[1]); img = pe.get_memory_mapped_image(); B = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
def ins_at(a):
    return next(md.disasm(bytes(img[a-B:a-B+16]), a), None)
for tok in sys.argv[2:]:
    a = int(tok, 16)
    if a >= 0x65d000 or a < 0x401000:
        print(f"{a:08x}: DATA  {bytes(img[a-B:a-B+8]).hex()}"); continue
    hits = []
    for s in range(a-10, a+1):
        i = ins_at(s)
        if i and s <= a < s + i.size:
            # check the decode chain from s-32.. re-syncs to s (crude boundary sanity)
            hits.append(i)
    # prefer the longest-from-earliest that also is reached by a linear decode from a-40
    ok = None
    for back in range(40, 0, -1):
        p = a - back; chain = []
        for i in md.disasm(bytes(img[p-B:p-B+64]), p):
            chain.append(i)
            if i.address + i.size > a: break
        if chain and chain[-1].address <= a < chain[-1].address + chain[-1].size:
            cand = chain[-1]
            if ok is None or True: ok = cand
            break
    i = ok
    print(f"{a:08x}: in {i.address:08x} +{a-i.address}  {i.mnemonic} {i.op_str}   [{i.bytes.hex()}]   candidates={[hex(h.address) for h in hits]}")
