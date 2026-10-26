#!/usr/bin/env bash
# Cross-compiles dsp.so for the Move (aarch64 Linux) and assembles the module:
#   build/modules/sound_generators/tinyscratch/{module.json,dsp.so,samples/}
#   build/tinyscratch-<version>.tar.gz
# Toolchain: CROSS_PREFIX=aarch64-linux-gnu- | CROSS_CC="..." | zig (auto).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/toolchain.sh

if [ "${#CROSSCC[@]}" -eq 0 ]; then
    echo "error: no aarch64 cross compiler (set CROSS_PREFIX, CROSS_CC, or install zig)" >&2
    exit 2
fi

ID=tinyscratch
OUT=build/modules/sound_generators/$ID
rm -rf build/modules
mkdir -p "$OUT/samples"

bash scripts/gen_manifest.sh "$OUT/module.json" >/dev/null
VERSION=$(grep -o '"version":"[^"]*"' "$OUT/module.json" | cut -d'"' -f4)
[ -n "$VERSION" ] || { echo "error: no version in module.json" >&2; exit 1; }

echo "== cross compiler: ${CROSSCC[*]}"
"${CROSSCC[@]}" -std=gnu11 -D_GNU_SOURCE -O3 -shared -fPIC -fvisibility=hidden -s -pthread \
    -Wall -Wextra -Wshadow -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Isrc src/schwung_plugin.c src/scratch_engine.c -lm \
    -o "$OUT/dsp.so"

cp samples/*.wav "$OUT/samples/"
cp src/scratcher.js src/waveform.js "$OUT/"          # the screens (canvas scripts)
cp clips/recipes.txt "$OUT/recipes.txt"              # the clips every new instance starts with (more are kept on the Move)

# sanity: ELF, 64-bit, little-endian, machine 0xB7 (aarch64), exports the entry point
magic=$(od -An -tx1 -N4 "$OUT/dsp.so" | tr -d ' \n')
machine=$(od -An -tx1 -j18 -N2 "$OUT/dsp.so" | tr -d ' \n')
[ "$magic" = "7f454c46" ] || { echo "error: dsp.so is not an ELF" >&2; exit 1; }
[ "$machine" = "b700" ] || { echo "error: dsp.so is not aarch64 (e_machine=$machine)" >&2; exit 1; }
grep -aq move_plugin_init_v2 "$OUT/dsp.so" || { echo "error: move_plugin_init_v2 not exported" >&2; exit 1; }

TAR=build/$ID-$VERSION.tar.gz
tar -C build/modules/sound_generators -czf "$TAR" "$ID"

echo
echo "== binary sizes (bytes)"
printf '%10d  %s\n' "$(wc -c < "$OUT/dsp.so")" "$OUT/dsp.so (aarch64, stripped)"
printf '%10d  %s\n' "$(wc -c < "$OUT/module.json")" "$OUT/module.json (limit 8192)"
for f in "$OUT"/samples/*.wav; do printf '%10d  %s\n' "$(wc -c < "$f")" "$f"; done
printf '%10d  %s\n' "$(wc -c < "$TAR")" "$TAR"
