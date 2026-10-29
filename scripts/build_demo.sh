#!/usr/bin/env bash
# Builds a stand-alone demo of the Remote UI: build/demo/tinyscratch_remote_demo.html
#   tools/remote_demo.c plays a scripted set through the real engine and records what the browser would be sent (and the sound);
#   this puts the recording into a copy of src/web_ui.html, which plays it back through the same code the Move feeds.
# Needs a host C compiler, python3, and ffmpeg for a small MP3 (without it the sound goes in as WAV, bigger).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/toolchain.sh

OUT=build/demo
MOD=$OUT/module
rm -rf "$OUT"
mkdir -p "$MOD/samples"
cp samples/*.wav "$MOD/samples/"
cp clips/recipes.txt clips/kept.bin "$MOD/"

"${HOSTCC[@]}" -std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Isrc -pthread \
    tools/remote_demo.c src/schwung_plugin.c src/scratch_engine.c -lm -o "$OUT/remote_demo$EXE"
"./$OUT/remote_demo$EXE" "$MOD" "$OUT/rec"

AUDIO="$OUT/rec.wav" MIME=audio/wav
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -loglevel error -y -i "$OUT/rec.wav" -codec:a libmp3lame -b:a 128k "$OUT/rec.mp3"
    AUDIO="$OUT/rec.mp3" MIME=audio/mpeg
fi

python3 - "$OUT/rec.json" "$AUDIO" "$MIME" src/web_ui.html "$OUT/tinyscratch_remote_demo.html" <<'PY'
import base64, json, sys
rec, audio, mime, page, out = sys.argv[1:]
demo = json.load(open(rec))
demo["audio"] = "data:%s;base64,%s" % (mime, base64.b64encode(open(audio, "rb").read()).decode())
html = open(page).read()
bridge = '<script src="/static/schwung-remote-api.js"></script>'
assert bridge in html
# no host here: the bridge goes, the recording comes in its place
html = html.replace(bridge, "<script>window.TS_DEMO = %s;</script>" % json.dumps(demo, separators=(",", ":")))
open(out, "w").write(html)
print("wrote %s (%d bytes)" % (out, len(html)))
PY
