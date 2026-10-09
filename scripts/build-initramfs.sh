#!/usr/bin/env bash
# build-initramfs.sh — assemble the piano debug/test initramfs.
#
# Usage:
#   scripts/build-initramfs.sh --busybox DIR --dropbear DIR --output FILE
#       [--mode test|rootfs]      (rootfs: /pianoinit, key-only rescue)
#       [--authorized-keys FILE | --generate-access-key FILE]
#       [--diagnostic]           (rootfs: pause before each storage step)
#       [--root-password PASS]   (empty string = blank password login)
#       [--module FILE]... [--kernel-version VER]
#       [--firmware-dir DIR]      (copies DIR/novatek/*.bin)
#       [--compress none|gzip]    (default gzip)
#
# DIR arguments point at directories that contain the binaries:
#   --busybox  DIR with a static `busybox`
#   --dropbear DIR with `dropbear` and `dropbearkey` (dropbearmulti works
#              if `dropbearkey` is symlinked next to it)
#   --dropbear-tree DIR  alternative: copy a full dropbear userland tree
#              (binaries + shared-library closure, as staged by
#              scripts/fetch-arm64-tools.sh) verbatim into the initramfs
#
# Extra content (test image):
#   --authorized-keys FILE  installs FILE as /root/.ssh/authorized_keys
#   --generate-access-key F generates a fresh ed25519 key pair with the
#                           host ssh-keygen; the public half is installed
#                           into the initramfs and the PRIVATE key is
#                           written to F (mode 0600) for the operator.
#   --module FILE           installs a kernel module into
#                           /lib/modules/<--kernel-version>/
#   --firmware-dir DIR      copies DIR/novatek/*.bin to /lib/firmware/novatek/
#   --touch-view FILE       installs the static THP frame viewer
#                           (scripts/build-touch-view.sh) as
#                           /usr/bin/piano-touch-view
#   --bt-scan FILE          installs the static raw-HCI LE scanner as
#                           /usr/bin/piano-bt-scan
#
# Any executable under initramfs/tests/ is installed into /usr/bin
# (piano-tests, piano-touch-test, piano-display-test, piano-collect).
#
# Fails (non-zero) when tools are missing, the NCM gadget function cannot
# be represented in the generated image (init sanity), or the cpio output
# is empty.

set -euo pipefail

usage() {
    sed -n '2,25p' "$0"; exit 2
}

die() {
    echo "build-initramfs: $*" >&2
    exit 1
}

BUSYBOX_DIR=""
DROPBEAR_DIR=""
DROPBEAR_TREE=""
IW_TREE=""
PD_LOCATOR=""
TOUCH_VIEW=""
BT_SCAN=""
APLAY_TREE=""
AUTHORIZED_KEYS=""
GENERATE_KEY_OUT=""
ROOT_PASSWORD=""
ROOT_PASSWORD_SET=0
MODULES=()
KERNEL_VERSION=""
FIRMWARE_DIR=""
COMPRESS=gzip
MODE='test'
DIAGNOSTIC=0

while [ $# -gt 0 ]; do
    case "$1" in
        --mode)               MODE=${2-}; shift 2 ;;
        --diagnostic)          DIAGNOSTIC=1; shift ;;
        --busybox)             BUSYBOX_DIR=${2-}; shift 2 ;;
        --dropbear)            DROPBEAR_DIR=${2-}; shift 2 ;;
        --dropbear-tree)       DROPBEAR_TREE=${2-}; shift 2 ;;
        --iw-tree)             IW_TREE=${2-}; shift 2 ;;
        --pd-locator)          PD_LOCATOR=${2-}; shift 2 ;;
        --touch-view)          TOUCH_VIEW=${2-}; shift 2 ;;
        --bt-scan)             BT_SCAN=${2-}; shift 2 ;;
        --aplay-tree)          APLAY_TREE=${2-}; shift 2 ;;
        --output)              OUTPUT=${2-}; shift 2 ;;
        --authorized-keys)     AUTHORIZED_KEYS=${2-}; shift 2 ;;
        --generate-access-key) GENERATE_KEY_OUT=${2-}; shift 2 ;;
        --module)              MODULES+=("${2-}"); shift 2 ;;
        --kernel-version)      KERNEL_VERSION=${2-}; shift 2 ;;
        --root-password)       ROOT_PASSWORD=${2-}; ROOT_PASSWORD_SET=1; shift 2 ;;
        --firmware-dir)        FIRMWARE_DIR=${2-}; shift 2 ;;
        --compress)            COMPRESS=${2-}; shift 2 ;;
        -h|--help)             usage ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$BUSYBOX_DIR" ] || usage
