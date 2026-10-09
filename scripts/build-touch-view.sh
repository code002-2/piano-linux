#!/usr/bin/env bash
# build-touch-view.sh — cross-compile the static piano-touch-view helper.
#
# Usage: scripts/build-touch-view.sh --uapi DIR --output FILE [--sysroot DIR]
#                                    [--source FILE]
#
#   --uapi DIR     kernel UAPI headers (make headers_install
#                  INSTALL_HDR_PATH=...; DIR/include/linux/fb.h must exist)
#   --sysroot DIR  aarch64 musl sysroot staged by fetch-arm64-tools.sh
#                  (default: out/arm64-tools/musl-sysroot)
#   --output FILE  static arm64 ELF to write
#   --source FILE  C source to build (default: the touch viewer); also used
#                  for the other single-file helpers (piano-bt-scan,
#                  piano-camerad)
#
# Needs clang and ld.lld on the host; links musl libc.a plus the staged
# compiler-rt builtins, no host aarch64 runtime required.

set -euo pipefail

die() {
    echo "build-touch-view: $*" >&2
    exit 1
}

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
SYSROOT="$REPO_ROOT/out/arm64-tools/musl-sysroot"
UAPI=""
OUTPUT=""
SRC="$REPO_ROOT/initramfs/touch-view/piano-touch-view.c"

while [ $# -gt 0 ]; do
    case "$1" in
        --uapi)    UAPI=${2-}; shift 2 ;;
        --sysroot) SYSROOT=${2-}; shift 2 ;;
        --output)  OUTPUT=${2-}; shift 2 ;;
        --source)  SRC=${2-}; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

if [ -z "$UAPI" ] || [ -z "$OUTPUT" ]; then
    die "--uapi and --output are required"
fi
[ -f "$UAPI/include/linux/fb.h" ] || die "no UAPI headers at $UAPI/include"
for f in usr/lib/crt1.o usr/lib/crti.o usr/lib/crtn.o usr/lib/libc.a \
         usr/lib/libclang_rt.builtins-aarch64.a usr/include/stdio.h; do
    [ -f "$SYSROOT/$f" ] || die "sysroot lacks $f (run scripts/fetch-arm64-tools.sh)"
done
command -v clang >/dev/null 2>&1 || die "clang not found"
command -v ld.lld >/dev/null 2>&1 || die "ld.lld not found"

[ -f "$SRC" ] || die "no such source: $SRC"
OBJ=$(mktemp "${TMPDIR:-/tmp}/piano-touch-view.XXXXXX.o")
trap 'rm -f "$OBJ"' EXIT

clang --target=aarch64-linux-musl -nostdinc \
    -isystem "$UAPI/include" -isystem "$SYSROOT/usr/include" \
    -isystem "$(clang -print-resource-dir)/include" \
    -O2 -Wall -Wextra -Werror -c -o "$OBJ" "$SRC"
ld.lld -static -s -o "$OUTPUT" \
    "$SYSROOT/usr/lib/crt1.o" "$SYSROOT/usr/lib/crti.o" "$OBJ" \
    "$SYSROOT/usr/lib/libc.a" "$SYSROOT/usr/lib/libclang_rt.builtins-aarch64.a" \
    "$SYSROOT/usr/lib/crtn.o"

file "$OUTPUT" | grep -q 'ARM aarch64.*statically linked' \
    || die "$OUTPUT is not a static arm64 ELF"
echo "build-touch-view: wrote $OUTPUT"
