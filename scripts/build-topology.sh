#!/usr/bin/env bash
# Build the piano AudioReach topology into a firmware tree.
# Usage: build-topology.sh AUDIOREACH_TOPOLOGY OUTPUT
#   AUDIOREACH_TOPOLOGY  linux-msm/audioreach-topology checkout (m4 macros)
#   OUTPUT               firmware root; gets qcom/sm8750/<model>-tplg.bin
# The kernel requests qcom/<card driver>/<card model>-tplg.bin.
set -euo pipefail
MACROS=$(realpath "${1:?audioreach-topology checkout}")
DEST=${2:?output firmware directory}
SRC=$(cd "$(dirname "$0")/../topology" && pwd)
MODEL='Xiaomi Pad 8 Pro'
for cmd in m4 alsatplg; do command -v "$cmd" >/dev/null; done
[ -f "$MACROS/audioreach/audioreach.m4" ] || { echo "Not an audioreach-topology tree: $MACROS" >&2; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
m4 -I "$SRC" -I "$MACROS" "$SRC/Xiaomi-Pad-8-Pro.m4" > "$TMP/piano.conf"
# Routes to the DAI stream widgets resolve in the kernel, not here.
alsatplg -c "$TMP/piano.conf" -o "$TMP/tplg.bin" 2>&1 | grep -v "undefined \(sink\|source\) widget/stream" >&2 || true
[ -s "$TMP/tplg.bin" ] || { echo 'alsatplg produced no topology' >&2; exit 1; }
install -D -m 0644 "$TMP/tplg.bin" "$DEST/qcom/sm8750/$MODEL-tplg.bin"
