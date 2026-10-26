#!/usr/bin/env bash
# Sourced by build.sh / test.sh / gen_manifest.sh. Picks compilers; override with
#   HOST_CC="clang"  CROSS_CC="aarch64-linux-gnu-gcc"
# Sets: HOSTCC (array), CROSSCC (array, may be empty), EXE (".exe" on Windows).

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

HOSTCC=()
if [ -n "${HOST_CC:-}" ]; then
    read -r -a HOSTCC <<< "$HOST_CC"
elif command -v gcc >/dev/null 2>&1; then HOSTCC=(gcc)
elif command -v clang >/dev/null 2>&1; then HOSTCC=(clang)
elif command -v zig >/dev/null 2>&1; then HOSTCC=(zig cc)
fi

CROSSCC=()
if [ -n "${CROSS_CC:-}" ]; then
    read -r -a CROSSCC <<< "$CROSS_CC"
elif [ -n "${CROSS_PREFIX:-}" ]; then CROSSCC=("${CROSS_PREFIX}gcc")
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then CROSSCC=(aarch64-linux-gnu-gcc)
elif command -v zig >/dev/null 2>&1; then CROSSCC=(zig cc -target aarch64-linux-gnu.2.28)
fi

if [ "${#HOSTCC[@]}" -eq 0 ]; then
    echo "error: no host C compiler found (install gcc/clang/zig or set HOST_CC)" >&2
    exit 2
fi
