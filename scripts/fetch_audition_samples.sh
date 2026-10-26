#!/usr/bin/env bash
# Builds tools/audio_convert (with mackron's public-domain dr_wav/dr_flac,
# downloaded into build/third_party) and fetches the public-domain audition
# samples into samples_audition/ (gitignored). See tools/fetch_audition_samples.py.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/toolchain.sh

bash scripts/build_converter.sh

PY=""
for cand in "${PYTHON:-}" python3 python py; do
    [ -n "$cand" ] || continue
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -I -c 'import sys; sys.exit(sys.version_info < (3, 6))' >/dev/null 2>&1; then
        PY="$cand"; break
    fi
done
[ -n "$PY" ] || { echo "error: Python 3 not found (set PYTHON=...)" >&2; exit 2; }

"$PY" -I tools/fetch_audition_samples.py --converter "build/audio_convert$EXE" "$@"
