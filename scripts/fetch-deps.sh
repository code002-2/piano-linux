#!/bin/sh
set -eu
W=$(cd "$(dirname "$0")/.." && pwd)
KERNEL_REPO=${KERNEL_REPO:-https://github.com/code002-2/linux.git}
KERNEL_REF=${KERNEL_REF:-piano-7.2.6}
FIRMWARE_REPO=${FIRMWARE_REPO:-https://github.com/blu-sharky/piano-firmware.git}
FIRMWARE_REF=${FIRMWARE_REF:-main}
if [ ! -d "$W/kernel/.git" ]; then
    rm -rf "$W/kernel"
    git clone --depth 1 -b "$KERNEL_REF" "$KERNEL_REPO" "$W/kernel"
fi
if [ ! -d "$W/firmware/.git" ]; then
    rm -rf "$W/firmware"
    git clone --depth 1 -b "$FIRMWARE_REF" "$FIRMWARE_REPO" "$W/firmware"
fi
echo "kernel:   $(git -C "$W/kernel" rev-parse --short HEAD) ($KERNEL_REF)"
echo "firmware: $(git -C "$W/firmware" rev-parse --short HEAD) ($FIRMWARE_REF)"
