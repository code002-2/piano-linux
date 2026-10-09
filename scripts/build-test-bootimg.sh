#!/usr/bin/env bash
# build-test-bootimg.sh — build the minimal piano RAM-boot test image set.
#
# Usage:
#   scripts/build-test-bootimg.sh --kernel-dir DIR --output-dir DIR \
#       [--dtbo-source PATH]
#
# Produces exactly three files in --output-dir:
#   boot.img      v4 boot image wrapping the kernel Image (which embeds the
#                 debug initramfs via CONFIG_INITRAMFS_SOURCE). The external
#                 ramdisk is empty — the only image form ABL accepts.
#   dtbo.img      test overlay, deterministic build from
#                 boot/dtbo-piano-touch-v2.dts by default (milestone-1
#                 USB/display overlay + touch path); flash to dtbo_b
#                 before boot.
#   MANIFEST.txt  provenance, hashes, verified boot recipe.
#
# PROVEN BOOT CONTRACT (on-device 2026-09-22/23, umbrella runbook §7):
#   fastboot set_active b                       # RAM boot composes slot-b
#   fastboot flash dtbo_b dtbo.img
#   fastboot boot boot.img                       # RAM boot, nothing else
#   The CURRENT SLOT's stock vendor_boot/init_boot stay untouched; ABL
#   concatenates their ramdisks after ours and the kernel's forced cmdline
#   (rdinit=/beaconinit) selects our init. Custom vendor_boot / v2 / v0
#   images are silently REJECTED by ABL — those variants were deleted from
#   this builder (documented dead ends, runbook §7.1).
#
# The kernel Image is expected to already embed the initramfs; the umbrella
# scripts/build-test-image.sh orchestrates that end to end.

set -euo pipefail

usage() {
    sed -n '2,28p' "$0"; exit 2
}

die() {
    echo "build-test-bootimg: error: $*" >&2
    exit 1
}

KERNEL_DIR=""
OUTPUT_DIR=""
DTBO_DTS=""
MODE='test'

while [ $# -gt 0 ]; do
    case "$1" in
        --mode) MODE=${2-}; shift 2 ;;
        --kernel-dir) KERNEL_DIR=${2-}; shift 2 ;;
        --output-dir) OUTPUT_DIR=${2-}; shift 2 ;;
        --dtbo-source) DTBO_DTS=${2-}; shift 2 ;;
        -h|--help)    usage ;;
        *)            die "unknown option: $1" ;;
    esac
done
case "$MODE" in test|rootfs) ;; *) die '--mode must be test or rootfs' ;; esac

for v in KERNEL_DIR OUTPUT_DIR; do
    [ -n "${!v}" ] || { echo "build-test-bootimg: missing required argument for $v" >&2; usage; }
done

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARAMS_FILE="$REPO_ROOT/boot/stock-boot-params.env"
MKBOOTIMG="$REPO_ROOT/mkbootimg/mkbootimg.py"
UNPACK="$REPO_ROOT/mkbootimg/unpack_bootimg.py"
BUILD_DTBO="$REPO_ROOT/scripts/build-dtbo.py"
DTBO_DTS=${DTBO_DTS:-"$REPO_ROOT/boot/dtbo-piano-touch-v2.dts"}

IMAGE=$KERNEL_DIR/arch/arm64/boot/Image
UTSRELEASE_H=$KERNEL_DIR/include/generated/utsrelease.h

missing=()
command -v python3   >/dev/null 2>&1 || missing+=(python3)
command -v dtc       >/dev/null 2>&1 || missing+=(dtc)
command -v sha256sum >/dev/null 2>&1 || missing+=(sha256sum)
for f in "$MKBOOTIMG" "$UNPACK" "$PARAMS_FILE" "$BUILD_DTBO" "$DTBO_DTS" \
         "$IMAGE" "$UTSRELEASE_H"; do
    [ -s "$f" ] || missing+=("$f (missing or empty)")
done
if [ "${#missing[@]}" -gt 0 ]; then
    printf 'build-test-bootimg: missing prerequisites:\n' >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

KVER=$(sed -n 's/^#define UTS_RELEASE "\(.*\)"$/\1/p' "$UTSRELEASE_H")
[ -n "$KVER" ] || die "cannot parse kernel release from $UTSRELEASE_H"

