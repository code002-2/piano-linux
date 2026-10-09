#!/usr/bin/env bash
# Build the matched local GNOME boot.img + dtbo.img + userdata.img set.
# Run from the workspace; firmware and outputs remain ignored/local.
set -euo pipefail
W=$(cd "$(dirname "$0")/.." && pwd)
K=$W/kernel D=$W
JOBS=$(nproc) OUTPUT=$W/out/gnome-image BASE='' KEYS='' SIZE=12G KERNEL_ONLY=0
WITHOUT_FIRMWARE=0 DIAGNOSTIC=0 FW_TREE='' MESA_DIR='' SENSORS_DIR='' TOPOLOGY='' LOOPBACK=''
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS=${2:?}; shift 2 ;;
        --output) OUTPUT=$(realpath -m "${2:?}"); shift 2 ;;
        --rootfs-build) BASE=$(realpath "${2:?}"); shift 2 ;;
        --authorized-keys) KEYS=$(realpath "${2:?}"); shift 2 ;;
        --image-size) SIZE=${2:?}; shift 2 ;;
        --kernel-only) KERNEL_ONLY=1; shift ;;
        --without-firmware) WITHOUT_FIRMWARE=1; shift ;;
        --firmware-tree) FW_TREE=$(realpath "${2:?}"); shift 2 ;;
        --diagnostic) DIAGNOSTIC=1; shift ;;
        --mesa-dir) MESA_DIR=$(realpath "${2:?}"); shift 2 ;;
        --sensors-dir) SENSORS_DIR=$(realpath "${2:?}"); shift 2 ;;
        --audioreach-topology) TOPOLOGY=$(realpath "${2:?}"); shift 2 ;;
        --v4l2loopback) LOOPBACK=$(realpath "${2:?}"); shift 2 ;;
        -h|--help) echo 'Usage: scripts/build-rootfs-image.sh [--jobs N] [--output DIR] [--rootfs-build DIR] [--authorized-keys FILE] [--image-size 12G] [--kernel-only [--diagnostic]] [--without-firmware | --firmware-tree DIR] [--mesa-dir DIR] [--sensors-dir DIR] [--audioreach-topology DIR] [--v4l2loopback DIR]'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || exit 2
[ -z "$FW_TREE" ] || [ "$WITHOUT_FIRMWARE" = 0 ] || { echo '--firmware-tree conflicts with --without-firmware' >&2; exit 2; }
[ "$DIAGNOSTIC" = 0 ] || [ "$KERNEL_ONLY" = 1 ] || { echo '--diagnostic requires --kernel-only' >&2; exit 2; }
[ ! -e "$OUTPUT" ] || { echo 'Choose a fresh output directory' >&2; exit 1; }
for cmd in clang ld.lld cpio depmod modinfo dtc python3 mkfs.ext4 dumpe2fs; do command -v "$cmd" >/dev/null; done
ROOT=()
if [ "$KERNEL_ONLY" = 0 ]; then
    if [ "$(id -u)" != 0 ]; then ROOT=(sudo); sudo -v; fi
fi
mkdir -p "$OUTPUT"
OUTPUT=$(realpath "$OUTPUT")
if [ -z "$KEYS" ]; then
    ssh-keygen -q -t ed25519 -N '' -C piano -f "$OUTPUT/access-key"
    KEYS=$OUTPUT/access-key.pub
fi
if [ "$KERNEL_ONLY" = 0 ]; then
    if [ -z "$BASE" ]; then
        BASE=$OUTPUT/rootfs-build
        # --mesa-dir: piano-mesa runtime packages (Adreno 830); without it the
        # rootfs keeps Debian's Mesa, which does not know the GPU.
        # --sensors-dir: piano-sensors runtime packages; without them the
        # image has no sensors (no screen rotation).
        "${ROOT[@]}" "$W/scripts/build-rootfs.sh" --suite trixie --output "$BASE" --authorized-keys "$KEYS" \
            ${MESA_DIR:+--mesa-dir "$MESA_DIR"} ${SENSORS_DIR:+--userspace-dir "$SENSORS_DIR"}
    fi
    [ -f "$BASE/COMPLETE" ] || { echo 'Rootfs bootstrap incomplete' >&2; exit 1; }
    # A reused base gets the same key as the new rescue image.
    for home in root home/piano; do
        "${ROOT[@]}" install -m 0600 "$KEYS" "$BASE/rootfs/$home/.ssh/authorized_keys"
    done
    "${ROOT[@]}" chroot "$BASE/rootfs" chown -R piano:piano /home/piano/.ssh
