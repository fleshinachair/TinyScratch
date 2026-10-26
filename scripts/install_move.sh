#!/usr/bin/env bash
# Builds TinyScratch and installs it on a Move over SSH, together with the
# public-domain audition samples (samples_audition/, gitignored), then restarts
# Move so Schwung picks it up.
#
#   scripts/install_move.sh [--no-build] [--no-restart] [--no-audition]
#   MOVE_HOST=move.local MOVE_USER=ableton   (defaults)
#
# Needs key-based SSH to the Move (BatchMode: it never asks for a password).
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-move.local}"
RUSER="${MOVE_USER:-ableton}"
DEST=/data/UserData/schwung/modules/sound_generators/tinyscratch
OPTS=(-o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR)   # LogLevel: no known-hosts chatter, real errors still show
SSH=(ssh "${OPTS[@]}" "$RUSER@$HOST")
SCP=(scp "${OPTS[@]}")
BUILD=1 RESTART=1 AUDITION=1
for a in "$@"; do
    case "$a" in
        --no-build) BUILD=0 ;;
        --no-restart) RESTART=0 ;;
        --no-audition) AUDITION=0 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done

[ "$BUILD" = 1 ] && bash scripts/build.sh
SRC=build/modules/sound_generators/tinyscratch
[ -f "$SRC/dsp.so" ] || { echo "error: no build in $SRC (run without --no-build)" >&2; exit 1; }

if ! "${SSH[@]}" true; then
    echo "error: cannot reach $RUSER@$HOST over SSH. Is the Move awake and on this network?" >&2
    echo "       (if mDNS is flaky, use its address: MOVE_HOST=192.168.x.y $0)" >&2
    exit 1
fi
echo "== installing to $RUSER@$HOST:$DEST"
# the samples folder is ours: it is rewritten whole, so nothing from an older build is left to load as a User slot. The
# exception is what you recorded on the Move (user1.wav .. user4.wav), which is kept.
"${SSH[@]}" "mkdir -p $DEST/samples && for f in $DEST/samples/*.wav; do case \"\$f\" in */user[1-4].wav) ;; *) rm -f \"\$f\" ;; esac; done; rm -f $DEST/deck.js $DEST/clips.bin"
"${SCP[@]}" "$SRC/dsp.so" "$SRC/module.json" "$SRC/scratcher.js" "$SRC/waveform.js" "$RUSER@$HOST:$DEST/"
"${SCP[@]}" "$SRC"/samples/*.wav "$RUSER@$HOST:$DEST/samples/"
files=(dsp.so module.json scratcher.js waveform.js)
# clips imported from a Live Pack you own (tools/import_clips.py): your own copy, kept out of the repository (local/ is gitignored).
# They are read-only on the Move: they can be played, not deleted.
if [ -f local/clips.bin ]; then
    "${SCP[@]}" local/clips.bin "$RUSER@$HOST:$DEST/"
    files+=(clips.bin)
else
    echo "   (no local/clips.bin: only the built-in clips. python tools/import_clips.py <a Live Pack .alp> makes it)"
fi
# the default clips are recipes (clips/recipes.txt). The Move keeps the library you have made there, so it is only sent
# when the Move has none yet.
if ! "${SSH[@]}" "[ -f $DEST/recipes.txt ]"; then
    "${SCP[@]}" "$SRC/recipes.txt" "$RUSER@$HOST:$DEST/"
    files+=(recipes.txt)
fi
for f in "$SRC"/samples/*.wav; do files+=("samples/$(basename "$f")"); done
if [ "$AUDITION" = 1 ] && ls samples_audition/*.wav >/dev/null 2>&1; then
    "${SCP[@]}" samples_audition/*.wav "$RUSER@$HOST:$DEST/samples/"
    for f in samples_audition/*.wav; do
        n="samples/$(basename "$f")"
        case " ${files[*]} " in *" $n "*) ;; *) files+=("$n") ;; esac       # an audition sample of the same name replaces the shipped one: it is one file
    done
elif [ "$AUDITION" = 1 ]; then
    echo "   (no audition samples: run scripts/fetch_audition_samples.sh first)"
fi
"${SSH[@]}" "chmod 755 $DEST/dsp.so"

echo "== verifying ${#files[@]} files by checksum"
local_sums=$(for f in "${files[@]}"; do
    case "$f" in
        clips.bin) p="local/clips.bin" ;;
        samples/*) if [ "$AUDITION" = 1 ] && [ -f "samples_audition/${f#samples/}" ]; then p="samples_audition/${f#samples/}"; else p="$SRC/$f"; fi ;;   # the audition copy was sent last, so it is the one there
        *) p="$SRC/$f" ;;
    esac
    printf '%s %s\n' "$(sha256sum "$p" | cut -d' ' -f1)" "$f"
done | sort -k2)
remote_sums=$("${SSH[@]}" "cd $DEST && sha256sum ${files[*]}" 2>/dev/null | awk '{print $1, $2}' | sort -k2)
if [ "$local_sums" != "$remote_sums" ]; then
    echo "error: checksum mismatch on the Move" >&2
    diff <(echo "$local_sums") <(echo "$remote_sums") >&2 || true
    exit 1
fi
echo "   all match"

if [ "$RESTART" = 1 ]; then
    echo "== restarting Move (Schwung's own restart-move.sh)"
    "${SSH[@]}" "sh /data/UserData/schwung/restart-move.sh >/dev/null 2>&1 </dev/null" 2>/dev/null || true
    for i in $(seq 1 20); do
        sleep 2
        if "${SSH[@]}" "pidof MoveOriginal >/dev/null && grep -q '^tinyscratch=' /data/UserData/schwung/module-snapshot.txt" 2>/dev/null; then
            echo "   Move is back; Schwung lists $("${SSH[@]}" "grep '^tinyscratch=' /data/UserData/schwung/module-snapshot.txt" 2>/dev/null)"
            exit 0
        fi
    done
    echo "warning: Move did not report back within 40 s; check the device" >&2
    exit 1
fi
