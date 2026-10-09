#!/usr/bin/env bash
# fetch-arm64-tools.sh — fetch static arm64 busybox + dropbear from Debian.
#
# Usage: scripts/fetch-arm64-tools.sh [--suite SUITE] [--output-dir DIR]
#
# Downloads arm64 Debian packages, extracts them with ar(1) + tar(1) (no
# dpkg needed) and stages a ready-to-embed userland tree:
#
#   DIR/musl-sysroot/        Alpine musl-dev (aarch64) sysroot used to
#                            cross-compile the static piano-pd-locator and
#                            piano-touch-view (crt1.o + libc.a, plus the
#                            compiler-rt builtins that musl's long double
#                            printf needs; toolchain material, never
#                            shipped into any repository)
#   DIR/iw/tree/             iw + its shared-library closure (libnl-3,
#                            libnl-genl-3, libc) — used by the WLAN test
#   DIR/dropbear/tree/       full dropbear userland tree:
#                             usr/sbin/dropbear, usr/bin/dropbearkey and
#                             the shared-library closure under lib/
#                             (Debian has no static dropbear; the runtime
#                             closure — libc6, libcrypt1, libtomcrypt1,
#                             libtommath1, zlib1g, libgcc-s1 — is staged
#                             so the binaries run inside a bare initramfs)
#   DIR/dropbear/dropbear    + dropbearkey convenience symlinks
#   DIR/TOOLS-PROVENANCE     versions + sha256 record
#
# Default DIR: out/arm64-tools (gitignored).

set -euo pipefail

usage() {
    sed -n '2,19p' "$0"; exit 2
}

die() {
    echo "fetch-arm64-tools: $*" >&2
    exit 1
}

SUITE=trixie
OUTDIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --suite)      SUITE=${2-}; shift 2 ;;
        --output-dir) OUTDIR=${2-}; shift 2 ;;
        -h|--help)    usage ;;
        *) die "unknown option: $1" ;;
    esac
done

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTDIR=${OUTDIR:-$REPO_ROOT/out/arm64-tools}
MIRROR=http://deb.debian.org/debian

# dropbear-bin runtime closure (trixie arm64 Depends, measured 2026-09-19):
# libc6 additionally depends on libgcc-s1.
PKGS=(busybox-static dropbear-bin libc6 libcrypt1 libtomcrypt1 libtommath1 libgmp10 zlib1g libgcc-s1 iw libnl-3-200 libnl-genl-3-200 alsa-utils libasound2t64)

missing=()
command -v curl >/dev/null 2>&1 || missing+=(curl)
command -v zcat >/dev/null 2>&1 || missing+=(gzip)
command -v ar    >/dev/null 2>&1 || missing+=(ar)
command -v tar   >/dev/null 2>&1 || missing+=(tar)
if [ "${#missing[@]}" -gt 0 ]; then
    printf 'fetch-arm64-tools: missing: %s\n' "${missing[*]}" >&2
    exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/piano-tools.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "fetch-arm64-tools: resolving packages for $SUITE/arm64..."
curl -fsSL "$MIRROR/dists/$SUITE/main/binary-arm64/Packages.gz" -o "$WORK/Packages.gz" \
    || die "cannot download Packages.gz for $SUITE"

package_field() { # package_field PKGNAME FIELD
    # NOTE: awk must not exit early — an early exit SIGPIPEs zcat under
    # pipefail and kills the script.
    zcat "$WORK/Packages.gz" \
        | awk -v pkg="$1" -v fld="$2" '
            /^Package: / { cur = $2 }
            cur == pkg && !done && index($0, fld ": ") == 1 {
                sub("^[^:]+: ", ""); print; done = 1
            }
        '
}

fetch_and_extract() { # fetch_and_extract PKGNAME — extracts data.tar into WORK/x/PKGNAME
    local pkg=$1 ver fn
    ver=$(package_field "$pkg" Version) || true
    [ -n "$ver" ] || die "package $pkg not found in $SUITE"
    fn=$(package_field "$pkg" Filename)
    [ -n "$fn" ] || die "package $pkg has no Filename"
    echo "fetch-arm64-tools: $pkg $ver" >&2
    curl -fsSL "$MIRROR/$fn" -o "$WORK/$pkg.deb" || die "download failed: $pkg"
    sha256sum "$WORK/$pkg.deb" >&2
    mkdir -p "$WORK/x/$pkg"
    ( cd "$WORK/x/$pkg" && ar x "$WORK/$pkg.deb" ) || die "ar extract failed: $pkg"
    ( cd "$WORK/x/$pkg" && tar -xf data.tar.* ) || die "tar extract failed: $pkg"
}

for p in "${PKGS[@]}"; do
    fetch_and_extract "$p"
done

