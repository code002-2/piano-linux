#!/usr/bin/env bash
# build-bootimg.sh — pack an Android boot image for piano and verify it.
#
# Usage:
#   scripts/build-bootimg.sh --kernel FILE --ramdisk FILE --dtb FILE \
#       --header-version N --pagesize N --ramdisk-compression ALGO \
#       --output FILE [--cmdline STR] [--params-file FILE] \
#       [--allow-unverified]
#
# - --ramdisk names an UNCOMPRESSED cpio; this script compresses it with
#   the requested algorithm (none|gzip|lz4) before packing, because AOSP
#   mkbootimg expects a pre-compressed ramdisk.
# - The image is packed with the vendored AOSP mkbootimg.py and read back
#   with unpack_bootimg.py; header version, payload sizes and payload
#   bytes are all verified.
#
# Deliverable gate: every parameter placed into the image must be
# CONFIRMED in the params file (default boot/stock-boot-params.env,
# evidence: offline measurement of the stock fastboot ROM). Any UNVERIFIED
# parameter blocks a deliverable image. --allow-unverified permits
# synthetic smoke artifacts only, and forces a 'synthetic-' output prefix.

set -euo pipefail

usage() {
    sed -n '2,14p' "$0"; exit 2
}

die() {
    echo "build-bootimg: error: $*" >&2
    exit 1
}

KERNEL=""
RAMDISK=""
DTB=""
HEADER_VERSION=""
PAGESIZE=""
RAMDISK_COMPRESSION=""
OUTPUT=""
CMDLINE=""
PARAMS_FILE=""
ALLOW_UNVERIFIED=0

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel)              KERNEL="${2:?}"; shift 2 ;;
        --ramdisk)             RAMDISK="${2:?}"; shift 2 ;;
        --dtb)                 DTB="${2:?}"; shift 2 ;;
        --header-version)      HEADER_VERSION="${2:?}"; shift 2 ;;
        --pagesize)            PAGESIZE="${2:?}"; shift 2 ;;
        --ramdisk-compression) RAMDISK_COMPRESSION="${2:?}"; shift 2 ;;
        --output)              OUTPUT="${2:?}"; shift 2 ;;
        --cmdline)             CMDLINE="${2:?}"; shift 2 ;;
        --params-file)         PARAMS_FILE="${2:?}"; shift 2 ;;
        --allow-unverified)    ALLOW_UNVERIFIED=1; shift ;;
        -h|--help)             usage ;;
        *)                     usage ;;
    esac
done

for v in KERNEL RAMDISK DTB HEADER_VERSION PAGESIZE RAMDISK_COMPRESSION OUTPUT; do
    [ -n "${!v}" ] || { echo "build-bootimg: missing required argument for $v" >&2; usage; }
done

case "$RAMDISK_COMPRESSION" in
    none|gzip|lz4) ;;
    *) die "unsupported --ramdisk-compression '$RAMDISK_COMPRESSION' (none|gzip|lz4)" ;;
esac

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARAMS_FILE="${PARAMS_FILE:-$REPO_ROOT/boot/stock-boot-params.env}"
MKBOOTIMG="$REPO_ROOT/mkbootimg/mkbootimg.py"
UNPACK="$REPO_ROOT/mkbootimg/unpack_bootimg.py"

missing=()
command -v python3 >/dev/null 2>&1 || missing+=(python3)
[ "$RAMDISK_COMPRESSION" = gzip ] && ! command -v gzip >/dev/null 2>&1 && missing+=(gzip)
[ "$RAMDISK_COMPRESSION" = lz4 ]  && ! command -v lz4  >/dev/null 2>&1 && missing+=(lz4)
[ -f "$MKBOOTIMG" ]    || missing+=("$MKBOOTIMG")
[ -f "$UNPACK" ]       || missing+=("$UNPACK")
[ -f "$PARAMS_FILE" ]  || missing+=("$PARAMS_FILE (params file)")
for f in "$KERNEL" "$RAMDISK" "$DTB"; do
    [ -s "$f" ] || missing+=("$f (input missing or empty)")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "build-bootimg: missing prerequisites:" >&2
    printf '  - %s\n' "${missing[@]}" >&2
    exit 1
fi

# --- provenance gate ------------------------------------------------------
unverified=()

