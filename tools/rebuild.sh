#!/usr/bin/env bash
# Merge profiles/hp2-seeds.json into the seed list, re-catalog, prune, lift, compile (cached), link.
set -euo pipefail
cd "$(dirname "$0")/.."
PY=".venv/bin/python -I"; W=work/analysis; EXE=work/data/NFSHP2.exe
$PY - <<'P'
import json
a=json.load(open('work/analysis/seeds5.json')); have={e['address'] for e in a}
a+=[e for e in json.load(open('profiles/hp2-seeds.json')) if e['address'] not in have]
json.dump(a,open('work/analysis/seeds5.json','w')); print("seeds:",len(a))
P
PCRECOMP="$PWD/pcrecomp" $PY nfsu1-recompiled/scripts/run_lift.py catalog $EXE $W/seeds5.json $W/catalog_blocks.json | tail -1
$PY tools/prune_catalog.py $EXE $W/catalog_blocks.json $W/catalog_blocks.final.json --report $W/catalog_blocks.final.report.json --keep-seeds $W/seeds5.json | tail -1
tools/step2_lift.sh | grep -a '"emitted"\|objects'
tools/step3_build.sh | tail -1
