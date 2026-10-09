# SPDX-License-Identifier: BSD-3-Clause
dnl AudioReach topology of the Xiaomi Pad 8 Pro (piano, SM8750).
dnl
dnl Built against the linux-msm/audioreach-topology macro library:
dnl   m4 -I topology -I <audioreach-topology> topology/Xiaomi-Pad-8-Pro.m4 > piano.conf
dnl   alsatplg -c piano.conf -o "Xiaomi Pad 8 Pro-tplg.bin"
dnl The kernel loads it as qcom/sm8750/<card model>-tplg.bin.
include(`audioreach/audioreach.m4')
include(`audioreach/stream-subgraph.m4')
include(`audioreach/device-subgraph.m4')
include(`util/route.m4')
include(`util/mixer.m4')
include(`piano/tokens.m4')
#
# Stream SubGraph for MultiMedia1 playback
#  ______________________________________________
# |               Sub Graph 1                    |
# | [WR_SH] -> [PCM DEC] -> [PCM CONV] -> [LOG]  |- Kcontrol
# |______________________________________________|
#
dnl Stereo, or four channels FL FR LS RS for the four speakers
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-vol-playback.m4, FRONTEND_DAI_MULTIMEDIA1,
	`S16_LE', 48000, 48000, 1, 4,
	0x00004001, 0x00004001, 0x00006001, `110000')
dnl Capture MultiMedia3
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-capture.m4, FRONTEND_DAI_MULTIMEDIA3,
	`S16_LE', 48000, 48000, 1, 2,
	0x00004003, 0x00004003, 0x00006020, `110000')
#
# Device SubGraph for the speakers: four FS19xx amplifiers on the
# secondary LPAIF TDM interface, 4 x 32-bit slots at 48 kHz, internal
# long frame sync, data one bit clock after the sync (stock ACDB
# TDM_INTF_CFG of TDM-LPAIF-RX-SECONDARY).
#
#         _________________________________
#        |           Sub Graph 2           |
# Mixer -| [LOG] -> [MFC] -> [TDM SINK EP] |
#        |_________________________________|
#
define(`TDM_SYNC_SRC', `1') dnl internal
define(`TDM_DATA_OUT_ENABLE', `1') dnl
define(`TDM_SLOT_MASK', `0xF') dnl
define(`TDM_NSLOTS', `4') dnl
define(`TDM_SLOT_WIDTH', `32') dnl
define(`TDM_SYNC_MODE', `1') dnl long
define(`TDM_INVERT_SYNC', `0') dnl
define(`TDM_DATA_DELAY', `1') dnl
dnl
dnl DEVICE_SG_ADD(subgraph, name, dai-id, format, min-rate, max-rate,
dnl	min-channels, max-channels, interface-type, interface-index,
dnl	sd-line-idx, data-format, sg-iid-start, cont-iid-start,
dnl	mod-iid-start, mixer-prefix)
DEVICE_SG_ADD(piano/subgraph-device-tdm-playback.m4, `Secondary TDM0', SECONDARY_TDM_RX_0,
	`S32_LE', 48000, 48000, 4, 4,
	LPAIF_INTF_TYPE_LPAIF, 1, 0, DATA_FORMAT_FIXED_POINT,
	0x00004005, 0x00004005, 0x00006050, `SECONDARY_TDM_RX_0')
dnl
dnl VA Capture: the digital microphones
DEVICE_SG_ADD(audioreach/subgraph-device-codec-dma-capture.m4, `VA_CODEC_DMA_TX_0', VA_CODEC_DMA_TX_0,
	`S16_LE', 48000, 48000, 1, 2,
	LPAIF_INTF_TYPE_VA, CODEC_INTF_IDX_TX0, 0, DATA_FORMAT_FIXED_POINT,
	0x00004008, 0x00004008, 0x00006080)
dnl

STREAM_DEVICE_PLAYBACK_MIXER(SECONDARY_TDM_RX_0, ``SECONDARY_TDM_RX_0'', ``MultiMedia1'')
STREAM_DEVICE_PLAYBACK_ROUTE(SECONDARY_TDM_RX_0, ``SECONDARY_TDM_RX_0 Audio Mixer'', ``MultiMedia1, stream0.logger1'')

dnl STREAM_DEVICE_CAPTURE_MIXER(stream-index, kcontro1, kcontrol2... kcontrolN)
STREAM_DEVICE_CAPTURE_MIXER(FRONTEND_DAI_MULTIMEDIA3, ``VA_CODEC_DMA_TX_0'')
dnl STREAM_DEVICE_CAPTURE_ROUTE(stream-index, mixer-name, route1, route2.. routeN)
STREAM_DEVICE_CAPTURE_ROUTE(FRONTEND_DAI_MULTIMEDIA3, ``MultiMedia3 Mixer'', ``VA_CODEC_DMA_TX_0, device110.logger1'')
