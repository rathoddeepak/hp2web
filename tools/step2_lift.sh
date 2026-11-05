#!/usr/bin/env bash
# Step 2: lift the pruned catalog to C (gen/hp2) and compile every shard to wasm objects.
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf gen/hp2
.venv/bin/python -I tools/hp2_lift.py --catalog work/analysis/catalog_blocks.final.json --output-dir gen/hp2 --link-stubs | tail -10
tools/compile_shards.sh gen/hp2 | tail -1
