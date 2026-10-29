#!/usr/bin/env bash
# Builds and runs tests/test_engine.c under ASan+UBSan where the toolchain has
# them, plus an optimised pass. Every sanitizer is proven live by a self-check
# (a program with a known bug must be caught) before it is trusted; the
# summary says exactly which ones ran.
#
#   HOST_CC="clang"      choose the compiler
#   REQUIRE_ASAN=1       fail instead of falling back when ASan is unavailable
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/toolchain.sh

T=build/test
mkdir -p "$T" "$T/dedupe" "$T/rec"


cat > "$T/probe_ub.c" <<'EOF'
#include <limits.h>
int main(int c, char **v) { (void)v; volatile int x = INT_MAX; x += c; return x == 0; }
EOF
cat > "$T/probe_asan.c" <<'EOF'
#include <stdlib.h>
int main(int c, char **v) { (void)v; volatile int *p = malloc(4 * sizeof(int)); p[c + 3] = 1; int r = p[c + 3]; free((void *)p); return r == 0; }
EOF
cat > "$T/probe_fco.c" <<'EOF'
int main(int c, char **v) { (void)v; volatile float f = 1e30f * (float)c; volatile int i = (int)f; return i == 0; }
EOF

# compiles probe with flags, runs it; success means the sanitizer CAUGHT the bug
detects() {
    local flags="$1" src="$2" exe="$T/probe$EXE"
    rm -f "$exe"
    "${HOSTCC[@]}" -std=gnu11 -O1 -g $flags "$src" -o "$exe" >/dev/null 2>&1 || return 1
    if "$exe" >/dev/null 2>&1; then return 1; fi
    return 0
}

SAN=()
ASAN_ON=0 UBSAN_ON=0 FCO_ON=0
if detects "-fsanitize=address -fno-omit-frame-pointer" "$T/probe_asan.c"; then
    SAN+=(-fsanitize=address -fno-omit-frame-pointer); ASAN_ON=1
fi
if detects "-fsanitize=undefined -fno-sanitize-recover=undefined" "$T/probe_ub.c"; then
    SAN+=(-fsanitize=undefined -fno-sanitize-recover=undefined); UBSAN_ON=1
fi
if detects "-fsanitize=float-cast-overflow -fno-sanitize-recover=float-cast-overflow" "$T/probe_fco.c"; then
    SAN+=(-fsanitize=float-cast-overflow -fno-sanitize-recover=float-cast-overflow); FCO_ON=1
fi

if [ "${REQUIRE_ASAN:-0}" = 1 ] && [ "$ASAN_ON" = 0 ]; then
    echo "error: REQUIRE_ASAN=1 but AddressSanitizer is not usable with: ${HOSTCC[*]}" >&2
    exit 3
fi

CFLAGS=(-std=gnu11 -D_GNU_SOURCE -g -Wall -Wextra -Wshadow -Isrc -pthread)
SRC=(tests/test_engine.c)

echo "== host compiler: ${HOSTCC[*]}"
echo "== sanitizers:  ASan=$([ $ASAN_ON = 1 ] && echo ON || echo UNAVAILABLE)  UBSan=$([ $UBSAN_ON = 1 ] && echo ON || echo UNAVAILABLE)  float-cast-overflow=$([ $FCO_ON = 1 ] && echo ON || echo UNAVAILABLE)"
if [ "$ASAN_ON" = 0 ]; then
    echo "   (no ASan here: tests/test_engine.c still checks leaks and redzone overruns on every allocation)"
fi

if [ "${#SAN[@]}" -gt 0 ]; then
    echo "== sanitized build (-O1)"
    "${HOSTCC[@]}" "${CFLAGS[@]}" -O1 "${SAN[@]}" "${SRC[@]}" -lm -o "$T/test_san$EXE"
    "./$T/test_san$EXE" .
fi

echo "== optimised build (-O2)"
"${HOSTCC[@]}" "${CFLAGS[@]}" -O2 "${SRC[@]}" -lm -o "$T/test_opt$EXE"
"./$T/test_opt$EXE" .

echo "== the screen (src/scratcher.js, headless, fed with the engine's own views)"
if command -v node >/dev/null 2>&1; then
    node tests/screen_test.mjs
else
    echo "   SKIPPED: node not found"
fi

echo "== the Remote UI (src/web_ui.html in a browser, against a recording of the engine)"
if command -v node >/dev/null 2>&1 && NODE_PATH="${NODE_PATH:-$(npm root -g 2>/dev/null || true)}" node -e 'require("playwright")' >/dev/null 2>&1; then
    bash scripts/build_demo.sh
    NODE_PATH="${NODE_PATH:-$(npm root -g)}" node tests/remote_ui_test.mjs
else
    echo "   SKIPPED: node or playwright not found"
fi

echo "== manifest drift check"
bash scripts/gen_manifest.sh "$T/module.generated.json" >/dev/null
if ! diff -u module.json "$T/module.generated.json"; then
    echo "module.json is stale: run scripts/gen_manifest.sh" >&2
    exit 1
fi
echo "module.json matches the parameter table ($(wc -c < module.json) bytes, limit 8192)"

echo
echo "ALL TESTS PASSED  [ASan=$ASAN_ON UBSan=$UBSAN_ON float-cast-overflow=$FCO_ON]"
