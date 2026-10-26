#!/usr/bin/env bash
# Regenerates module.json from the plugin's parameter table.
# With an argument, writes there instead (test.sh uses this to check for drift).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/toolchain.sh

out="${1:-module.json}"
mkdir -p build
"${HOSTCC[@]}" -std=gnu11 -D_GNU_SOURCE -O1 -Wall -Wextra -Isrc -pthread \
    tools/gen_manifest.c src/schwung_plugin.c src/scratch_engine.c -lm \
    -o "build/gen_manifest$EXE"
"./build/gen_manifest$EXE" > "$out"
echo "wrote $out ($(wc -c < "$out") bytes)"
