#!/usr/bin/env bash
# build-test-image.sh — one-command builder for the piano RAM-boot test
# image, tying the two component repos together from the workspace root.
#
# Usage:
#   scripts/build-test-image.sh [--jobs N]
#
# Why this lives in the umbrella repo: the test image consumes artifacts
# from BOTH component repos (linux-piano kernel outputs + debian-piano
# packaging scripts). Per the workspace layout, cross-repo orchestration
# belongs here (scripts/), NOT inside linux-piano or hardcoded with
# relative ../ paths inside debian-piano.
#
# Produces exactly THREE deliverables in out/test-image/
# (packed and round-trip-verified by scripts/build-test-bootimg.sh):
#   boot.img  dtbo.img  MANIFEST.txt
#
# Steps:
#   1. Refuses to run unless linux-piano is on piano/test-bringup and
#      debian-piano on main, both clean.
#   2. Configures the kernel from the committed piano_defconfig in
#      linux-piano/out/test-image (boot-critical options are pinned there
#      and gated below), regenerates the release string, then builds Image
#      and modules. Every staged module must carry that release (vermagic).
#   3. Builds the debug initramfs: busybox/telnetd NCM environment, the
#      touch module closure (TLMM, GPI, GENI SPI, NT36532, uinput) under
#      /lib/modules/<release>/, both piano touch firmware blobs, the test
#      scripts and the static piano-touch-view helper. Nothing touch
#      related is loaded at boot; piano-tests / piano-touch-test do that.
#   4. Embeds it via CONFIG_INITRAMFS_SOURCE, rebuilds Image and packs the
#      image set with the touch overlay (boot/dtbo-piano-touch-v2.dts).
#
# Host tools: clang/lld (kernel and helper), dtc, python3, cpio, depmod,
# modinfo, plus the arm64 userland staged by
#   scripts/fetch-arm64-tools.sh --output-dir out/arm64-tools
# (umbrella out/, so debian-piano/out holds nothing but the image set) and
# the touch firmware extracted to local/firmware/odm/firmware/.

set -euo pipefail

usage() {
    sed -n '2,36p' "$0"; exit 2
}

die() {
    echo "build-test-image: error: $*" >&2
    exit 1
}

JOBS=$(nproc)

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs)    JOBS=${2-}; shift 2 ;;
        -h|--help) usage ;;
        *) die "unknown option: $1" ;;
    esac
done

case "$JOBS" in ''|*[!0-9]*) die "--jobs must be a positive integer (got '$JOBS')" ;; esac
[ "$JOBS" -ge 1 ] || die "--jobs must be >= 1"

WORKSPACE=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="$WORKSPACE/kernel"
DEBIAN="$WORKSPACE"
KERNEL_OUT="$KERNEL/out/test-image"
OUTPUT_DIR="$DEBIAN/out/test-image"
STAGE="$KERNEL_OUT/stage"
TOOLS="$WORKSPACE/out/arm64-tools"
TOUCH_FIRMWARE_SRC="$WORKSPACE/local/firmware/odm/firmware"

check_repo() { # check_repo DIR BRANCH
    local branch
    [ -e "$1/.git" ] || die "$1 not found (git submodule update --init)"
    branch=$(git -C "$1" rev-parse --abbrev-ref HEAD)
    [ "$branch" = "$2" ] || die "$(basename "$1") is on '$branch'; this image requires $2
(booting another branch would silently change what is being tested)"
    [ -z "$(git -C "$1" status --porcelain)" ] \
        || die "$(basename "$1") has uncommitted changes — commit or stash first"
}
check_repo "$KERNEL" piano/wlanbt
check_repo "$DEBIAN" bp/wlanbt

BUSYBOX="$TOOLS/busybox"
DROPBEAR_TREE="$TOOLS/dropbear/tree"
SYSROOT="$TOOLS/musl-sysroot"
for f in "$BUSYBOX/busybox" "$DROPBEAR_TREE/usr/sbin/dropbear" \
         "$SYSROOT/usr/lib/libclang_rt.builtins-aarch64.a"; do
    [ -e "$f" ] || die "no staged arm64 tools in $TOOLS ($f missing)
  run: scripts/fetch-arm64-tools.sh --output-dir $TOOLS"