[ -n "$OUTPUT" ]       || usage
if [ -z "$DROPBEAR_DIR" ] && [ -z "$DROPBEAR_TREE" ]; then
    usage
fi

case "$COMPRESS" in
    none|gzip) ;;
    *) die "--compress must be none or gzip (got: $COMPRESS)" ;;
esac

if [ "${#MODULES[@]}" -gt 0 ] && [ -z "$KERNEL_VERSION" ]; then
    die "--module requires --kernel-version (e.g. 5.15.0-piano)"
fi

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
INIT_SRC="$REPO_ROOT/initramfs/init"
case "$MODE" in
    test) ;;
    rootfs)
        INIT_SRC="$REPO_ROOT/initramfs/rootfs-init"
        [ -n "$AUTHORIZED_KEYS$GENERATE_KEY_OUT" ] || die 'rootfs mode requires SSH keys'
        [ "$ROOT_PASSWORD_SET" = 0 ] || die 'rootfs rescue is key-only'
        ;;
    *) die '--mode must be test or rootfs' ;;
esac
[ "$DIAGNOSTIC" = 0 ] || [ "$MODE" = rootfs ] || die '--diagnostic requires --mode rootfs'
TESTS_SRC="$REPO_ROOT/initramfs/tests"

missing=()
command -v cpio   >/dev/null 2>&1 || missing+=(cpio)
command -v gzip   >/dev/null 2>&1 || missing+=(gzip)
command -v sha256sum >/dev/null 2>&1 || missing+=(sha256sum)
[ "$COMPRESS" = gzip ] && ! command -v gzip >/dev/null 2>&1 && missing+=(gzip)
[ -x "$BUSYBOX_DIR/busybox" ] || missing+=("$BUSYBOX_DIR/busybox (static busybox)")
if [ -n "$DROPBEAR_TREE" ]; then
    [ -x "$DROPBEAR_TREE/usr/sbin/dropbear" ] || missing+=("$DROPBEAR_TREE/usr/sbin/dropbear")
    [ -x "$DROPBEAR_TREE/usr/bin/dropbearkey" ] || missing+=("$DROPBEAR_TREE/usr/bin/dropbearkey")
    [ -e "$DROPBEAR_TREE/lib/ld-linux-aarch64.so.1" ] || missing+=("$DROPBEAR_TREE/lib/ld-linux-aarch64.so.1 (runtime closure)")
    [ -e "$DROPBEAR_TREE/usr/lib/aarch64-linux-gnu/libgmp.so.10" ] \
        || missing+=("libgmp.so.10 (refresh the arm64 tools tree)")
else
    [ -x "$DROPBEAR_DIR/dropbear" ] || missing+=("$DROPBEAR_DIR/dropbear")
    [ -x "$DROPBEAR_DIR/dropbearkey" ] || missing+=("$DROPBEAR_DIR/dropbearkey")
fi
[ -f "$INIT_SRC" ] || missing+=("$INIT_SRC (initramfs/init)")
[ -n "$AUTHORIZED_KEYS" ] && [ ! -s "$AUTHORIZED_KEYS" ] && missing+=("$AUTHORIZED_KEYS (authorized keys)")
[ -n "$FIRMWARE_DIR" ] && [ ! -d "$FIRMWARE_DIR" ] && missing+=("$FIRMWARE_DIR (firmware dir)")
for m in "${MODULES[@]:-}"; do
    [ -n "$m" ] && [ ! -s "$m" ] && missing+=("$m (kernel module)")
done
if [ "${#MODULES[@]}" -gt 0 ] && ! command -v depmod >/dev/null 2>&1; then
    missing+=(depmod)
fi
if [ -n "$GENERATE_KEY_OUT" ] && ! command -v ssh-keygen >/dev/null 2>&1; then
    missing+=(ssh-keygen)
fi
if [ "$ROOT_PASSWORD_SET" = 1 ] && [ -n "$ROOT_PASSWORD" ] \
        && ! command -v openssl >/dev/null 2>&1; then
    missing+=(openssl)
fi

if [ "${#missing[@]}" -gt 0 ]; then
    printf 'build-initramfs: missing: %s\n' "${missing[*]}" >&2
    exit 1
fi

