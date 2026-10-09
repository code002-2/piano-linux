#!/usr/bin/env bash
# Assemble the local-only ext4 userdata image; never writes a block device.
set -euo pipefail
ROOTFS='' MODULES='' KVER='' FIRMWARE='' TOUCH='' CAMERAD='' BUSYBOX='' OUTPUT='' SIZE=12G
die() { echo "assemble-rootfs-image: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        --rootfs) ROOTFS=${2:?}; shift 2 ;;
        --modules) MODULES=${2:?}; shift 2 ;;
        --kernel-release) KVER=${2:?}; shift 2 ;;
        --firmware-dir) FIRMWARE=${2:?}; shift 2 ;;
        --touch-view) TOUCH=${2:?}; shift 2 ;;
        --camera-daemon) CAMERAD=${2:?}; shift 2 ;;
        --busybox) BUSYBOX=${2:?}; shift 2 ;;
        --output-dir) OUTPUT=${2:?}; shift 2 ;;
        --image-size) SIZE=${2:?}; shift 2 ;;
        *) die "unknown argument $1" ;;
    esac
done
[ "$(id -u)" = 0 ] || die 'run as root to preserve rootfs ownership'
[ -x "$ROOTFS/sbin/init" ] || die 'incomplete rootfs build'
[ -f "$ROOTFS/../COMPLETE" ] || die 'incomplete rootfs build'
[ ! -e "$ROOTFS/etc/piano/kernel-release" ] || die 'use a pristine base, not an already assembled rootfs'
[ -d "$MODULES/lib/modules/$KVER" ] || die 'missing kernel module tree'
[ -z "$FIRMWARE" ] || [ -d "$FIRMWARE/ath12k/PEACH/hw2.0" ] || die 'missing local firmware'
[ -x "$TOUCH" ] || die 'missing arm64 helpers'
[ -x "$CAMERAD" ] || die 'missing arm64 helpers'
[ -x "$BUSYBOX" ] || die 'missing arm64 helpers'
[ -n "$OUTPUT" ] || die '--output-dir required'
mkdir -p "$OUTPUT"
[ ! -e "$OUTPUT/userdata.img" ] || die 'userdata output already exists'
[ ! -e "$OUTPUT/userdata.raw.img" ] || die 'userdata output already exists'
[ ! -e "$OUTPUT/rootfs" ] || die 'assembled rootfs output already exists'
# Do not contaminate the reusable, firmware-free base (especially across CI/local builds).
cp -a --reflink=auto "$ROOTFS" "$OUTPUT/rootfs"
ROOTFS=$OUTPUT/rootfs
mkdir -p "$ROOTFS/usr/lib/modules" "$ROOTFS/usr/lib/firmware" "$ROOTFS/etc/piano"
# Module and firmware trees belong to the building user; install them as root.
cp -a --no-preserve=ownership "$MODULES/lib/modules/$KVER" "$ROOTFS/usr/lib/modules/"
rm -f "$ROOTFS/usr/lib/modules/$KVER/build" "$ROOTFS/usr/lib/modules/$KVER/source"
if [ -n "$FIRMWARE" ]; then
    cp -a --no-preserve=ownership "$FIRMWARE/." "$ROOTFS/usr/lib/firmware/"
    echo local > "$ROOTFS/etc/piano/firmware-status"
else
    echo not-included > "$ROOTFS/etc/piano/firmware-status"
fi
install -m 0755 "$TOUCH" "$ROOTFS/usr/bin/piano-touch-view"
install -m 0755 "$CAMERAD" "$ROOTFS/usr/lib/piano/piano-camerad"
install -m 0755 "$BUSYBOX" "$ROOTFS/usr/lib/piano/busybox"
printf '%s\n' "$KVER" > "$ROOTFS/etc/piano/kernel-release"
depmod -b "$ROOTFS" "$KVER"
truncate -s "$SIZE" "$OUTPUT/userdata.raw.img"
mkfs.ext4 -F -L piano-root -m 0 -E lazy_itable_init=0,lazy_journal_init=0 \
    -d "$ROOTFS" "$OUTPUT/userdata.raw.img"
e2fsck -fn "$OUTPUT/userdata.raw.img"
# Free space must be DONT_CARE, not img2simg FILL chunks (ABL writes FILL slowly).
"$(dirname "$0")/ext4-to-simg.py" "$OUTPUT/userdata.raw.img" "$OUTPUT/userdata.img"
sha256sum "$OUTPUT/userdata.raw.img" "$OUTPUT/userdata.img"
