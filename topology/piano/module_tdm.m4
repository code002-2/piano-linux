# SPDX-License-Identifier: BSD-3-Clause
include(`util/util.m4') dnl
define(`MODULE_ID_TDM_SINK', `0x0700100E') dnl
dnl AR_MODULE_TDM_RX(index, sgidx, container-idx, iid, maxip-ports, max-op-ports, src-port, dst-port,
dnl hw-if-type, hw-if-idx, fmt, dev-name, dst-iid)
dnl
dnl The frame layout comes from TDM_SYNC_SRC, TDM_DATA_OUT_ENABLE,
dnl TDM_SLOT_MASK, TDM_NSLOTS, TDM_SLOT_WIDTH, TDM_SYNC_MODE,
dnl TDM_INVERT_SYNC and TDM_DATA_DELAY.  The 16-bit tokens go in the word
dnl tuples too: the kernel only reads the module's one vendor array.
define(`AR_MODULE_TDM_RX',
`'
`SectionVendorTuples."NAME_PREFIX.tdm_rx$1_tuples" {'
`        tokens "audioreach_tokens"'
`'
`        tuples."word.u32_data" {'
`                AR_TKN_U32_MODULE_INSTANCE_ID STR($4)'
`                AR_TKN_U32_MODULE_ID STR(MODULE_ID_TDM_SINK)'
`                AR_TKN_U32_MODULE_MAX_IP_PORTS STR($5)'
`                AR_TKN_U32_MODULE_MAX_OP_PORTS STR($6)'
`                AR_TKN_U32_MODULE_SRC_OP_PORT_ID STR($7)'
`                AR_TKN_U32_MODULE_DST_IN_PORT_ID STR($8)'
`                AR_TKN_U32_MODULE_SRC_INSTANCE_ID STR($4)'
`                AR_TKN_U32_MODULE_DST_INSTANCE_ID STR($13)'
`                AR_TKN_U32_MODULE_HW_IF_TYPE STR($9)'
`                AR_TKN_U32_MODULE_HW_IF_IDX STR($10)'
`                AR_TKN_U32_MODULE_FMT_DATA STR($11)'
`                AR_TKN_U16_MODULE_SYNC_SRC STR(TDM_SYNC_SRC)'
`                AR_TKN_U16_MODULE_CTRL_DATA_OUT_ENABLE STR(TDM_DATA_OUT_ENABLE)'
`                AR_TKN_U32_MODULE_SLOT_MASK STR(TDM_SLOT_MASK)'
`                AR_TKN_U16_MODULE_NSLOTS_PER_FRAME STR(TDM_NSLOTS)'
`                AR_TKN_U16_MODULE_SLOT_WIDTH STR(TDM_SLOT_WIDTH)'
`                AR_TKN_U16_MODULE_SYNC_MODE STR(TDM_SYNC_MODE)'
`                AR_TKN_U16_MODULE_CTRL_INVERT_SYNC_PULSE STR(TDM_INVERT_SYNC)'
`                AR_TKN_U16_MODULE_CTRL_SYNC_DATA_DELAY STR(TDM_DATA_DELAY)'
`        }'
`}'
`'
`SectionData."NAME_PREFIX.tdm_rx$1_data" {'
`        tuples "NAME_PREFIX.tdm_rx$1_tuples"'
`}'
`'
`SectionWidget."NAME_PREFIX.tdm_rx$1" {'
`        index STR($1)'
`        type "aif_in"'
`        no_pm "true"'
`        stream_name "$12 Playback"'
`        subseq "10"'
`        data ['
`                "NAME_PREFIX.sub_graph$2_data"'
`                "NAME_PREFIX.container$3_data"'
`                "NAME_PREFIX.tdm_rx$1_data"'
`        ]'
`}') dnl
