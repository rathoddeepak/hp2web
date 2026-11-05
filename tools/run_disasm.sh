#!/usr/bin/env bash
# run_disasm.sh <seeds.json|-> <out_catalog.json>
cd "$(dirname "$0")/.."
S=(); [ "$1" != "-" ] && S=(--seed-functions "$1")
.venv/bin/python -I pcrecomp/tools/disasm/disasm32.py work/data/NFSHP2.exe "${S[@]}" -o "$2"
