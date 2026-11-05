#!/usr/bin/env python3
"""ret_pops.py <hex addrs...> : print 'addr:bytes' = stack bytes each function pops on return (4 + ret imm), -1 if mixed/unknown."""
import json, sys, pefile, capstone
pe = pefile.PE(sys.argv[1]); b = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
byA = {f["address"]: f for f in json.load(open(sys.argv[2]))["functions"]}
out = []
for h in sys.argv[3:]:
    a = int(h, 16); f = byA.get(a); vals = set()
    for bl in (f["blocks"] if f else []):
        for i in md.disasm(img[bl[0] - b:bl[1] - b], bl[0]):
            if i.mnemonic == "ret": vals.add(4 + (int(i.op_str, 16) if i.op_str else 0))
    out.append(f"{h}:{vals.pop() if len(vals) == 1 else -1}")
print(" ".join(out))