done
[ -d "$TOUCH_FIRMWARE_SRC" ] || die "missing extracted touch firmware: $TOUCH_FIRMWARE_SRC"

echo "build-test-image: linux-piano $(git -C "$KERNEL" rev-parse --short HEAD)," \
     "debian-piano $(git -C "$DEBIAN" rev-parse --short HEAD), out=$KERNEL_OUT"

# --- kernel: committed defconfig, fresh release string ------------------------
mkdir -p "$KERNEL_OUT"
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" piano_defconfig
rm -f "$KERNEL_OUT/include/config/kernel.release" \
      "$KERNEL_OUT/include/generated/utsrelease.h" "$KERNEL_OUT/.version"
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" prepare
find "$KERNEL_OUT" \( -name '*.mod.c' -o -name '*.ko' \) -delete

# --- critical-option gate (the 09-22 lesson, now enforced) ---------------------
absent=()
for pair in CONFIG_CMDLINE_FORCE=y CONFIG_DRM_SIMPLEDRM=y \
            CONFIG_PCIE_QCOM=y CONFIG_PHY_QCOM_QMP_PCIE=m \
            CONFIG_FRAMEBUFFER_CONSOLE=y CONFIG_FONT_TER16x32=y \
            CONFIG_SM_TCSRCC_8750=y CONFIG_PSTORE_RAM=y \
            CONFIG_PINCTRL_SM8750=m CONFIG_QCOM_GPI_DMA=m \
            CONFIG_SPI_QCOM_GENI=m CONFIG_TOUCHSCREEN_NT36532E_SPI=m \
            CONFIG_INPUT_UINPUT=m; do
    grep -qxF "$pair" "$KERNEL_OUT/.config" || absent+=("$pair")
done
if [ "${#absent[@]}" -gt 0 ]; then
    printf 'build-test-image: piano_defconfig lost required options:\n' >&2
    printf '  %s\n' "${absent[@]}" >&2
    die "fix arch/arm64/configs/piano_defconfig"
fi

echo "build-test-image: building Image and modules (-j$JOBS)"
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" -j"$JOBS" Image modules

KVER=$(sed -n 's/^#define UTS_RELEASE "\(.*\)"$/\1/p' \
       "$KERNEL_OUT/include/generated/utsrelease.h")
[ -n "$KVER" ] || die "cannot determine kernel release"
case "$KVER" in *dirty*) die "kernel release $KVER is dirty" ;; esac

# --- module closure (touch + WLAN/BT ladder), resolved via depmod -------------
# modules_install + modprobe --show-depends walks the real dependency graph,
# so no hand-maintained .ko list can go stale (the 09-21 lesson: a missing
# transitive dep silently killed pcie0).  pcie-qcom itself is a bool option
# in this kernel (built-in): it stays inert until the initramfs WLAN phase
# loads phy-qcom-qmp-pcie (the root-port PHY provider).  Socket/alias-bound
# modules are seeded explicitly: qrtr + qrtr_mhi carry ath12k's QMI and are
# never pulled in by symbol dependencies.
MOD_INSTALL="$KERNEL_OUT/mod-closure"
CLOSURE="$KERNEL_OUT/mod-closure.txt"
rm -rf "$MOD_INSTALL"
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" -j"$JOBS" modules_install \
     INSTALL_MOD_PATH="$MOD_INSTALL" INSTALL_MOD_STRIP=1 >/dev/null \
  || die "modules_install failed"

: > "$CLOSURE"
for mod in pinctrl_sm8750 nt36532e_ts uinput \
           gpio_shared_proxy pwrseq_qcom_wcn pci_pwrctrl_pwrseq \
           phy_qcom_qmp_pcie qrtr qrtr_mhi ath12k_wifi7 hci_uart; do
    modprobe -S "$KVER" -d "$MOD_INSTALL" --show-depends "$mod" \
        >> "$CLOSURE" 2>/dev/null \
      || die "cannot resolve module closure for $mod (is it built?)"
done

MODULES=()
while read -r ko; do
    [ -s "$ko" ] || die "closure module missing: $ko"
    [ "$(modinfo -F vermagic "$ko")" = "$KVER SMP preempt mod_unload aarch64" ] \
        || die "stale vermagic in $ko"
    MODULES+=("$ko")
