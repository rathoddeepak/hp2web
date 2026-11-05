#!/usr/bin/env bash
# Repeat: lift -> seed branch-out targets -> catalog -> prune, until no new targets (max 6 rounds).
set -uo pipefail
cd "$(dirname "$0")/.."
PY=".venv/bin/python -I"; W=work/analysis; EXE=work/data/NFSHP2.exe
for round in 1 2 3 4 5 6; do
  rm -rf gen/hp2-r
  $PY tools/hp2_lift.py --catalog $W/catalog_blocks.final.json --output-dir gen/hp2-r --link-stubs >/dev/null 2>&1
  out=$($PY tools/seed_branchouts.py $EXE gen/hp2-r/lift-report.json $W/seeds5.json)
  echo "round $round: $out"
  n=$(echo "$out" | grep -o '[0-9]* branch-out targets' | cut -d' ' -f1)
  [ "${n:-0}" = 0 ] && break
  PCRECOMP="$PWD/pcrecomp" $PY nfsu1-recompiled/scripts/run_lift.py catalog $EXE $W/seeds5.json $W/catalog_blocks.json | tail -1
  $PY tools/prune_catalog.py $EXE $W/catalog_blocks.json $W/catalog_blocks.final.json --report $W/catalog_blocks.final.report.json --keep-seeds $W/seeds5.json | tail -1
done
rm -rf gen/hp2-r