check_provenance() {
    # check_provenance <param> <value-passed>
    #
    # Entries: CONFIRMED:<p>=<value> must match the passed value;
    # UNVERIFIED:<p>=... blocks deliverables; POLICY:<p>=operator-choice
    # accepts any operator value (deliberate choice, e.g. our own cmdline
    # where the stock image carried none).
    local p="$1" passed="$2" entry status value
    if grep -qE "^POLICY:$p=operator-choice\$" "$PARAMS_FILE"; then
        return
    fi
    entry=$(grep -hE "^(CONFIRMED|UNVERIFIED):$p=" "$PARAMS_FILE" | tail -n1 || true)
    if [ -z "$entry" ]; then
        unverified+=("$p (not recorded in $PARAMS_FILE)")
        return
    fi
    status=${entry%%:*}
    value=${entry#*:}; value=${value#"$p="}
    if [ "$status" != CONFIRMED ]; then
        unverified+=("$p (UNVERIFIED in $PARAMS_FILE)")
    elif [ -n "$value" ] && [ "$value" != "$passed" ]; then
        die "parameter $p passed as '$passed' but the CONFIRMED value is '$value'"
    fi
}

check_provenance header_version "$HEADER_VERSION"
check_provenance pagesize "$PAGESIZE"
check_provenance ramdisk_compression "$RAMDISK_COMPRESSION"
[ -n "$CMDLINE" ] && check_provenance cmdline "$CMDLINE"

if [ "${#unverified[@]}" -gt 0 ]; then
    echo "build-bootimg: UNVERIFIED boot parameters present:" >&2
    printf '  - %s\n' "${unverified[@]}" >&2
    if [ "$ALLOW_UNVERIFIED" -ne 1 ]; then
        die "refusing to build a deliverable image from UNVERIFIED parameters (stock-ROM measurement required; see boot/stock-boot-params.env)"
    fi
    case "$(basename "$OUTPUT")" in
        synthetic-*) : ;;
        *) OUTPUT="$(dirname "$OUTPUT")/synthetic-$(basename "$OUTPUT")" ;;
    esac
    echo "build-bootimg: --allow-unverified: output forced to $OUTPUT" >&2
fi

# --- prepare ramdisk ------------------------------------------------------
WORK=$(mktemp -d "${TMPDIR:-/tmp}/piano-bootimg.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

case "$RAMDISK_COMPRESSION" in
    none) cp "$RAMDISK" "$WORK/ramdisk" ;;
    gzip) gzip -9 -n -c "$RAMDISK" > "$WORK/ramdisk" ;;
    lz4)  lz4 -q -9 -c "$RAMDISK" > "$WORK/ramdisk" ;;
esac
[ -s "$WORK/ramdisk" ] || die "ramdisk compression produced empty output"

# --- pack -----------------------------------------------------------------
mkdir -p "$(dirname "$OUTPUT")"
ARGS=(--kernel "$KERNEL" --ramdisk "$WORK/ramdisk" --dtb "$DTB"
      --header_version "$HEADER_VERSION" --pagesize "$PAGESIZE"
      -o "$OUTPUT")
[ -n "$CMDLINE" ] && ARGS+=(--cmdline "$CMDLINE")
python3 "$MKBOOTIMG" "${ARGS[@]}"
[ -s "$OUTPUT" ] || die "mkbootimg produced empty output"

# --- read back and verify -------------------------------------------------
python3 "$UNPACK" --boot_img "$OUTPUT" --out "$WORK/unpack" > "$WORK/unpack.log" 2>&1 \
    || { cat "$WORK/unpack.log" >&2; die "unpack_bootimg failed to read back $OUTPUT"; }

hdr=$(awk -F': *' '/^boot image header version/{print $2}' "$WORK/unpack.log" | tr -d '[:space:]')
[ "$hdr" = "$HEADER_VERSION" ] \
    || { cat "$WORK/unpack.log" >&2; die "header version round-trip mismatch: packed $HEADER_VERSION, read back '${hdr:-none}'"; }

KSIZE=$(wc -c < "$KERNEL")
RSIZE=$(wc -c < "$WORK/ramdisk")
grep -q "^kernel_size: $KSIZE\$" "$WORK/unpack.log" \
    || { cat "$WORK/unpack.log" >&2; die "kernel size round-trip mismatch (expected $KSIZE)"; }
grep -q "^ramdisk size: $RSIZE\$" "$WORK/unpack.log" \
    || { cat "$WORK/unpack.log" >&2; die "ramdisk size round-trip mismatch (expected $RSIZE)"; }

cmp -s "$KERNEL" "$WORK/unpack/kernel" \
    || die "kernel payload bytes differ after round-trip"
case "$RAMDISK_COMPRESSION" in
    gzip) gunzip -c "$WORK/unpack/ramdisk" > "$WORK/ramdisk.roundtrip" ;;
    lz4)  lz4 -q -d -c "$WORK/unpack/ramdisk" > "$WORK/ramdisk.roundtrip" ;;
    none) cp "$WORK/unpack/ramdisk" "$WORK/ramdisk.roundtrip" ;;
esac
cmp -s "$RAMDISK" "$WORK/ramdisk.roundtrip" \
    || die "ramdisk payload bytes differ after decompression round-trip"

echo "build-bootimg: wrote $OUTPUT ($(wc -c < "$OUTPUT") bytes) — round-trip verified (header v$HEADER_VERSION, kernel $KSIZE B, ramdisk $RSIZE B compressed)"
sha256sum "$OUTPUT"