# busybox must be static (no interpreter deps inside initramfs)
if ldd "$BUSYBOX_DIR/busybox" >/dev/null 2>&1; then
    die "busybox at $BUSYBOX_DIR/busybox is dynamically linked; a static build is required"
fi

STAGING=$(mktemp -d "${TMPDIR:-/tmp}/piano-initramfs.XXXXXX")
trap 'rm -rf "$STAGING"' EXIT

mkdir -p \
    "$STAGING/bin" "$STAGING/sbin" "$STAGING/usr/bin" "$STAGING/usr/sbin" \
    "$STAGING/etc/dropbear" "$STAGING/root/.ssh" \
    "$STAGING/dev" "$STAGING/proc" "$STAGING/sys" "$STAGING/run" \
    "$STAGING/var/lib/misc" \
    "$STAGING/sys/kernel/config"

install -m 0755 "$INIT_SRC" "$STAGING/init"
# The same script ships as /beaconinit: ABL's v4 RAM boot concatenates the
# stock vendor ramdisk AFTER ours, so Android's first_stage init wins the
# /init cascade slot — the kernel's forced cmdline selects rdinit=/beaconinit
# instead (runbook §7.3; without this twin the RAM boot panics on
# "Failed to execute /beaconinit").
install -m 0755 "$INIT_SRC" "$STAGING/beaconinit"
if [ "$MODE" = rootfs ]; then
    rm "$STAGING/beaconinit"
    install -m 0755 "$INIT_SRC" "$STAGING/pianoinit"
    install -D -m 0755 "$REPO_ROOT/rootfs/overlay/usr/lib/piano/usb-network" \
        "$STAGING/usr/lib/piano/usb-network"
    if [ "$DIAGNOSTIC" = 1 ]; then
        mkdir -p "$STAGING/etc/piano"
        touch "$STAGING/etc/piano/diagnostic"
    fi
fi
install -m 0755 "$BUSYBOX_DIR/busybox" "$STAGING/bin/busybox"
if [ -n "$DROPBEAR_TREE" ]; then
    cp -a "$DROPBEAR_TREE/usr" "$STAGING/"
    # Some aarch64 tool trees provide /lib as a symlink into /usr/lib.
    # Preserve a real /lib in the initramfs so kernel modules and firmware
    # staged below it remain discoverable by the early userspace modprobe.
    mkdir -p "$STAGING/lib"
    cp -aL "$DROPBEAR_TREE/lib/." "$STAGING/lib/"
else
    install -m 0755 "$DROPBEAR_DIR/dropbear"     "$STAGING/usr/sbin/dropbear"
    install -m 0755 "$DROPBEAR_DIR/dropbearkey"  "$STAGING/usr/bin/dropbearkey"
fi

# --- aplay + audio test tone ---------------------------------------------------
if [ -n "$APLAY_TREE" ]; then
    [ -x "$APLAY_TREE/usr/bin/aplay" ] || die "no aplay binary at $APLAY_TREE/usr/bin/aplay"
    ( cd "$APLAY_TREE" && tar -cf - usr ) | ( cd "$STAGING" && tar -xf - )
    echo "build-initramfs: installed aplay from $APLAY_TREE"
fi