fi
O=$K/out/rootfs-image
STAGE=$OUTPUT/stage
TOOLS=$W/out/arm64-tools
mkdir -p "$O" "$STAGE"
MAKE=(make -C "$K" ARCH=arm64 LLVM=1 O="$O")
if command -v ccache >/dev/null; then MAKE+=("CC=ccache clang"); fi
"${MAKE[@]}" piano_defconfig
"$K/scripts/kconfig/merge_config.sh" -m -O "$O" "$O/.config" "$K/arch/arm64/configs/piano_rootfs.config"
"${MAKE[@]}" olddefconfig
rm -f "$O/include/config/kernel.release" "$O/include/generated/utsrelease.h" "$O/.version"
"${MAKE[@]}" prepare
find "$O" \( -name '*.mod.c' -o -name '*.ko' \) -delete
"${MAKE[@]}" -j"$JOBS" Image modules
KVER=$(cat "$O/include/config/kernel.release")
"${MAKE[@]}" -j"$JOBS" modules_install INSTALL_MOD_PATH="$STAGE/modules" INSTALL_MOD_STRIP=1
# --v4l2loopback: umlaeute/v4l2loopback checkout (GPL-2.0), built out of
# tree against this kernel; without it the cameras are not offered to
# applications (piano-camerad has nothing to feed). Built from a copy so
# that no objects land in the checkout.
if [ -n "$LOOPBACK" ]; then
    cp -a "$LOOPBACK" "$STAGE/v4l2loopback"
    rm -rf "$STAGE/v4l2loopback/.git"
    "${MAKE[@]}" -j"$JOBS" M="$STAGE/v4l2loopback" modules
    "${MAKE[@]}" M="$STAGE/v4l2loopback" modules_install INSTALL_MOD_PATH="$STAGE/modules" \
        INSTALL_MOD_DIR=updates INSTALL_MOD_STRIP=1
fi
while IFS= read -r -d '' ko; do
    [[ "$(modinfo -F vermagic "$ko")" == "$KVER "* ]] || { echo "Stale module: $ko" >&2; exit 1; }
done < <(find "$STAGE/modules" -name '*.ko*' -print0)
FW_ARGS=()
if [ "$WITHOUT_FIRMWARE" = 0 ]; then
    if [ -n "$FW_TREE" ]; then
        # A piano-firmware checkout: already in /usr/lib/firmware layout.
        (cd "$FW_TREE" && sha256sum --check --quiet SHA256SUMS)
        cp -a "$FW_TREE" "$STAGE/firmware"
    else
        "$W/scripts/stage-piano-firmware.sh" "$W/local/firmware" "$STAGE/firmware"
    fi
    FW_ARGS=(--firmware-dir "$STAGE/firmware")
    # --audioreach-topology: linux-msm/audioreach-topology checkout (m4
    # macros) for the piano AudioReach topology; without it the image has
    # no audio graphs.
    [ -z "$TOPOLOGY" ] || "$W/scripts/build-topology.sh" "$TOPOLOGY" "$STAGE/firmware"
fi
"${MAKE[@]}" headers_install INSTALL_HDR_PATH="$STAGE/uapi"
"$W/scripts/build-touch-view.sh" --uapi "$STAGE/uapi" --sysroot "$TOOLS/musl-sysroot" --output "$STAGE/piano-touch-view"
"$W/scripts/build-touch-view.sh" --uapi "$STAGE/uapi" --sysroot "$TOOLS/musl-sysroot" \
    --source "$W/camera/piano-camerad.c" --output "$STAGE/piano-camerad"
# Neither UFS host nor PHY may probe before the initramfs debug network.
UFS_MODULES=()
for module in phy_qcom_qmp_ufs ufs_qcom; do
    dependencies=$(modprobe -S "$KVER" -d "$STAGE/modules" --show-depends "$module")
    while read -r kind ko rest; do
        [ "$kind" != insmod ] || UFS_MODULES+=(--module "$ko")
    done <<< "$dependencies"
done
[ "${#UFS_MODULES[@]}" -gt 0 ] || { echo 'Missing UFS module closure' >&2; exit 1; }
INIT_ARGS=()
[ "$DIAGNOSTIC" = 0 ] || INIT_ARGS+=(--diagnostic)
"$W/scripts/build-initramfs.sh" --mode rootfs --busybox "$TOOLS/busybox" \
    --dropbear-tree "$TOOLS/dropbear/tree" --authorized-keys "$KEYS" \
    --kernel-version "$KVER" "${UFS_MODULES[@]}" "${INIT_ARGS[@]}" --output "$STAGE/initramfs.cpio.gz"
