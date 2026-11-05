#!/usr/bin/env bash
# Step 1: build the pruned function catalog for NFSHP2.exe v242.
#   pass0 disasm -> seed gaps (x2) -> disasm with seeds -> prune SafeDisc filler by reachability
# Output: work/analysis/catalog_blocks.final.json (+ .report.json)
set -euo pipefail
cd "$(dirname "$0")/.."
EXE=work/data/NFSHP2.exe; W=work/analysis; PY=".venv/bin/python -I"
EXPECT=3a08f1051319dc83341a63e61e7184496d3b92f7843712b195456d86b7f262c8
[ "$(shasum -a 256 "$EXE" | cut -d' ' -f1)" = "$EXPECT" ] || { echo "NFSHP2.exe hash mismatch (need v242)"; exit 1; }
mkdir -p "$W"
tools/run_disasm.sh - "$W/catalog.json" | tail -3
$PY tools/seed_gaps.py "$EXE" "$W/catalog.json"  "$W/seeds1.json"
tools/run_disasm.sh "$W/seeds1.json" "$W/catalog1.json" | tail -2
$PY tools/seed_gaps.py "$EXE" "$W/catalog1.json" "$W/seeds2.json" --merge "$W/seeds1.json"
$PY tools/seed_initterm.py "$EXE" "$W/seeds3.json" --merge "$W/seeds2.json" profiles/hp2-seeds.json
# final pass keeps basic blocks (needed by the lifter), via nfsu1-recompiled's run_lift.py catalog
PCRECOMP="$PWD/pcrecomp" $PY nfsu1-recompiled/scripts/run_lift.py catalog "$EXE" "$W/seeds3.json" "$W/catalog_blocks.json" | tail -2
# second block pass: CRT/asm jump-table arms + code-address immediates (needs blocks)
PCRECOMP="$PWD/pcrecomp" $PY nfsu1-recompiled/scripts/code_seeds.py "$EXE" "$W/catalog_blocks.json" "$W/seeds_code.json" | tail -1
$PY tools/seed_jumptables.py "$EXE" "$W/catalog_blocks.json" "$W/seeds4.json" --merge "$W/seeds3.json" "$W/seeds_code.json"
PCRECOMP="$PWD/pcrecomp" $PY nfsu1-recompiled/scripts/run_lift.py catalog "$EXE" "$W/seeds4.json" "$W/catalog_blocks.json" | tail -2
# aligned code pointers in data sections (vtables whose targets a SafeDisc junk body covers)
$PY tools/seed_vtables.py "$EXE" "$W/catalog_blocks.json" "$W/seeds4.json"
# third pass: jump-table closure over the new arms
$PY tools/seed_jumptables.py "$EXE" "$W/catalog_blocks.json" "$W/seeds5.json" --merge "$W/seeds4.json"
PCRECOMP="$PWD/pcrecomp" $PY nfsu1-recompiled/scripts/run_lift.py catalog "$EXE" "$W/seeds5.json" "$W/catalog_blocks.json" | tail -2
$PY tools/prune_catalog.py "$EXE" "$W/catalog_blocks.json" "$W/catalog_blocks.final.json" --report "$W/catalog_blocks.final.report.json" --keep-seeds "$W/seeds5.json"