# Test tone: 3 s of 440 Hz + 880 Hz alternating sine, 16 kHz mono 16-bit PCM,
# generated deterministically here (python3 struct/math, no external file).
mkdir -p "$STAGING/usr/share"
python3 - "$STAGING/usr/share/piano-test-tone.wav" <<'WAVEOF' || die "tone generation failed"
import math, struct, sys
rate = 16000
frames = rate * 3
with open(sys.argv[1], "wb") as f:
    f.write(b"RIFF")
    f.write(struct.pack("<I", 36 + frames * 2))
    f.write(b"WAVEfmt ")
    f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
    f.write(b"data")
    f.write(struct.pack("<I", frames * 2))
    for i in range(frames):
        freq = 440 if (i // rate) % 2 == 0 else 880
        v = int(12000 * math.sin(2 * math.pi * freq * i / rate))
        f.write(struct.pack("<h", v))
WAVEOF
echo "build-initramfs: generated 3 s audio test tone"

# --- piano-pd-locator (our static SERVREG_LOC daemon) ------------------------
if [ -n "$PD_LOCATOR" ]; then
    [ -s "$PD_LOCATOR" ] || die "pd-locator binary missing: $PD_LOCATOR"
    file "$PD_LOCATOR" | grep -q 'ARM aarch64' \
        || die "staged pd-locator is not an arm64 ELF: $PD_LOCATOR"
    install -m 0755 "$PD_LOCATOR" "$STAGING/usr/sbin/piano-pd-locator"
    echo "build-initramfs: installed piano-pd-locator from $PD_LOCATOR"
fi

# --- piano-touch-view (our static THP frame viewer) --------------------------
if [ -n "$TOUCH_VIEW" ]; then
    [ -s "$TOUCH_VIEW" ] || die "touch-view binary missing: $TOUCH_VIEW"
    file "$TOUCH_VIEW" | grep -q 'ARM aarch64.*statically linked' \
        || die "staged touch-view is not a static arm64 ELF: $TOUCH_VIEW"
    install -m 0755 "$TOUCH_VIEW" "$STAGING/usr/bin/piano-touch-view"
    echo "build-initramfs: installed piano-touch-view from $TOUCH_VIEW"
fi

# --- piano-bt-scan (raw-HCI LE scanner, Bluetooth proof) ----------------------
if [ -n "$BT_SCAN" ]; then
    file "$BT_SCAN" | grep -q 'ARM aarch64.*statically linked' \
        || die "bt-scan is not a static arm64 ELF: $BT_SCAN"
    install -m 0755 "$BT_SCAN" "$STAGING/usr/bin/piano-bt-scan"
fi

# --- iw (WLAN nl80211 client) ------------------------------------------------
if [ -n "$IW_TREE" ]; then
    [ -x "$IW_TREE/usr/sbin/iw" ] || die "no iw binary at $IW_TREE/usr/sbin/iw"
    ( cd "$IW_TREE" && tar -cf - usr ) | ( cd "$STAGING" && tar -xf - )
    echo "build-initramfs: installed iw from $IW_TREE"
fi

# Minimal command symlinks; /init does `busybox --install -s` at runtime,
# but the earliest init lines need these before that install runs.
for cmd in sh mount mkdir ln echo cat ls modprobe; do
    ln -sf /bin/busybox "$STAGING/bin/$cmd"
done

# --- test suite -------------------------------------------------------------
if [ -d "$TESTS_SRC" ]; then
    for t in "$TESTS_SRC"/*; do
        [ -f "$t" ] || continue
        if [ "$MODE" = rootfs ] && [ "${t##*/}" != piano-qup-smmu ]; then continue; fi
        install -m 0755 "$t" "$STAGING/usr/bin/$(basename "$t")"
    done
fi

# --- SSH access -------------------------------------------------------------
if [ -n "$AUTHORIZED_KEYS" ]; then
    install -m 0600 "$AUTHORIZED_KEYS" "$STAGING/root/.ssh/authorized_keys"
elif [ -n "$GENERATE_KEY_OUT" ]; then
    [ -e "$GENERATE_KEY_OUT" ] && die "refusing to overwrite existing key $GENERATE_KEY_OUT"
    ssh-keygen -t ed25519 -N '' -C piano-test-image -f "$GENERATE_KEY_OUT" -q
    chmod 0600 "$GENERATE_KEY_OUT"
    install -m 0600 "$GENERATE_KEY_OUT.pub" "$STAGING/root/.ssh/authorized_keys"
fi

# Account database + optional root password auth. The password field of
# /etc/passwd holds a SHA-512 crypt hash (verified by the staged libcrypt
# through dropbear); an EMPTY --root-password sets an empty field, which
# dropbear's -B flag turns into "press enter to log in" on the USB link.
PW_FIELD=x
if [ "$ROOT_PASSWORD_SET" = 1 ]; then
    if [ -n "$ROOT_PASSWORD" ]; then
        PW_FIELD=$(openssl passwd -6 "$ROOT_PASSWORD")
        [ -n "$PW_FIELD" ] || die "openssl passwd failed"
    else
        PW_FIELD=""
    fi
fi
printf 'root:%s:0:0:root:/root:/bin/sh\n' "$PW_FIELD" > "$STAGING/etc/passwd"
printf 'root:x:0:\n' > "$STAGING/etc/group"