done < <(awk '$1 == "insmod" {print $2}' "$CLOSURE" | sort -u)
[ "${#MODULES[@]}" -ge 14 ] \
    || die "module closure suspiciously small (${#MODULES[@]} modules)"
echo "build-test-image: module closure = ${#MODULES[@]} modules"

# --- firmware + helper staging (inside the ignored kernel out dir) ------------
rm -rf "$STAGE"
mkdir -p "$STAGE"
"$DEBIAN/scripts/stage-piano-firmware.sh" "$WORKSPACE/local/firmware" "$STAGE/firmware"
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" headers_install \
    INSTALL_HDR_PATH="$STAGE/uapi"
"$DEBIAN/scripts/build-touch-view.sh" --uapi "$STAGE/uapi" \
    --sysroot "$SYSROOT" --output "$STAGE/piano-touch-view"
"$DEBIAN/scripts/build-touch-view.sh" --uapi "$STAGE/uapi" \
    --sysroot "$SYSROOT" --output "$STAGE/piano-bt-scan" \
    --source "$DEBIAN/initramfs/bt-scan/piano-bt-scan.c"

# --- debug initramfs -----------------------------------------------------------
INITRAMFS="$KERNEL_OUT/initramfs.cpio.gz"
echo "build-test-image: building initramfs -> $INITRAMFS"
INITRAMFS_ARGS=(
    "$DEBIAN/scripts/build-initramfs.sh"
    --busybox "$BUSYBOX" --dropbear-tree "$DROPBEAR_TREE"
    --output "$INITRAMFS" --kernel-version "$KVER"
    --firmware-dir "$STAGE/firmware" --touch-view "$STAGE/piano-touch-view"
    --bt-scan "$STAGE/piano-bt-scan"
    --iw-tree "$TOOLS/iw/tree" \
)
for module in "${MODULES[@]}"; do
    INITRAMFS_ARGS+=(--module "$module")
done
"${INITRAMFS_ARGS[@]}"

# --- embed it into the kernel image ----------------------------------------------
"$KERNEL/scripts/config" --file "$KERNEL_OUT/.config" \
    --set-str CONFIG_INITRAMFS_SOURCE "$INITRAMFS"
# the new INITRAMFS_* sub-options must get their defaults non-interactively
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" olddefconfig
for pair in CONFIG_CMDLINE_FORCE=y CONFIG_FONT_TER16x32=y; do
    grep -qxF "$pair" "$KERNEL_OUT/.config" || die "olddefconfig dropped $pair"
done
make -C "$KERNEL" ARCH=arm64 LLVM=1 O="$KERNEL_OUT" -j"$JOBS" Image
[ "$(sed -n 's/^#define UTS_RELEASE "\(.*\)"$/\1/p' \
     "$KERNEL_OUT/include/generated/utsrelease.h")" = "$KVER" ] \
    || die "kernel release changed during the final Image build"

"$DEBIAN/scripts/build-test-bootimg.sh" \
    --kernel-dir "$KERNEL_OUT" --output-dir "$OUTPUT_DIR" \
    --dtbo-source "$DEBIAN/boot/dtbo-piano-wlanbt.dts"

{
    echo
    echo "sources:"
    echo "  umbrella     $(git -C "$WORKSPACE" rev-parse HEAD)"
    echo "  linux-piano  $(git -C "$KERNEL" rev-parse HEAD) ($KVER)"
    echo "  debian-piano $(git -C "$DEBIAN" rev-parse HEAD)"
    echo
    echo "on the device (telnet 10.42.0.2 23): piano-tests; touch = menu 1"
    echo "  (piano-touch-test: TLMM -> SPI -> NT36532 firmware -> 30 s draw test)"
} >> "$OUTPUT_DIR/MANIFEST.txt"

echo
echo "build-test-image: complete. Deliverables (RAM boot; only dtbo_b is flashed):"
echo "  $OUTPUT_DIR/boot.img"
echo "  $OUTPUT_DIR/dtbo.img"
echo "  $OUTPUT_DIR/MANIFEST.txt   (recipe + safety notes)"
