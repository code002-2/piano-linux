#!/usr/bin/env bash
# Local firmware injection shared by RAM-test and Debian image builds.
# Usage: stage-piano-firmware.sh LOCAL_FIRMWARE OUTPUT
set -euo pipefail
SRC=$(realpath "${1:?local firmware directory}")
DEST=${2:?output directory}
[ ! -e "$DEST" ] || { echo 'Firmware output already exists' >&2; exit 1; }
(cd "$SRC/wifi-bt" && sha256sum --check --quiet SHA256SUMS)
mkdir -p "$DEST/novatek" "$DEST/ath12k/PEACH/hw2.0" "$DEST/qca"
for name in novatek_nt36532_piano_fw_csot.bin novatek_nt36532_piano_fw_boe.bin; do
    install -m 0644 "$SRC/odm/firmware/$name" "$DEST/novatek/"
done
cp -a "$SRC/wifi-bt/ath12k/." "$DEST/ath12k/"
cp -a "$SRC/wifi-bt/qca/." "$DEST/qca/"
for pair in peach/amss20.bin:amss.bin peach/phy_ucode20.elf:m3.bin \
    peach/aux_ucode20.elf:aux_ucode.bin peach/regdb_xiaomi.bin:regdb.bin \
    peach/bd_p81.elf:board.bin tmel_peach_20.elf:tmel.bin \
    peach/qdss_trace_config_v2.cfg:qdss_trace_config.bin; do
    install -m 0644 "$SRC/non-hlos/image/${pair%%:*}" "$DEST/ath12k/PEACH/hw2.0/${pair#*:}"
done
for file in "$SRC/btfm/image"/brhbtfw20.tlv "$SRC/btfm/image"/brhbtnv20.* \
    "$SRC/btfm/image"/brhperifw20.tlv "$SRC/btfm/image"/brhperinv20.bin; do
    install -m 0644 "$file" "$DEST/qca/"
done
# ADSP (sensors hub, battery transport, audio): stock names, as the stock
# DT firmware-name expects; pd-mapper reads the PD lists (*.jsn).
install -m 0644 "$SRC/non-hlos/image"/adsp.mdt "$SRC/non-hlos/image"/adsp.b[0-9]* \
    "$SRC/non-hlos/image"/adsp_dtb.* "$SRC/non-hlos/image"/adsp*.jsn "$DEST/"
# Speaker amplifier presets (FS19xx), at the path the piano DT names.
install -D -m 0644 "$SRC/odm/firmware/fs19xx.fsm" "$DEST/qcom/sm8750/xiaomi/piano/fs19xx.fsm"
(cd "$DEST" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum) > "$DEST/SHA256SUMS"