# --- provenance-gated parameters --------------------------------------------
param_value() { # param_value NAME — echoes CONFIRMED value or dies
    local val
    val=$(sed -n "s/^CONFIRMED:$1=//p" "$PARAMS_FILE" | head -n 1)
    [ -n "$val" ] || die "$1 is not CONFIRMED in $PARAMS_FILE"
    printf '%s\n' "$val"
}

HEADER_VERSION=$(param_value header_version)
PAGESIZE=$(param_value pagesize)
VB_BASE=$(param_value vb_base)
VB_KERNEL_OFFSET=$(param_value vb_kernel_offset)
VB_RAMDISK_OFFSET=$(param_value vb_ramdisk_offset)
VB_TAGS_OFFSET=$(param_value vb_tags_offset)

mkdir -p "$OUTPUT_DIR"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/piano-bootimg.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- boot.img: v4 + Image + empty external ramdisk ---------------------------
# The offsets mirror the stock vendor_boot geometry; with an empty ramdisk
# they are inert but keep the header identical in shape to the
# device-verified usb31 image form.
echo "build-test-bootimg: packing boot.img (header v$HEADER_VERSION, pagesize $PAGESIZE)"
: > "$WORK/empty"
python3 "$MKBOOTIMG" \
    --kernel "$IMAGE" --ramdisk "$WORK/empty" \
    --header_version "$HEADER_VERSION" --pagesize "$PAGESIZE" \
    --base "$VB_BASE" --kernel_offset "$VB_KERNEL_OFFSET" \
    --ramdisk_offset "$VB_RAMDISK_OFFSET" --tags_offset "$VB_TAGS_OFFSET" \
    -o "$OUTPUT_DIR/boot.img"

# --- dtbo.img: deterministic overlay ------------------------------------------
echo "build-test-bootimg: building dtbo.img from $DTBO_DTS"
python3 "$BUILD_DTBO" --dts "$DTBO_DTS" --output "$OUTPUT_DIR/dtbo.img"

# --- verification --------------------------------------------------------------
echo "build-test-bootimg: verifying round-trips"
python3 "$UNPACK" --boot_img "$OUTPUT_DIR/boot.img" --out "$WORK/verify" >/dev/null
cmp -s "$WORK/verify/kernel" "$IMAGE" \
    || die "boot.img does not round-trip to the kernel Image bytes"
[ -s "$OUTPUT_DIR/dtbo.img" ] || die "dtbo.img is empty"

BOOT_SHA=$(sha256sum "$OUTPUT_DIR/boot.img" | awk '{print $1}')
DTBO_SHA=$(sha256sum "$OUTPUT_DIR/dtbo.img" | awk '{print $1}')
BOOT_SIZE=$(stat -c%s "$OUTPUT_DIR/boot.img")
DTBO_SIZE=$(stat -c%s "$OUTPUT_DIR/dtbo.img")

# --- manifest -------------------------------------------------------------------
{
    echo "piano $MODE image set"
    echo "built:   $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "kernel: $KVER (initramfs embedded via CONFIG_INITRAMFS_SOURCE)"
    echo
    echo "files:"
    printf '  boot.img  %10d bytes  sha256 %s\n' "$BOOT_SIZE" "$BOOT_SHA"
    printf '  dtbo.img  %10d bytes  sha256 %s\n' "$DTBO_SIZE" "$DTBO_SHA"
    echo
    echo "verified: boot.img unpacks byte-identical to the built Image."
    echo
    if [ "$MODE" = test ]; then
        echo "boot recipe (RAM boot; the only flash is dtbo_b):"
        echo "  fastboot getvar current-slot   # must be b for slot-b compose"
        echo "  fastboot flash dtbo_b dtbo.img"
        echo "  fastboot boot boot.img"
        echo "  # host 10.42.0.1/24; telnet 10.42.0.2 23 (unauthenticated test mode)"
    else
        echo "userdata/GNOME boot: requires the matching userdata.img from build-rootfs-image.sh."
        echo "rdinit=/pianoinit; key-only USB SSH; never flash this as a standalone rootfs set."
    fi
    echo
    echo "safety: vendor_boot/init_boot stay STOCK — custom vendor_boot,"
    echo "  v2 and v0 images are silently rejected by ABL (runbook §7.1)."
    echo "  If 'Booting' fails instantly and 'oem lkmsg' shows no mainline"
    echo "  log, that is ABL partition-state poisoning (runbook §7.5), not"
    echo "  a code problem."
} | tee "$OUTPUT_DIR/MANIFEST.txt"

echo
echo "build-test-bootimg: complete — boot.img, dtbo.img, MANIFEST.txt in $OUTPUT_DIR"