"$K/scripts/config" --file "$O/.config" --set-str INITRAMFS_SOURCE "$STAGE/initramfs.cpio.gz"
"${MAKE[@]}" olddefconfig
for option in CONFIG_CMDLINE_FORCE=y CONFIG_EXT4_FS=y CONFIG_DRM_SIMPLEDRM=y \
    CONFIG_SCSI_UFSHCD=m CONFIG_SCSI_UFS_QCOM=m CONFIG_PHY_QCOM_QMP_UFS=m CONFIG_USB_CONFIGFS_NCM=y \
    CONFIG_PINCTRL_SM8750=m CONFIG_QCOM_GPI_DMA=m CONFIG_PHY_QCOM_QMP_PCIE=m; do
    grep -qxF "$option" "$O/.config" || { echo "Required: $option" >&2; exit 1; }
done
"${MAKE[@]}" -j"$JOBS" Image
[ "$(cat "$O/include/config/kernel.release")" = "$KVER" ]
"$W/scripts/build-test-bootimg.sh" --kernel-dir "$O" --output-dir "$OUTPUT" --dtbo-source "$W/boot/dtbo-piano-power.dts" --mode rootfs
if [ "$KERNEL_ONLY" = 0 ]; then
    "${ROOT[@]}" "$W/scripts/assemble-rootfs-image.sh" --rootfs "$BASE/rootfs" \
        --modules "$STAGE/modules" --kernel-release "$KVER" "${FW_ARGS[@]}" \
        --touch-view "$STAGE/piano-touch-view" --camera-daemon "$STAGE/piano-camerad" --busybox "$TOOLS/busybox/busybox" \
        --output-dir "$OUTPUT" --image-size "$SIZE"
    cp "$BASE/build-manifest.txt" "$OUTPUT/packages.txt"
fi
cp "$O/.config" "$OUTPUT/kernel.config"
{
    sha256sum "$K/arch/arm64/configs/piano_rootfs.config" "$W/scripts/build-rootfs-image.sh"
    (cd "$D" && git ls-files -z --cached --others --exclude-standard | sort -zu | xargs -0 sha256sum)
} > "$OUTPUT/SOURCE-SHA256SUMS"
{
    echo "Piano GNOME image set: without_firmware=$WITHOUT_FIRMWARE"
    if [ -n "$FW_TREE" ]; then
        echo "firmware=piano-firmware $(git -C "$FW_TREE" rev-parse HEAD 2>/dev/null || echo unknown); see its README compliance statement"
    elif [ "$WITHOUT_FIRMWARE" = 0 ]; then
        echo 'Contains locally staged proprietary firmware; do not publish'
    fi
    echo "kernel=$KVER kernel_only=$KERNEL_ONLY diagnostic=$DIAGNOSTIC"
    for repo in "$W" "$K" "$D"; do
        echo "source=$repo $(git -C "$repo" rev-parse HEAD)"
        git -C "$repo" diff --binary HEAD | sha256sum
    done
    echo 'SOURCE-SHA256SUMS includes packaging files and the kernel profile, including uncommitted/new files.'
    echo 'root=PARTLABEL=userdata; ext4; GNOME llvmpipe; key-only USB SSH 10.42.0.2'
    if [ "$DIAGNOSTIC" = 1 ]; then
        echo 'DIAGNOSTIC: NCM/SSH before SMMU/UFS setup; each storage stage waits for /run/piano-next.'
        echo 'Do not flash userdata. Stream /dev/kmsg over SSH before advancing.'
        echo 'The final diagnostic stage mounts userdata ro,noload; Debian handoff is disabled.'
    fi
    echo 'WARNING: flashing userdata destroys Android data; booting Android can overwrite Debian.'
    echo 'Keep stock slot A, vendor_boot and init_boot. Never flash bootloader-chain partitions.'
    echo 'Check current-slot=b and userdata partition-size before any write; RAM boot preferred.'
    (cd "$OUTPUT" && sha256sum boot.img dtbo.img kernel.config SOURCE-SHA256SUMS)
    if [ "$KERNEL_ONLY" = 0 ]; then (cd "$OUTPUT" && sha256sum userdata.img userdata.raw.img packages.txt); fi
} > "$OUTPUT/MANIFEST.txt"
echo "Built $OUTPUT (kernel-only=$KERNEL_ONLY); see MANIFEST.txt. No device writes performed."
