#!/usr/bin/env bash
# Add a signed AVB hash footer to a boot image so stock ABL loads it from
# boot_b on a cold boot.
#
# Stock vbmeta chains "boot" (rollback index location 3), so ABL rejects a
# boot_b image without a footer ("Load Error") and treats a rollback index
# below the stored one as fatal even when unlocked. A footer signed with any
# RSA4096 key only fails the public-key check, which an unlocked device
# tolerates, so a throwaway key is generated per build unless one is given.
#
# The rollback index and props mirror the stock boot.img footer of
# OS3.0.308.0.WPYCNXM. Location 3 is shared by both slots: never raise it
# above stock, or a higher value may be recorded against stock slot A.
# Bump it only when a stock OTA raises the stock boot rollback index.
#
# usage: sign-boot-avb.sh <boot.img> [rsa4096-key.pem]
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
AVBTOOL="$REPO_ROOT/avb/avbtool.py"
PARTITION_SIZE=$((0x6000000))   # boot_b: getvar partition-size:boot_b
ROLLBACK_INDEX=1785542400

die() { echo "sign-boot-avb: $*" >&2; exit 1; }

case $# in 1 | 2) ;; *) die "usage: $0 <boot.img> [rsa4096-key.pem]" ;; esac
IMAGE=$1
[ -f "$IMAGE" ] || die "no such image: $IMAGE"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
KEY=${2:-}
if [ -z "$KEY" ]; then
    KEY="$WORK/key.pem"
    openssl genrsa -out "$KEY" 4096 2>/dev/null || die "openssl genrsa failed"
fi

python3 "$AVBTOOL" add_hash_footer --image "$IMAGE" \
    --partition_name boot --partition_size "$PARTITION_SIZE" \
    --algorithm SHA256_RSA4096 --key "$KEY" \
    --rollback_index "$ROLLBACK_INDEX" \
    --prop com.android.build.boot.os_version:15 \
    --prop com.android.build.boot.fingerprint:Xiaomi/piano/piano:15/AQ3A.250226.002/OS3.0.308.0.WPYCNXM:user/release-keys \
    --prop com.android.build.boot.security_patch:2026-08-01 \
    || die "add_hash_footer failed"

python3 "$AVBTOOL" info_image --image "$IMAGE" > "$WORK/info" || die "cannot read back footer"
grep -q "^Rollback Index: *$ROLLBACK_INDEX\$" "$WORK/info" || die "rollback index read-back mismatch"
grep -q "^Algorithm: *SHA256_RSA4096\$" "$WORK/info" || die "algorithm read-back mismatch"
echo "sign-boot-avb: signed $IMAGE (boot, $PARTITION_SIZE B, rollback $ROLLBACK_INDEX)"