# --- stage busybox ----------------------------------------------------------
BB=$WORK/x/busybox-static/bin/busybox
if [ ! -s "$BB" ] || ! head -c4 "$BB" | grep -q .ELF; then
    BB=$(find "$WORK/x/busybox-static" -type f -name 'busybox*' \
        -exec sh -c 'head -c4 "$1" | grep -q .ELF && echo "$1"' _ {} \; | head -1)
fi
[ -s "$BB" ] || die "no ELF busybox found in busybox-static package"
file "$BB" | grep -q 'ELF .*ARM aarch64' || die "busybox at $BB is not an arm64 ELF"
mkdir -p "$OUTDIR/busybox"
install -m 0755 "$BB" "$OUTDIR/busybox/busybox"

# --- stage dropbear tree ----------------------------------------------------
# Debian trixie is merged-usr: libraries live under usr/lib/aarch64-linux-gnu
# and the loader symlink at usr/lib/ld-linux-aarch64.so.1; binaries want
TREE="$OUTDIR/dropbear/tree"
rm -rf "$TREE"
mkdir -p "$TREE/usr"
for f in usr/sbin/dropbear usr/bin/dropbearkey; do
    [ -s "$WORK/x/dropbear-bin/$f" ] || die "dropbear-bin does not contain $f"
done
( cd "$WORK/x/dropbear-bin" && tar -cf - usr/sbin usr/bin ) | ( cd "$TREE" && tar -xf - )
for p in libc6 libcrypt1 libtomcrypt1 libtommath1 libgmp10 zlib1g libgcc-s1; do
    if [ -d "$WORK/x/$p/usr/lib" ]; then
        ( cd "$WORK/x/$p" && tar -cf - usr/lib ) | ( cd "$TREE" && tar -xf - )
    fi
done
file "$TREE/usr/sbin/dropbear" | grep -q 'ARM aarch64' \
    || die "staged dropbear is not arm64"
ln -sfn usr/lib "$TREE/lib"
[ -e "$TREE/lib/ld-linux-aarch64.so.1" ] || die "no arm64 loader reachable at tree /lib"
mkdir -p "$OUTDIR/dropbear"
ln -sf tree/usr/sbin/dropbear    "$OUTDIR/dropbear/dropbear"
ln -sf tree/usr/bin/dropbearkey  "$OUTDIR/dropbear/dropbearkey"

# --- stage aplay tree ---------------------------------------------------------
# aplay (ALSA playback for the audio test tone) is dynamically linked against
# libasound + libc; staged like the iw tree.
APLAY_TREE="$OUTDIR/aplay/tree"
rm -rf "$APLAY_TREE"
mkdir -p "$APLAY_TREE/usr"
[ -s "$WORK/x/alsa-utils/usr/bin/aplay" ] || die "the alsa-utils package does not contain usr/bin/aplay"
( cd "$WORK/x/alsa-utils" && tar -cf - usr/bin ) | ( cd "$APLAY_TREE" && tar -xf - )
for p in libasound2t64 libc6 libgcc-s1; do
    ( cd "$WORK/x/$p" && tar -cf - usr/lib ) | ( cd "$APLAY_TREE" && tar -xf - ) 2>/dev/null \
        || ( cd "$WORK/x/$p" && tar -cf - lib ) | ( cd "$APLAY_TREE" && tar -xf - )
done
file "$APLAY_TREE/usr/bin/aplay" | grep -q 'ARM aarch64' \
    || die "staged aplay is not arm64"
ln -sfn usr/lib "$APLAY_TREE/lib"
[ -e "$APLAY_TREE/lib/ld-linux-aarch64.so.1" ] || die "no arm64 loader reachable at aplay tree /lib"
mkdir -p "$OUTDIR/aplay"
ln -sf tree/usr/bin/aplay "$OUTDIR/aplay/aplay"

# --- stage musl sysroot (for the static pd-locator cross-build) ---------------
# Alpine musl-dev + compiler-rt, downloaded from the official Alpine CDN and
# extracted under the gitignored tools dir. This is compiler material, not
# shipped code. Versions are resolved from the branch APKINDEX at run time:
# the CDN drops superseded apk revisions, so a hardcoded version 404s once
# the branch rolls (musl-dev 1.2.6-r3 on edge, Sept 2026).
ALPINE_MIRROR=https://dl-cdn.alpinelinux.org/alpine

