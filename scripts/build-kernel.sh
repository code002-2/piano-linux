#!/usr/bin/env bash
# build-kernel.sh — cross-repo kernel build entry point for the umbrella
# workspace.
#
# Usage:
#   scripts/build-kernel.sh [--jobs N] [--out DIR]
#
# - Locates linux-piano/ relative to the workspace root (this script's
#   parent's parent).
# - Defaults: JOBS=$(nproc), OUT=<workspace>/out/kernel.
# - Builds piano_defconfig, then Image, dtbs and modules with LLVM=1.
# - Verifies the piano-specific artifacts exist before exiting 0:
#     OUT/arch/arm64/boot/Image
#     OUT/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dtb
# - Fails non-zero when the submodule is missing, the build fails, or the
#   expected artifacts were not produced. Never performs any flash
#   operation.

set -euo pipefail

usage() {
    sed -n '2,15p' "$0"; exit 2
}

die() {
    echo "build-kernel: error: $*" >&2
    exit 1
}

JOBS=$(nproc)
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="${2:?}"; shift 2 ;;
        --out)  OUT="${2:?}";  shift 2 ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

case "$JOBS" in
    ''|*[!0-9]*) die "--jobs must be a positive integer (got '$JOBS')" ;;
esac
[ "$JOBS" -ge 1 ] || die "--jobs must be >= 1"

WORKSPACE=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="$WORKSPACE/kernel"

[ -d "$KERNEL/.git" ] || [ -f "$KERNEL/.git" ] \
    || die "linux-piano submodule not found at $KERNEL (run: git submodule update --init)"
[ -f "$KERNEL/Makefile" ] || die "$KERNEL does not look like a kernel tree"
[ -f "$KERNEL/arch/arm64/configs/piano_defconfig" ] \
    || die "piano_defconfig missing in linux-piano (is the checked-out branch up to date?)"

OUT="${OUT:-$WORKSPACE/out/kernel}"
mkdir -p "$OUT"
OUT=$(realpath "$OUT")

echo "build-kernel: tree=$KERNEL out=$OUT jobs=$JOBS"

make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$OUT" piano_defconfig
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$OUT" -j"$JOBS" Image dtbs modules

IMAGE="$OUT/arch/arm64/boot/Image"
DTB="$OUT/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dtb"

[ -s "$IMAGE" ] || die "expected artifact missing: $IMAGE"
[ -s "$DTB" ]   || die "expected artifact missing: $DTB"

echo "build-kernel: OK"
ls -la "$IMAGE" "$DTB"
