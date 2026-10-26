#!/usr/bin/env bash
# Builds tools/audio_convert (FLAC / WAV / AIFF / MP3 -> mono 16-bit 44.1 kHz) with mackron's
# public-domain dr_libs, which are downloaded into build/third_party and never committed.
# Shared by scripts/fetch_audition_samples.sh and scripts/classic_samples.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/toolchain.sh

TP=build/third_party
mkdir -p "$TP"
for h in dr_wav.h dr_flac.h dr_mp3.h; do
    if [ ! -s "$TP/$h" ]; then
        echo "== fetching $h"
        curl -sSfL --max-time 120 -o "$TP/$h" "https://raw.githubusercontent.com/mackron/dr_libs/master/$h"
    fi
done

echo "== building audio_convert"
"${HOSTCC[@]}" -std=gnu11 -O2 -I"$TP" -w tools/audio_convert.c -lm -o "build/audio_convert$EXE"