alpine_current_version() { # alpine_current_version BRANCH PKG — version from the branch APKINDEX
    local index="$WORK/APKINDEX-$1"
    if [ ! -s "$index" ]; then
        curl -fsSL "$ALPINE_MIRROR/$1/main/aarch64/APKINDEX.tar.gz" -o "$WORK/apkindex-$1.tgz" \
            || die "cannot download APKINDEX for alpine $1"
        ( cd "$WORK" && tar -xzf "apkindex-$1.tgz" APKINDEX ) \
            || die "cannot unpack APKINDEX for alpine $1"
        mv "$WORK/APKINDEX" "$index"
    fi
    grep -A1 "^P:$2\$" "$index" | sed -n 's/^V://p' | head -1
}
MUSL_VER=""
SYSROOT="$OUTDIR/musl-sysroot"
if [ ! -f "$SYSROOT/usr/lib/libc.a" ] || [ ! -f "$SYSROOT/usr/lib/crt1.o" ]; then
    MUSL_VER=$(alpine_current_version edge musl-dev)
    [ -n "$MUSL_VER" ] || die "no musl-dev in the alpine edge APKINDEX"
    echo "fetch-arm64-tools: fetching musl-dev $MUSL_VER (aarch64) for the sysroot..."
    curl -fsSL "$ALPINE_MIRROR/edge/main/aarch64/musl-dev-$MUSL_VER.apk" \
        -o "$WORK/musl-dev.apk" || die "cannot download musl-dev"
    rm -rf "$SYSROOT"
    mkdir -p "$SYSROOT"
    tar -xzf "$WORK/musl-dev.apk" -C "$SYSROOT" || die "cannot extract musl-dev (not gzip?)"
    [ -f "$SYSROOT/usr/lib/libc.a" ] || die "musl-dev apk lacks usr/lib/libc.a"
    [ -f "$SYSROOT/usr/lib/crt1.o" ] || die "musl-dev apk lacks usr/lib/crt1.o"
fi

# compiler-rt builtins for the same target (musl's stdio uses 128-bit long
# double soft-float helpers that the host clang has no aarch64 runtime for);
# taken from a stable Alpine branch for slower toolchain churn.
CRT_VER=""
CRT_LIB="$SYSROOT/usr/lib/libclang_rt.builtins-aarch64.a"
if [ ! -f "$CRT_LIB" ]; then
    CRT_VER=$(alpine_current_version v3.22 compiler-rt)
    [ -n "$CRT_VER" ] || die "no compiler-rt in the alpine v3.22 APKINDEX"
    echo "fetch-arm64-tools: fetching compiler-rt $CRT_VER (aarch64) for the sysroot..."
    curl -fsSL "$ALPINE_MIRROR/v3.22/main/aarch64/compiler-rt-$CRT_VER.apk" \
        -o "$WORK/compiler-rt.apk" || die "cannot download compiler-rt"
    mkdir -p "$WORK/compiler-rt"
    tar -xzf "$WORK/compiler-rt.apk" -C "$WORK/compiler-rt" 2>/dev/null \
        || die "cannot extract compiler-rt (not gzip?)"
    found=$(find "$WORK/compiler-rt" -name libclang_rt.builtins-aarch64.a | head -1)
    [ -n "$found" ] || die "compiler-rt apk lacks libclang_rt.builtins-aarch64.a"
    install -m 0644 "$found" "$CRT_LIB"
fi

# --- stage iw tree -----------------------------------------------------------
# iw (WLAN nl80211 client for the scan test) is dynamically linked against
# libnl-3/libnl-genl-3 + libc; the same merged-usr staging rules apply.
IW_TREE="$OUTDIR/iw/tree"
rm -rf "$IW_TREE"
mkdir -p "$IW_TREE/usr"
[ -s "$WORK/x/iw/usr/sbin/iw" ] || die "the iw package does not contain usr/sbin/iw"
( cd "$WORK/x/iw" && tar -cf - usr/sbin ) | ( cd "$IW_TREE" && tar -xf - )
for p in libnl-3-200 libnl-genl-3-200 libc6 libgcc-s1; do
    if [ -d "$WORK/x/$p/usr/lib" ]; then
        ( cd "$WORK/x/$p" && tar -cf - usr/lib ) | ( cd "$IW_TREE" && tar -xf - )
    fi
done
file "$IW_TREE/usr/sbin/iw" | grep -q 'ARM aarch64' \
    || die "staged iw is not arm64"
ln -sfn usr/lib "$IW_TREE/lib"
[ -e "$IW_TREE/lib/ld-linux-aarch64.so.1" ] || die "no arm64 loader reachable at iw tree /lib"
mkdir -p "$OUTDIR/iw"
ln -sf tree/usr/sbin/iw "$OUTDIR/iw/iw"
{
    echo "suite: $SUITE"
    for p in "${PKGS[@]}"; do
        printf '%s: %s\n' "$p" "$(package_field "$p" Version)"
    done
    echo "musl-dev: ${MUSL_VER:-cached} (alpine edge)"
    echo "compiler-rt: ${CRT_VER:-cached} (alpine v3.22)"
    file "$OUTDIR/busybox/busybox" "$TREE/usr/sbin/dropbear" "$IW_TREE/usr/sbin/iw"
} | tee "$OUTDIR/TOOLS-PROVENANCE"

echo "fetch-arm64-tools: staged in $OUTDIR"