# --- kernel modules ---------------------------------------------------------
if [ "${#MODULES[@]}" -gt 0 ]; then
    MODDIR="$STAGING/lib/modules/$KERNEL_VERSION"
    mkdir -p "$MODDIR"
    STRIP=llvm-strip
    command -v "$STRIP" >/dev/null 2>&1 || STRIP="strip"
    for m in "${MODULES[@]}"; do
        [ -n "$m" ] || continue
        # normalise to the standard kernel/<subdir>/<name>.ko layout no
        # matter where the .ko came from (kernel build dir or a
        # modules_install closure root)
        rel="kernel/${m##*/kernel/}"
        d="$MODDIR/$(dirname "$rel")"
        mkdir -p "$d"
        # Strip debug sections: the kernel builds modules unstripped and the
        # raw set can be an order of magnitude larger than needed.
        "$STRIP" --strip-debug -o "$d/$(basename "$m")" "$m" \
            || install -m 0644 "$m" "$d/$(basename "$m")"
    done
    depmod -b "$STAGING" "$KERNEL_VERSION" \
        || die "depmod failed for $KERNEL_VERSION"
fi

# --- firmware (touch + WLAN/BT combo) ----------------------------------------
if [ -n "$FIRMWARE_DIR" ]; then
    n_fw=0
    if compgen -G "$FIRMWARE_DIR/novatek/*.bin" >/dev/null; then
        mkdir -p "$STAGING/lib/firmware/novatek"
        for f in "$FIRMWARE_DIR"/novatek/*.bin; do
            install -m 0644 "$f" "$STAGING/lib/firmware/novatek/"
            n_fw=$((n_fw + 1))
        done
    fi
    echo "build-initramfs: installed $n_fw novatek firmware blob(s)"

    # WLAN/BT: qca/hmtbtfw*.tlv + hmtnv* (hci_qca) and the ath12k tree
    # (amss/m3/bdwlan/board-2) are copied verbatim when present.
    if [ -d "$FIRMWARE_DIR/qca" ]; then
        mkdir -p "$STAGING/lib/firmware/qca"
        for f in "$FIRMWARE_DIR"/qca/*; do
            [ -f "$f" ] || continue
            install -m 0644 "$f" "$STAGING/lib/firmware/qca/"
        done
        echo "build-initramfs: installed qca BT firmware: $(find "$FIRMWARE_DIR/qca" -type f -printf '%f ')"
    fi
    if [ -d "$FIRMWARE_DIR/ath12k" ]; then
        mkdir -p "$STAGING/lib/firmware/ath12k"
        cp -a "$FIRMWARE_DIR"/ath12k/. "$STAGING/lib/firmware/ath12k/"
        echo "build-initramfs: installed ath12k WLAN firmware tree ($(find "$FIRMWARE_DIR/ath12k" -type f | wc -l) files)"
    fi

    # remoteproc firmware: adsp/cdsp segments staged by build-test-bootimg.sh
    # as qcom/sm8750/{adsp,cdsp}[._dtb].{mbn,bXX}; copied verbatim.
    if [ -d "$FIRMWARE_DIR/qcom/sm8750" ]; then
        mkdir -p "$STAGING/lib/firmware/qcom/sm8750"
        cp -a "$FIRMWARE_DIR"/qcom/sm8750/. "$STAGING/lib/firmware/qcom/sm8750/"
        echo "build-initramfs: installed remoteproc firmware tree ($(find "$FIRMWARE_DIR/qcom/sm8750" -type f | wc -l) files)"
    fi
fi

# Sanity: the init script must carry the NCM gadget path. A missing or
# renamed function here means the debug network cannot come up — refuse
# to ship such an image instead of failing silently on the device.
USB_SCRIPT="$STAGING/init"
[ "$MODE" != rootfs ] || USB_SCRIPT="$STAGING/usr/lib/piano/usb-network"
grep -q 'functions/ncm\.usb0' "$USB_SCRIPT" \
    || die "missing NCM gadget setup"
grep -q 'usb_gadget/piano' "$USB_SCRIPT" \
    || die "missing piano USB gadget setup"

# Archive IDs describe the target, not the unprivileged host builder.
# Dropbear rejects root's authorized_keys if its parents have the host UID.
( cd "$STAGING" && find . -print0 | cpio --null -o --format=newc --owner=0:0 ) > "$OUTPUT.cpio" \
    || die "cpio failed"

case "$COMPRESS" in
    gzip) gzip -9 -c "$OUTPUT.cpio" > "$OUTPUT" ;;
    none) mv "$OUTPUT.cpio" "$OUTPUT" ;;
esac
rm -f "$OUTPUT.cpio"

[ -s "$OUTPUT" ] || die "cpio output is empty: $OUTPUT"

echo "build-initramfs: wrote $OUTPUT ($(wc -c < "$OUTPUT") bytes, compress=$COMPRESS)"
sha256sum "$OUTPUT"
