#!/usr/bin/env python3
"""classify_gaps.py <exe> <catalog.json> : what is still uncovered in .text"""
import json, struct, sys, collections, pefile
exe, catp = sys.argv[1:3]
pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase
cat = json.load(open(catp)); cs, ce = cat["code_start"], cat["code_end"]
ivs = sorted((f["address"], f["end"]) for f in cat["functions"] if f["end"] > f["address"])
gaps, cur = [], cs
for s, e in ivs:
    if s > cur: gaps.append((cur, s))
    cur = max(cur, e)
if ce > cur: gaps.append((cur, ce))
kind = collections.Counter(); n = collections.Counter(); samples = collections.defaultdict(list)
for s, e in gaps:
    d = pe.get_data(s - base, e - s)
    if set(d) <= {0xCC, 0x90, 0}: k = "padding"
    else:
        # strip leading/trailing padding
        a = 0
        while a < len(d) and d[a] in (0xCC, 0x90): a += 1
        a4 = (s + a + 3) & ~3
        body = pe.get_data(a4 - base, max(0, e - a4))
        words = [struct.unpack_from("<I", body, i)[0] for i in range(0, len(body) - 3, 4)]
        ptr = sum(cs <= w < ce for w in words)
        idx = len(d) - a > 0 and all(b < 64 for b in d[a:]) and len(d) - a >= 4
        if words and ptr >= max(1, 0.75 * len(words)): k = "jump table (ptrs into .text)"
        elif idx: k = "switch index bytes"
        else: k = "code/other"
    kind[k] += e - s; n[k] += 1
    if len(samples[k]) < 6: samples[k].append((hex(s), e - s))
tot = sum(kind.values())
for k in kind: print(f"{k:30} {n[k]:5} gaps {kind[k]:8,} B  {100*kind[k]/tot:5.1f}%  e.g. {samples[k]}")
