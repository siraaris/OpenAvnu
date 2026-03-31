#!/usr/bin/env bash
set -euo pipefail

SESS_GPTP="${SESS_GPTP:-gptp_run}"
SESS_MRPD="${SESS_MRPD:-mrpd_run}"
SESS_MRPD_WATCH="${SESS_MRPD_WATCH:-mrpd_watch_run}"
SESS_MAAP="${SESS_MAAP:-maap_run}"
SESS_SHAPER="${SESS_SHAPER:-shaper_run}"
SESS_AVDECC="${SESS_AVDECC:-avdecc_run}"
SESS_HOST="${SESS_HOST:-host_run}"

CFG_DIR="${CFG_DIR:-/root/avb-ini}"
AUDIO_INI_0="${AUDIO_INI_0:-bus32split_milan_0.ini}"
AUDIO_INI_1="${AUDIO_INI_1:-bus32split_milan_1.ini}"
AUDIO_INI_2="${AUDIO_INI_2:-bus32split_milan_2.ini}"
AUDIO_INI_3="${AUDIO_INI_3:-bus32split_milan_3.ini}"
CRF_INI="${CRF_INI:-bus32split_milan_crf.ini}"
CRF_TONEGEN_INI="${CRF_TONEGEN_INI:-crf_talker_milan.ini}"
CRF_LISTENER_INI="${CRF_LISTENER_INI:-crf_listener_milan.ini}"
SINGLE_AUDIO_INI="${SINGLE_AUDIO_INI:-$AUDIO_INI_0}"
TONEGEN_INI_0="${TONEGEN_INI_0:-tonegen_milan_0.ini}"
TONEGEN_INI_1="${TONEGEN_INI_1:-tonegen_milan_1.ini}"
TONEGEN_INI_2="${TONEGEN_INI_2:-tonegen_milan_2.ini}"
TONEGEN_INI_3="${TONEGEN_INI_3:-tonegen_milan_3.ini}"
TONEGEN_MAX_TRANSIT_USEC="${TONEGEN_MAX_TRANSIT_USEC:-2000}"
TONEGEN_MAX_TRANSMIT_DEFICIT_USEC="${TONEGEN_MAX_TRANSMIT_DEFICIT_USEC:-$TONEGEN_MAX_TRANSIT_USEC}"
TONEGEN_TX_MIN_LEAD_USEC="${TONEGEN_TX_MIN_LEAD_USEC:-250}"
TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC="${TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC:-0}"
TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES="${TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES:-0}"
TONEGEN_SELECTED_CLOCK_WARMUP_USEC="${TONEGEN_SELECTED_CLOCK_WARMUP_USEC:-500000}"
TONEGEN_SELECTED_CLOCK_MUTE_USEC="${TONEGEN_SELECTED_CLOCK_MUTE_USEC:-500000}"
BUS32_AAF_SELECTED_CLOCK_TRIM_USEC="${BUS32_AAF_SELECTED_CLOCK_TRIM_USEC:-0}"
CRF_TONEGEN_MAX_TRANSIT_USEC="${CRF_TONEGEN_MAX_TRANSIT_USEC:-2500}"
CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC="${CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC:-2500}"
CRF_TONEGEN_TX_RATE="${CRF_TONEGEN_TX_RATE:-500}"
CRF_TONEGEN_LAUNCH_LEAD_USEC="${CRF_TONEGEN_LAUNCH_LEAD_USEC:-1200}"
STOCK_IGB_AAF_TX_MIN_LEAD_USEC="${STOCK_IGB_AAF_TX_MIN_LEAD_USEC:-2000}"
STOCK_IGB_AAF_MAX_TRANSIT_USEC="${STOCK_IGB_AAF_MAX_TRANSIT_USEC:-500}"
STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC="${STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC:-$STOCK_IGB_AAF_MAX_TRANSIT_USEC}"
STOCK_IGB_CRF_LAUNCH_LEAD_USEC="${STOCK_IGB_CRF_LAUNCH_LEAD_USEC:-2000}"
STOCK_IGB_CLASSA_ETF_DELTA_NS="${STOCK_IGB_CLASSA_ETF_DELTA_NS:-1500000}"
STOCK_IGB_CLASSB_ETF_DELTA_NS="${STOCK_IGB_CLASSB_ETF_DELTA_NS:-1500000}"
STOCK_IGB_MQPRIO_HW="${STOCK_IGB_MQPRIO_HW:-0}"
STOCK_IGB_CLASSA_CBS_OFFLOAD="${STOCK_IGB_CLASSA_CBS_OFFLOAD:-1}"
STOCK_IGB_CLASSA_ETF_OFFLOAD="${STOCK_IGB_CLASSA_ETF_OFFLOAD:-0}"
STOCK_IGB_CLASSB_CBS_OFFLOAD="${STOCK_IGB_CLASSB_CBS_OFFLOAD:-0}"
STOCK_IGB_CLASSB_ETF_OFFLOAD="${STOCK_IGB_CLASSB_ETF_OFFLOAD:-0}"
STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC="${STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC:-12}"
BUS32_CRF_DIAG_ENABLE="${BUS32_CRF_DIAG_ENABLE:-1}"
BUS32_CRF_DIAG_LOG_EVERY_PACKETS="${BUS32_CRF_DIAG_LOG_EVERY_PACKETS:-500}"
BUS32_CRF_DIAG_JITTER_THRESH_NS="${BUS32_CRF_DIAG_JITTER_THRESH_NS:-250000}"
NULL_TALKER_INI="${NULL_TALKER_INI:-null_talker.ini}"
WAV_LISTENER_INI="${WAV_LISTENER_INI:-/root/src/OpenAvnu/test_configs/aaf_wav_listener_8ch.ini}"
WAV_CAPTURE_FILE="${WAV_CAPTURE_FILE:-/tmp/openavnu_rme_capture_8ch.wav}"

DEFAULT_INI_FILES="${DEFAULT_INI_FILES:-$AUDIO_INI_0 $AUDIO_INI_1 $AUDIO_INI_2 $AUDIO_INI_3 $CRF_INI}"

TONEGEN_DEFAULT_INI_FILES="${TONEGEN_DEFAULT_INI_FILES:-$TONEGEN_INI_0,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_1,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_2,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_3,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $CRF_TONEGEN_INI,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC $CRF_LISTENER_INI}"
WAV_TONEGEN_DEFAULT_INI_FILES="${WAV_TONEGEN_DEFAULT_INI_FILES:-$TONEGEN_INI_0,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_1,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_2,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_3,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $CRF_TONEGEN_INI,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC $CRF_LISTENER_INI $WAV_LISTENER_INI,intf_nv_file_name_rx=$WAV_CAPTURE_FILE}"
NULL_CRF_DEFAULT_INI_FILES="${NULL_CRF_DEFAULT_INI_FILES:-$NULL_TALKER_INI $CRF_TONEGEN_INI}"
GENERIC_TSN_DEFAULT_INI_FILES="${GENERIC_TSN_DEFAULT_INI_FILES:-$TONEGEN_INI_0 $TONEGEN_INI_1 $TONEGEN_INI_2 $TONEGEN_INI_3 $CRF_TONEGEN_INI}"
GENERIC_TSN_IFACE_HOST="${GENERIC_TSN_IFACE_HOST:-sendmmsg:enp2s0}"
STOCK_IGB_IFACE_HOST="${STOCK_IGB_IFACE_HOST:-sendmmsg:enp2s0}"
STOCK_IGB_IFACE_AVDECC="${STOCK_IGB_IFACE_AVDECC:-enp2s0}"
STOCK_IGB_IFACE_DAEMONS="${STOCK_IGB_IFACE_DAEMONS:-enp2s0}"
GENERIC_TSN_SHAPER_LINK_SPEED_MBPS="${GENERIC_TSN_SHAPER_LINK_SPEED_MBPS:-1000}"
GENERIC_TSN_SHAPER_CLASSA_PARENT="${GENERIC_TSN_SHAPER_CLASSA_PARENT:-1:2}"
GENERIC_TSN_SHAPER_CLASSB_PARENT="${GENERIC_TSN_SHAPER_CLASSB_PARENT:-1:3}"
GENERIC_TSN_SHAPER_CLASSA_HANDLE="${GENERIC_TSN_SHAPER_CLASSA_HANDLE:-2}"
GENERIC_TSN_SHAPER_CLASSB_HANDLE="${GENERIC_TSN_SHAPER_CLASSB_HANDLE:-3}"
GENERIC_TSN_SHAPER_EGRESS_QMAP="${GENERIC_TSN_SHAPER_EGRESS_QMAP:-1}"
GENERIC_TSN_SHAPER_CLASSA_QMAP="${GENERIC_TSN_SHAPER_CLASSA_QMAP:-1}"
GENERIC_TSN_SHAPER_CLASSB_QMAP="${GENERIC_TSN_SHAPER_CLASSB_QMAP:-2}"
GENERIC_TSN_SHAPER_DEFAULT_QMAP="${GENERIC_TSN_SHAPER_DEFAULT_QMAP:-3}"
GENERIC_TSN_SHAPER_GPTP_QMAP="${GENERIC_TSN_SHAPER_GPTP_QMAP:-0}"
# Default generic-tsn queue split:
# - prio 7/6 (gPTP/network-control) -> tc0 -> classid 1:1 (queue 0, highest)
# - prio 3 (Class A) -> tc1 -> classid 1:2 (queue 1)
# - prio 2 (Class B) -> tc2 -> classid 1:3 (queue 2)
# - prio 0/1 and remaining priorities -> tc3 -> classid 1:4 (queue 3, default)
# gPTP is additionally steered by shaper egress filters (ethertype 0x88f7).
STOCK_IGB_SHAPER_TC_MAP="${STOCK_IGB_SHAPER_TC_MAP:-3 3 2 1 3 3 0 0 3 3 3 3 3 3 3 3}"
STOCK_IGB_MQPRIO_CLASSA_PARENT="${STOCK_IGB_MQPRIO_CLASSA_PARENT:-1:2}"
STOCK_IGB_MQPRIO_CLASSB_PARENT="${STOCK_IGB_MQPRIO_CLASSB_PARENT:-1:3}"
STOCK_IGB_MQPRIO_CLASSA_HANDLE="${STOCK_IGB_MQPRIO_CLASSA_HANDLE:-2}"
STOCK_IGB_MQPRIO_CLASSB_HANDLE="${STOCK_IGB_MQPRIO_CLASSB_HANDLE:-3}"
INI_FILES="${INI_FILES:-$DEFAULT_INI_FILES}"
STACK_PROFILE="${STACK_PROFILE:-bus32split-sendmmsg-stockigb}"

AVDECC_BIN="${AVDECC_BIN:-/root/src/OpenAvnu/lib/avtp_pipeline/build_avdecc/platform/Linux/avb_avdecc/openavb_avdecc}"
HOST_BIN="${HOST_BIN:-/root/src/OpenAvnu/lib/avtp_pipeline/build/bin/openavb_host}"
GPTP_BIN="${GPTP_BIN:-/root/src/gptp/linux/build/obj/daemon_cl}"
MRPD_BIN="${MRPD_BIN:-/root/src/OpenAvnu/daemons/mrpd/mrpd}"
MAAP_BIN="${MAAP_BIN:-/root/src/OpenAvnu/daemons/maap/linux/build/maap_daemon}"
SHAPER_BIN="${SHAPER_BIN:-/root/src/OpenAvnu/daemons/shaper/shaper_daemon}"
MRPD_MAKE_DIR="${MRPD_MAKE_DIR:-/root/src/OpenAvnu/daemons/mrpd}"

IFACE_AVDECC="${IFACE_AVDECC:-enp2s0}"
IFACE_HOST="${IFACE_HOST:-igb:enp2s0}"
IFACE_DAEMONS="${IFACE_DAEMONS:-enp2s0}"

ENDPOINT_INI="${ENDPOINT_INI:-/root/avb-ini/endpoint.ini}"
ENDPOINT_SAVE_INI="${ENDPOINT_SAVE_INI:-/root/avb-ini/endpoint_save.ini}"
AVDECC_INI="${AVDECC_INI:-/root/avb-ini/avdecc.ini}"

CRF_MODEL_INI="${CRF_MODEL_INI:-$CRF_TONEGEN_INI}"
CRF_MODEL_ENDPOINT_INI="${CRF_MODEL_ENDPOINT_INI:-/root/avb-ini/endpoint_crf_only.ini}"
CRF_MODEL_ENDPOINT_SAVE_INI="${CRF_MODEL_ENDPOINT_SAVE_INI:-/root/avb-ini/endpoint_crf_only_save.ini}"
CRF_MODEL_AVDECC_INI="${CRF_MODEL_AVDECC_INI:-/root/avb-ini/avdecc_crf_only.ini}"

ACTIVE_ENDPOINT_INI="$ENDPOINT_INI"
ACTIVE_ENDPOINT_SAVE_INI="$ENDPOINT_SAVE_INI"
ACTIVE_AVDECC_INI="$AVDECC_INI"

RUN_GPTP="${RUN_GPTP:-1}"
RUN_MRPD="${RUN_MRPD:-1}"
RUN_MRPD_WATCH="${RUN_MRPD_WATCH:-1}"
RUN_MAAP="${RUN_MAAP:-1}"
RUN_SHAPER="${RUN_SHAPER:-1}"

GPTP_CWD="${GPTP_CWD:-/root/src/gptp/linux/build/obj}"
GPTP_ARGS="${GPTP_ARGS:--F gptp_cfg.ini}"
MRPD_ARGS="${MRPD_ARGS:-}"
MAAP_ARGS="${MAAP_ARGS:-}"
SHAPER_ARGS="${SHAPER_ARGS:-}"
SHAPER_TC_LOG="${SHAPER_TC_LOG:-1}"
SHAPER_SKIP_ROOT_QDISC="${SHAPER_SKIP_ROOT_QDISC:-}"
SHAPER_LINK_SPEED_MBPS="${SHAPER_LINK_SPEED_MBPS:-}"
SHAPER_CLASSA_PARENT="${SHAPER_CLASSA_PARENT:-}"
SHAPER_CLASSB_PARENT="${SHAPER_CLASSB_PARENT:-}"
SHAPER_CLASSA_HANDLE="${SHAPER_CLASSA_HANDLE:-}"
SHAPER_CLASSB_HANDLE="${SHAPER_CLASSB_HANDLE:-}"
SHAPER_EGRESS_QMAP="${SHAPER_EGRESS_QMAP:-}"
SHAPER_CLASSA_QMAP="${SHAPER_CLASSA_QMAP:-}"
SHAPER_CLASSB_QMAP="${SHAPER_CLASSB_QMAP:-}"
SHAPER_DEFAULT_QMAP="${SHAPER_DEFAULT_QMAP:-}"
SHAPER_GPTP_QMAP="${SHAPER_GPTP_QMAP:-}"
SHAPER_CLASSA_QDISC="${SHAPER_CLASSA_QDISC:-}"
SHAPER_CLASSA_CBS_OFFLOAD="${SHAPER_CLASSA_CBS_OFFLOAD:-}"
SHAPER_CLASSA_ETF_DELTA_NS="${SHAPER_CLASSA_ETF_DELTA_NS:-}"
SHAPER_CLASSA_ETF_OFFLOAD="${SHAPER_CLASSA_ETF_OFFLOAD:-}"
SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK="${SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK:-}"
SHAPER_CLASSB_QDISC="${SHAPER_CLASSB_QDISC:-}"
SHAPER_CLASSB_CBS_OFFLOAD="${SHAPER_CLASSB_CBS_OFFLOAD:-}"
SHAPER_CLASSB_ETF_DELTA_NS="${SHAPER_CLASSB_ETF_DELTA_NS:-}"
SHAPER_CLASSB_ETF_OFFLOAD="${SHAPER_CLASSB_ETF_OFFLOAD:-}"
SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK="${SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK:-}"
SHAPER_MQPRIO_HW="${SHAPER_MQPRIO_HW:-}"
SHAPER_TC_MAP="${SHAPER_TC_MAP:-}"
OPENAVB_DISABLE_SO_TXTIME="${OPENAVB_DISABLE_SO_TXTIME:-}"
MRPD_LOG_FLAGS="${MRPD_LOG_FLAGS:-lmvs}"
MRPD_PROFILE="${MRPD_PROFILE:-keep}"
MRPD_DEFAULT_CFLAGS="${MRPD_DEFAULT_CFLAGS:--O2 -Wall -Wextra -Wno-parentheses -ggdb -D_GNU_SOURCE}"
MRPD_DEBUG_CFLAGS="${MRPD_DEBUG_CFLAGS:--O2 -Wall -Wextra -Wno-parentheses -ggdb -D_GNU_SOURCE -DLOG_ERRORS=1 -DLOG_MSRP=1 -DLOG_MVRP=1}"
MRPD_CFLAGS="${MRPD_CFLAGS:-}"
GPTP_READY_WAIT_SEC="${GPTP_READY_WAIT_SEC:-8}"
SRP_SETTLE_SEC="${SRP_SETTLE_SEC:-7}"
HOST_SRP_CHECK_SEC="${HOST_SRP_CHECK_SEC:-3}"
HOST_SRP_RETRIES="${HOST_SRP_RETRIES:-3}"
HOST_SRP_RETRY_DELAY_SEC="${HOST_SRP_RETRY_DELAY_SEC:-3}"
HOST_ENDPOINT_READY_SEC="${HOST_ENDPOINT_READY_SEC:-20}"
MAAP_READY_WAIT_SEC="${MAAP_READY_WAIT_SEC:-20}"
IPC_CLEAN_RETRIES="${IPC_CLEAN_RETRIES:-20}"
IPC_CLEAN_DELAY_SEC="${IPC_CLEAN_DELAY_SEC:-0.1}"
IPC_ENDPOINT_SOCK="${IPC_ENDPOINT_SOCK:-/tmp/avb_endpoint}"
IPC_AVDECC_SOCK="${IPC_AVDECC_SOCK:-/tmp/avdecc_msg}"
STREAM_READY_WAIT_SEC="${STREAM_READY_WAIT_SEC:-20}"

LOG_DIR="${LOG_DIR:-/root/avb-logs}"
GPTP_LOG="${GPTP_LOG:-$LOG_DIR/daemon_cl_current.log}"
MRPD_LOG="${MRPD_LOG:-$LOG_DIR/mrpd_current.log}"
MRPD_TALKER_FAILED_LOG="${MRPD_TALKER_FAILED_LOG:-$LOG_DIR/mrpd_talker_failed_current.log}"
MAAP_LOG="${MAAP_LOG:-$LOG_DIR/maap_current.log}"
SHAPER_LOG="${SHAPER_LOG:-$LOG_DIR/shaper_current.log}"
AVDECC_LOG="${AVDECC_LOG:-$LOG_DIR/openavb_avdecc_current.log}"
HOST_LOG="${HOST_LOG:-$LOG_DIR/openavb_host_current.log}"

INI_LIST_ARR=()
INI_ARGS=""
EXPECTED_UIDS=()
EXPECTED_MAAP_UIDS=()

usage() {
    cat <<'EOF'
Usage:
  run_avb_stack_tmux.sh start|stop|restart|status|logs

Defaults (override via env vars):
  IFACE_DAEMONS=enp2s0
  RUN_GPTP=1 RUN_MRPD=1 RUN_MAAP=1 RUN_SHAPER=1
  RUN_MRPD_WATCH=1
  GPTP_READY_WAIT_SEC=8
  SRP_SETTLE_SEC=7
  HOST_ENDPOINT_READY_SEC=20
  MAAP_READY_WAIT_SEC=20
  HOST_SRP_CHECK_SEC=3 HOST_SRP_RETRIES=3 HOST_SRP_RETRY_DELAY_SEC=3
  STREAM_READY_WAIT_SEC=20
  IPC_CLEAN_RETRIES=20 IPC_CLEAN_DELAY_SEC=0.1
  IPC_ENDPOINT_SOCK=/tmp/avb_endpoint
  IPC_AVDECC_SOCK=/tmp/avdecc_msg
  GPTP_CWD=/root/src/gptp/linux/build/obj
  GPTP_ARGS="-F gptp_cfg.ini"
  MRPD_LOG_FLAGS=lmvs
  MRPD_PROFILE=keep|default|debug|custom
  MRPD_MAKE_DIR=/root/src/OpenAvnu/daemons/mrpd
  MRPD_DEFAULT_CFLAGS="-O2 -Wall -Wextra -Wno-parentheses -ggdb -D_GNU_SOURCE"
  MRPD_DEBUG_CFLAGS="-O2 -Wall -Wextra -Wno-parentheses -ggdb -D_GNU_SOURCE -DLOG_ERRORS=1 -DLOG_MSRP=1 -DLOG_MVRP=1"
  MRPD_CFLAGS="<required when MRPD_PROFILE=custom>"
  CFG_DIR=/root/avb-ini
  AUDIO_INI_0=bus32split_milan_0.ini
  AUDIO_INI_1=bus32split_milan_1.ini
  AUDIO_INI_2=bus32split_milan_2.ini
  AUDIO_INI_3=bus32split_milan_3.ini
  CRF_INI=bus32split_milan_crf.ini
  SINGLE_AUDIO_INI=bus32split_milan_0.ini
  TONEGEN_INI_0=tonegen_milan_0.ini
  TONEGEN_INI_1=tonegen_milan_1.ini
  TONEGEN_INI_2=tonegen_milan_2.ini
  TONEGEN_INI_3=tonegen_milan_3.ini
  TONEGEN_MAX_TRANSIT_USEC=4000
  TONEGEN_MAX_TRANSMIT_DEFICIT_USEC=4000
  TONEGEN_TX_MIN_LEAD_USEC=4000
  TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC=4000
  TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES=0
  CRF_TONEGEN_MAX_TRANSIT_USEC=2500
  CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC=2500
  CRF_TONEGEN_TX_RATE=500
  CRF_TONEGEN_LAUNCH_LEAD_USEC=1200
  NULL_TALKER_INI=null_talker.ini
  WAV_LISTENER_INI=/root/src/OpenAvnu/test_configs/aaf_wav_listener_8ch.ini
  WAV_CAPTURE_FILE=/tmp/openavnu_rme_capture_8ch.wav
  INI_FILES="bus32split_milan_0.ini bus32split_milan_1.ini bus32split_milan_2.ini bus32split_milan_3.ini bus32split_milan_crf.ini"
  TONEGEN_DEFAULT_INI_FILES="tonegen_milan_0.ini tonegen_milan_1.ini tonegen_milan_2.ini tonegen_milan_3.ini crf_talker_milan.ini crf_listener_milan.ini"
  WAV_TONEGEN_DEFAULT_INI_FILES="tonegen_milan_0.ini tonegen_milan_1.ini tonegen_milan_2.ini tonegen_milan_3.ini crf_talker_milan.ini crf_listener_milan.ini /root/src/OpenAvnu/test_configs/aaf_wav_listener_8ch.ini,intf_nv_file_name_rx=/tmp/openavnu_rme_capture_8ch.wav"
  NULL_CRF_DEFAULT_INI_FILES="null_talker.ini crf_talker_milan.ini"
  CRF_TONEGEN_INI=crf_talker_milan.ini
  CRF_LISTENER_INI=crf_listener_milan.ini
  CRF_MODEL_INI=crf_talker_milan.ini
  CRF_MODEL_ENDPOINT_INI=/root/avb-ini/endpoint_crf_only.ini
  CRF_MODEL_ENDPOINT_SAVE_INI=/root/avb-ini/endpoint_crf_only_save.ini
  CRF_MODEL_AVDECC_INI=/root/avb-ini/avdecc_crf_only.ini
  STACK_PROFILE=full|single|full-sendmmsg-stockigb|bus32split-sendmmsg-stockigb|tonegen4|toneaudio|toneaudio-sendmmsg|toneaudio-sendmmsg-stockigb|generic-tsn|nullcrf|crf|crfmodel|crf-listener|custom
    full   -> uses DEFAULT_INI_FILES (audio + CRF)
    single -> uses SINGLE_AUDIO_INI + CRF_INI
    tonegen4 -> uses TONEGEN_DEFAULT_INI_FILES (4x tonegen + CRF talker + CRF listener)
    toneaudio -> alias for tonegen4
    toneaudio-sendmmsg -> tonegen4/toneaudio streams on sendmmsg rawsock with generic-tsn shaper defaults
    toneaudio-sendmmsg-stockigb -> tonegen4/toneaudio streams on sendmmsg rawsock with explicit stock-igb host/AVDECC/daemon interfaces and stock-igb shaper defaults
    full-sendmmsg-stockigb|bus32split-sendmmsg-stockigb -> bus32split streams on sendmmsg rawsock with explicit stock-igb host/AVDECC/daemon interfaces and stock-igb shaper defaults
    wav     -> uses WAV_TONEGEN_DEFAULT_INI_FILES (4x tonegen + CRF talker/listener + AAF WAV listener)
    generic-tsn -> tonegen + CRF talker using sendmmsg rawsock and shaper-managed MQPRIO/CBS/ETF defaults
    nullcrf -> uses NULL_CRF_DEFAULT_INI_FILES (null talker + CRF talker)
    crf    -> uses only CRF_INI
    crfmodel -> uses CRF_MODEL_INI and switches to CRF-only endpoint/AVDECC config
    crf-listener -> uses only CRF_LISTENER_INI
    custom -> uses INI_FILES exactly as provided
  Note: CRF INI entries are automatically moved to the end of INI_FILES so
  clock streams enumerate after audio streams in controllers.
  INI entries support inline overrides, e.g.:
    INI_FILES="crf_talker_milan.ini,dest_addr=91:e0:f0:00:fe:81"
  ENDPOINT_INI=/root/avb-ini/endpoint.ini
  ENDPOINT_SAVE_INI=/root/avb-ini/endpoint_save.ini
  AVDECC_INI=/root/avb-ini/avdecc.ini
  IFACE_AVDECC=enp2s0
  IFACE_HOST=igb:enp2s0
  LOG_DIR=/root/avb-logs
  GPTP_LOG=/root/avb-logs/daemon_cl_current.log
  MRPD_LOG=/root/avb-logs/mrpd_current.log
  MRPD_TALKER_FAILED_LOG=/root/avb-logs/mrpd_talker_failed_current.log
  MAAP_LOG=/root/avb-logs/maap_current.log
  SHAPER_LOG=/root/avb-logs/shaper_current.log
  AVDECC_LOG=/root/avb-logs/openavb_avdecc_current.log
  HOST_LOG=/root/avb-logs/openavb_host_current.log
  SESS_MRPD_WATCH=mrpd_watch_run
EOF
}

apply_generic_tsn_shaper_defaults() {
    if [[ -z "$SHAPER_LINK_SPEED_MBPS" ]]; then
        SHAPER_LINK_SPEED_MBPS="$GENERIC_TSN_SHAPER_LINK_SPEED_MBPS"
    fi
    if [[ -z "$SHAPER_CLASSA_PARENT" ]]; then
        SHAPER_CLASSA_PARENT="$GENERIC_TSN_SHAPER_CLASSA_PARENT"
    fi
    if [[ -z "$SHAPER_CLASSB_PARENT" ]]; then
        SHAPER_CLASSB_PARENT="$GENERIC_TSN_SHAPER_CLASSB_PARENT"
    fi
    if [[ -z "$SHAPER_CLASSA_HANDLE" ]]; then
        SHAPER_CLASSA_HANDLE="$GENERIC_TSN_SHAPER_CLASSA_HANDLE"
    fi
    if [[ -z "$SHAPER_CLASSB_HANDLE" ]]; then
        SHAPER_CLASSB_HANDLE="$GENERIC_TSN_SHAPER_CLASSB_HANDLE"
    fi
    if [[ -z "$SHAPER_EGRESS_QMAP" ]]; then
        SHAPER_EGRESS_QMAP="$GENERIC_TSN_SHAPER_EGRESS_QMAP"
    fi
    if [[ -z "$SHAPER_CLASSA_QMAP" ]]; then
        SHAPER_CLASSA_QMAP="$GENERIC_TSN_SHAPER_CLASSA_QMAP"
    fi
    if [[ -z "$SHAPER_CLASSB_QMAP" ]]; then
        SHAPER_CLASSB_QMAP="$GENERIC_TSN_SHAPER_CLASSB_QMAP"
    fi
    if [[ -z "$SHAPER_DEFAULT_QMAP" ]]; then
        SHAPER_DEFAULT_QMAP="$GENERIC_TSN_SHAPER_DEFAULT_QMAP"
    fi
    if [[ -z "$SHAPER_GPTP_QMAP" ]]; then
        SHAPER_GPTP_QMAP="$GENERIC_TSN_SHAPER_GPTP_QMAP"
    fi
}

apply_stock_igb_shaper_defaults() {
    apply_generic_tsn_shaper_defaults

    SHAPER_MQPRIO_HW="$STOCK_IGB_MQPRIO_HW"
    SHAPER_TC_MAP="$STOCK_IGB_SHAPER_TC_MAP"
    SHAPER_CLASSA_PARENT="$STOCK_IGB_MQPRIO_CLASSA_PARENT"
    SHAPER_CLASSB_PARENT="$STOCK_IGB_MQPRIO_CLASSB_PARENT"
    SHAPER_CLASSA_HANDLE="$STOCK_IGB_MQPRIO_CLASSA_HANDLE"
    SHAPER_CLASSB_HANDLE="$STOCK_IGB_MQPRIO_CLASSB_HANDLE"

    SHAPER_CLASSA_QMAP="-1"
    SHAPER_CLASSB_QMAP="-1"
    SHAPER_DEFAULT_QMAP="-1"
    [[ -z "$SHAPER_CLASSA_QDISC" ]] && SHAPER_CLASSA_QDISC="cbs_etf"
    [[ -z "$SHAPER_CLASSA_CBS_OFFLOAD" ]] && SHAPER_CLASSA_CBS_OFFLOAD="$STOCK_IGB_CLASSA_CBS_OFFLOAD"
    [[ -z "$SHAPER_CLASSA_ETF_DELTA_NS" ]] && SHAPER_CLASSA_ETF_DELTA_NS="$STOCK_IGB_CLASSA_ETF_DELTA_NS"
    [[ -z "$SHAPER_CLASSA_ETF_OFFLOAD" ]] && SHAPER_CLASSA_ETF_OFFLOAD="$STOCK_IGB_CLASSA_ETF_OFFLOAD"
    [[ -z "$SHAPER_CLASSB_QDISC" ]] && SHAPER_CLASSB_QDISC="cbs_etf"
    [[ -z "$SHAPER_CLASSB_CBS_OFFLOAD" ]] && SHAPER_CLASSB_CBS_OFFLOAD="$STOCK_IGB_CLASSB_CBS_OFFLOAD"
    [[ -z "$SHAPER_CLASSB_ETF_DELTA_NS" ]] && SHAPER_CLASSB_ETF_DELTA_NS="$STOCK_IGB_CLASSB_ETF_DELTA_NS"
    [[ -z "$SHAPER_CLASSB_ETF_OFFLOAD" ]] && SHAPER_CLASSB_ETF_OFFLOAD="$STOCK_IGB_CLASSB_ETF_OFFLOAD"
}

apply_stack_profile() {
    local profile="${STACK_PROFILE,,}"
    ACTIVE_ENDPOINT_INI="$ENDPOINT_INI"
    ACTIVE_ENDPOINT_SAVE_INI="$ENDPOINT_SAVE_INI"
    ACTIVE_AVDECC_INI="$AVDECC_INI"

    case "$profile" in
        ""|full|all)
            INI_FILES="$DEFAULT_INI_FILES"
            ;;
        single|one|audio1|one-audio|one_audio)
            INI_FILES="$SINGLE_AUDIO_INI $CRF_INI"
            ;;
        tonegen4|toneaudio|toneaudio4|toneaudio-crf|toneaudio_crf|tonegen|tone4|tone)
            INI_FILES="$TONEGEN_DEFAULT_INI_FILES"
            ;;
        toneaudio-sendmmsg|toneaudio_sendmmsg|tonegen4-sendmmsg|tonegen4_sendmmsg|tone-sendmmsg|tone_sendmmsg)
            INI_FILES="$TONEGEN_DEFAULT_INI_FILES"
            if [[ "$IFACE_HOST" == "igb:enp2s0" ]]; then
                IFACE_HOST="$GENERIC_TSN_IFACE_HOST"
            fi
            apply_generic_tsn_shaper_defaults
            ;;
        toneaudio-sendmmsg-stockigb|toneaudio_sendmmsg_stockigb|tonegen4-sendmmsg-stockigb|tonegen4_sendmmsg_stockigb|tone-sendmmsg-stockigb|tone_sendmmsg_stockigb)
            INI_FILES="$TONEGEN_DEFAULT_INI_FILES"
            [[ "$TONEGEN_TX_MIN_LEAD_USEC" == "250" ]] && TONEGEN_TX_MIN_LEAD_USEC="$STOCK_IGB_AAF_TX_MIN_LEAD_USEC"
            [[ "$CRF_TONEGEN_LAUNCH_LEAD_USEC" == "1200" ]] && CRF_TONEGEN_LAUNCH_LEAD_USEC="$STOCK_IGB_CRF_LAUNCH_LEAD_USEC"
            INI_FILES="$TONEGEN_DEFAULT_INI_FILES"
            IFACE_HOST="$STOCK_IGB_IFACE_HOST"
            IFACE_AVDECC="$STOCK_IGB_IFACE_AVDECC"
            IFACE_DAEMONS="$STOCK_IGB_IFACE_DAEMONS"
            if [[ "$GPTP_ARGS" == "-F gptp_cfg.ini" ]]; then
                GPTP_ARGS="-F /root/src/gptp/gptp_cfg.ini"
            fi
            apply_stock_igb_shaper_defaults
            ;;
        full-sendmmsg-stockigb|full_sendmmsg_stockigb|bus32split-sendmmsg-stockigb|bus32split_sendmmsg_stockigb|bus32-sendmmsg-stockigb|bus32_sendmmsg_stockigb)
            [[ "$CRF_TONEGEN_LAUNCH_LEAD_USEC" == "1200" ]] && CRF_TONEGEN_LAUNCH_LEAD_USEC="$STOCK_IGB_CRF_LAUNCH_LEAD_USEC"
            INI_FILES="$AUDIO_INI_0,ifname=$STOCK_IGB_IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=500000,map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=0,map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$AUDIO_INI_1,ifname=$STOCK_IGB_IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=500000,map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=$STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC,map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$AUDIO_INI_2,ifname=$STOCK_IGB_IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=500000,map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=$((STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC * 2)),map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$AUDIO_INI_3,ifname=$STOCK_IGB_IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=500000,map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=$((STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC * 3)),map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$CRF_INI,ifname=$STOCK_IGB_IFACE_HOST,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC,map_nv_crf_diag_enable=$BUS32_CRF_DIAG_ENABLE,map_nv_crf_diag_log_every_packets=$BUS32_CRF_DIAG_LOG_EVERY_PACKETS,map_nv_crf_diag_jitter_thresh_ns=$BUS32_CRF_DIAG_JITTER_THRESH_NS \
$CRF_LISTENER_INI,ifname=$STOCK_IGB_IFACE_HOST"
            IFACE_HOST="$STOCK_IGB_IFACE_HOST"
            IFACE_AVDECC="$STOCK_IGB_IFACE_AVDECC"
            IFACE_DAEMONS="$STOCK_IGB_IFACE_DAEMONS"
            if [[ "$GPTP_ARGS" == "-F gptp_cfg.ini" ]]; then
                GPTP_ARGS="-F /root/src/gptp/gptp_cfg.ini"
            fi
            apply_stock_igb_shaper_defaults
            ;;
        wav|tonegen4wav|tonegen-wav|tonegen_wav|wav-listener|wav_listener)
            INI_FILES="$WAV_TONEGEN_DEFAULT_INI_FILES"
            ;;
        generic-tsn|generic_tsn|tsn|linux-tsn|linux_tsn|sendmmsg)
            INI_FILES="$GENERIC_TSN_DEFAULT_INI_FILES"
            if [[ "$IFACE_HOST" == "igb:enp2s0" ]]; then
                IFACE_HOST="$GENERIC_TSN_IFACE_HOST"
            fi
            apply_generic_tsn_shaper_defaults
            ;;
        nullcrf|null-crf|null_crf|null+crf)
            INI_FILES="$NULL_CRF_DEFAULT_INI_FILES"
            ;;
        crf|crf-only|crf_only)
            INI_FILES="$CRF_INI"
            ;;
        crfmodel|crf-model|crf_model|clock-only|clock_only)
            INI_FILES="$CRF_MODEL_INI"
            ACTIVE_ENDPOINT_INI="$CRF_MODEL_ENDPOINT_INI"
            ACTIVE_ENDPOINT_SAVE_INI="$CRF_MODEL_ENDPOINT_SAVE_INI"
            ACTIVE_AVDECC_INI="$CRF_MODEL_AVDECC_INI"
            ;;
        crf-listener|crf_listener|listener-crf|listener_crf)
            INI_FILES="$CRF_LISTENER_INI"
            ;;
        custom)
            : # respect INI_FILES exactly as supplied
            ;;
        *)
            echo "ERROR: Unsupported STACK_PROFILE='$STACK_PROFILE'. Use full|single|full-sendmmsg-stockigb|bus32split-sendmmsg-stockigb|tonegen4|toneaudio|toneaudio-sendmmsg|toneaudio-sendmmsg-stockigb|wav|generic-tsn|nullcrf|crf|crfmodel|crf-listener|custom." >&2
            exit 1
            ;;
    esac
}

reorder_crf_inis_last() {
    local ini_entry ini_path map_fn
    local -a ordered_non_crf=()
    local -a ordered_crf=()

    read -r -a INI_LIST_ARR <<< "$INI_FILES"
    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        if [[ -f "$ini_path" ]]; then
            map_fn="$(extract_ini_key "$ini_path" "map_fn" || true)"
        else
            map_fn=""
        fi
        if [[ "$map_fn" == "openavbMapCrfInitialize" ]]; then
            ordered_crf+=("$ini_entry")
        else
            ordered_non_crf+=("$ini_entry")
        fi
    done

    INI_FILES=""
    if [[ ${#ordered_non_crf[@]} -gt 0 ]]; then
        INI_FILES="${ordered_non_crf[*]}"
    fi
    if [[ ${#ordered_crf[@]} -gt 0 ]]; then
        if [[ -n "$INI_FILES" ]]; then
            INI_FILES+=" "
        fi
        INI_FILES+="${ordered_crf[*]}"
    fi
}

resolve_ini_path() {
    local ini="$1"
    if [[ "$ini" = /* ]]; then
        printf '%s\n' "$ini"
    else
        printf '%s\n' "$CFG_DIR/$ini"
    fi
}

resolve_ini_entry() {
    local ini_entry="$1"
    local ini_base="${ini_entry%%,*}"
    local ini_suffix="${ini_entry#"$ini_base"}"
    local ini_path
    ini_path="$(resolve_ini_path "$ini_base")"
    printf '%s%s\n' "$ini_path" "$ini_suffix"
}

build_ini_args() {
    local ini
    read -r -a INI_LIST_ARR <<< "$INI_FILES"
    INI_ARGS=""
    for ini in "${INI_LIST_ARR[@]}"; do
        local ini_entry
        ini_entry="$(resolve_ini_entry "$ini")"
        INI_ARGS+=" $(printf '%q' "$ini_entry")"
    done
}

extract_ini_key() {
    local ini_path="$1"
    local key="$2"
    awk -F= -v key="$key" '
        /^[[:space:]]*[#;]/ { next }
        {
            k = $1
            gsub(/[[:space:]]/, "", k)
            if (k == key) {
                v = $2
                sub(/^[[:space:]]+/, "", v)
                sub(/[[:space:]]+$/, "", v)
                print v
                exit
            }
        }
    ' "$ini_path"
}

validate_stream_ids_and_dests() {
    declare -A stream_key_owner=()
    declare -A uid_missing_stream_addr_owner=()
    declare -A talker_dest_owner=()
    local ini_entry ini_path role uid dest stream_addr stream_key

    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        role="$(extract_ini_key "$ini_path" "role" || true)"
        role="${role,,}"

        # Endpoint stream identity is (stream_addr + stream_uid).
        # Enforce uniqueness on that tuple, not stream_uid alone.
        uid="$(extract_ini_key "$ini_path" "stream_uid" || true)"
        if [[ -n "$uid" ]]; then
            stream_addr="$(extract_ini_key "$ini_path" "stream_addr" || true)"
            stream_addr="${stream_addr,,}"

            if [[ -n "$stream_addr" ]]; then
                stream_key="${stream_addr}|${uid}"
                if [[ -n "${stream_key_owner[$stream_key]:-}" && "${stream_key_owner[$stream_key]}" != "$ini_path" ]]; then
                    echo "ERROR: Duplicate stream tuple stream_addr=$stream_addr stream_uid=$uid in $ini_path and ${stream_key_owner[$stream_key]}" >&2
                    exit 1
                fi
                stream_key_owner[$stream_key]="$ini_path"
            else
                # If stream_addr is not specified, TL may derive it from ifname.
                # In that case, duplicate UIDs are risky and usually collide.
                if [[ -n "${uid_missing_stream_addr_owner[$uid]:-}" && "${uid_missing_stream_addr_owner[$uid]}" != "$ini_path" ]]; then
                    echo "ERROR: Duplicate stream_uid=$uid with unspecified stream_addr in $ini_path and ${uid_missing_stream_addr_owner[$uid]}" >&2
                    echo "Set explicit stream_addr or choose unique stream_uid." >&2
                    exit 1
                fi
                uid_missing_stream_addr_owner[$uid]="$ini_path"
            fi
        fi

        # dest_addr uniqueness is only required for talkers.
        if [[ -n "$role" && "$role" != "talker" ]]; then
            continue
        fi

        dest="$(extract_ini_key "$ini_path" "dest_addr" || true)"
        if [[ -n "$dest" ]]; then
            dest="${dest,,}"
            if [[ -n "${talker_dest_owner[$dest]:-}" && "${talker_dest_owner[$dest]}" != "$ini_path" ]]; then
                echo "ERROR: Duplicate talker dest_addr=$dest in $ini_path and ${talker_dest_owner[$dest]}" >&2
                exit 1
            fi
            talker_dest_owner[$dest]="$ini_path"
        fi
    done
}

collect_expected_uids() {
    EXPECTED_UIDS=()
    local ini_entry ini_path uid
    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        uid="$(extract_ini_key "$ini_path" "stream_uid" || true)"
        [[ -n "$uid" ]] && EXPECTED_UIDS+=("$uid")
    done
}

collect_expected_maap_uids() {
    EXPECTED_MAAP_UIDS=()
    local ini_entry ini_path uid dest
    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        uid="$(extract_ini_key "$ini_path" "stream_uid" || true)"
        [[ -z "$uid" ]] && continue
        dest="$(extract_ini_key "$ini_path" "dest_addr" || true)"
        # No explicit dest_addr means startup uses a placeholder and MAAP should
        # provide the final multicast destination before controller connect.
        if [[ -z "$dest" ]]; then
            EXPECTED_MAAP_UIDS+=("$uid")
        fi
    done
}

wait_for_host_stream_registration() {
    local timeout="$STREAM_READY_WAIT_SEC"
    [[ "$timeout" -le 0 ]] && return 0

    local deadline=$((SECONDS + timeout))
    local uid
    local missing=()
    local expected_count="${#EXPECTED_UIDS[@]}"
    local running_clients=0

    while (( SECONDS < deadline )); do
        missing=()
        for uid in "${EXPECTED_UIDS[@]}"; do
            if ! rg -q "Register .*/$uid: class:" "$HOST_LOG"; then
                missing+=("$uid")
            fi
        done

        if (( ${#missing[@]} == 0 )); then
            echo "Host stream registration complete for UIDs: ${EXPECTED_UIDS[*]}"
            return 0
        fi

        # Newer host builds can skip "Register ... class" log lines even when
        # all stream clients are running. Treat client-running state as ready.
        running_clients="$(awk '
            /Client [0-9]+ state changed to Running|Client [0-9]+ state is already at Running/ {
                for (i = 1; i <= NF; i++) {
                    if ($i == "Client") {
                        id = $(i + 1)
                        gsub(/[^0-9]/, "", id)
                        if (id != "") seen[id] = 1
                    }
                }
            }
            END {
                c = 0
                for (k in seen) c++
                print c + 0
            }
        ' "$HOST_LOG" 2>/dev/null || echo 0)"
        if (( expected_count > 0 && running_clients >= expected_count )); then
            echo "Host stream clients ready (${running_clients}/${expected_count}); UID register lines not present in log format."
            return 0
        fi
        sleep 1
    done

    echo "WARNING: Host stream readiness timeout after ${timeout}s. Missing UIDs by register-pattern: ${missing[*]} (running-clients=${running_clients}/${expected_count})" >&2
    return 0
}

wait_for_host_maap_ready() {
    local timeout="$MAAP_READY_WAIT_SEC"
    local expected="${#EXPECTED_MAAP_UIDS[@]}"
    [[ "$timeout" -le 0 || "$expected" -le 0 ]] && return 0

    local deadline=$((SECONDS + timeout))
    local count_alloc=0
    while (( SECONDS < deadline )); do
        count_alloc="$(rg -c "Endpoint MAAP] INFO: Allocated MAAP address" "$HOST_LOG" 2>/dev/null || echo 0)"
        if (( count_alloc >= expected )); then
            echo "Host MAAP ready (${count_alloc}/${expected} allocations observed)."
            return 0
        fi
        sleep 1
    done

    echo "WARNING: Host MAAP readiness timeout after ${timeout}s (${count_alloc}/${expected} allocations observed)." >&2
    echo "WARNING: AVDECC may advertise placeholder destination MACs until MAAP settles." >&2
    return 0
}

wait_for_host_endpoint_ready() {
    local timeout="$HOST_ENDPOINT_READY_SEC"
    [[ "$timeout" -le 0 ]] && return 0

    local deadline=$((SECONDS + timeout))
    local marker_re
    marker_re="TX buffers:|IGB launch time feature is|detected domain Class A|Register .*/[0-9]+: class:"
    while (( SECONDS < deadline )); do
        # Host is only truly ready once endpoint IPC exists; otherwise AVDECC can
        # appear healthy while host is stuck before endpoint initialization.
        if [[ -S "$IPC_ENDPOINT_SOCK" ]] && rg -q "$marker_re" "$HOST_LOG"; then
            echo "Host endpoint core ready."
            return 0
        fi

        # SRP startup can race mrpd readiness. Signal a retryable status so the
        # caller can restart host without tearing down the full stack.
        if rg -q "Failed to initialize SRP|Make sure that mrpd daemon is started\\." "$HOST_LOG"; then
            echo "WARNING: Host SRP initialization not ready yet. Will retry host startup. See $HOST_LOG" >&2
            return 2
        fi

        if rg -q \
            "Failed to initialize QMgr|Failed to initialize MAAP|Failed to initialize Shaper|PTP failed to start - Exiting|Failed to start endpoint thread|connect failed .*are you running as root\\?|init failed .*driver really loaded" \
            "$HOST_LOG"; then
            echo "ERROR: Host endpoint startup failure detected. See $HOST_LOG" >&2
            tail -n 80 "$HOST_LOG" >&2 || true
            return 1
        fi

        sleep 1
    done

    if rg -q "Failed to initialize SRP|Make sure that mrpd daemon is started\\." "$HOST_LOG"; then
        echo "WARNING: Host endpoint SRP startup not ready after ${timeout}s. Will retry host startup. See $HOST_LOG" >&2
        return 2
    fi

    echo "ERROR: Host endpoint not ready after ${timeout}s." >&2
    if [[ ! -S "$IPC_ENDPOINT_SOCK" ]]; then
        echo "Hint: endpoint IPC socket missing: $IPC_ENDPOINT_SOCK" >&2
    fi
    echo "Hint: no endpoint readiness markers found in $HOST_LOG." >&2
    echo "Hint: possible igb_avb attach/lock stall in openavb_host." >&2
    tail -n 80 "$HOST_LOG" >&2 || true
    return 1
}

wait_for_gptp_ready() {
    local timeout="$GPTP_READY_WAIT_SEC"
    [[ "$RUN_GPTP" != "1" || "$timeout" -le 0 ]] && return 0

    local deadline=$((SECONDS + timeout))
    while (( SECONDS < deadline )); do
        if rg -q "AsCapable: Enabled|Switching to (Slave|Master)|Starting PDelay" "$GPTP_LOG" 2>/dev/null; then
            return 0
        fi
        if rg -q "bind\\(\\) failed|Error \\(TX\\) timestamping PDelay request, error=" "$GPTP_LOG" 2>/dev/null; then
            echo "ERROR: gPTP startup reported a hard failure. See $GPTP_LOG" >&2
            tail -n 80 "$GPTP_LOG" >&2 || true
            return 1
        fi
        sleep 1
    done

    echo "WARNING: gPTP readiness markers not observed after ${timeout}s; continuing startup." >&2
    tail -n 40 "$GPTP_LOG" >&2 || true
    return 0
}

require_files() {
    apply_stack_profile
    reorder_crf_inis_last
    build_ini_args
    validate_stream_ids_and_dests
    collect_expected_uids
    collect_expected_maap_uids
    local missing=0
    local f
    for f in \
        "${INI_LIST_ARR[@]}" \
        "$ACTIVE_ENDPOINT_INI" \
        "$ACTIVE_AVDECC_INI"
    do
        if [[ "$f" != "$ACTIVE_ENDPOINT_INI" && "$f" != "$ACTIVE_AVDECC_INI" ]]; then
            f="$(resolve_ini_path "${f%%,*}")"
        fi
        if [[ ! -f "$f" ]]; then
            echo "Missing required file: $f" >&2
            missing=1
        fi
    done
    if [[ $missing -ne 0 ]]; then
        exit 1
    fi
}

require_bins() {
    local missing=0
    local f

    for f in "$AVDECC_BIN" "$HOST_BIN"; do
        if [[ ! -x "$f" ]]; then
            echo "Missing executable: $f" >&2
            missing=1
        fi
    done

    if [[ "$RUN_GPTP" == "1" && ! -x "$GPTP_BIN" ]]; then
        echo "Missing executable: $GPTP_BIN" >&2
        missing=1
    fi
    if [[ "$RUN_MRPD" == "1" && ! -x "$MRPD_BIN" ]]; then
        echo "Missing executable: $MRPD_BIN" >&2
        missing=1
    fi
    if [[ "$RUN_MAAP" == "1" && ! -x "$MAAP_BIN" ]]; then
        echo "Missing executable: $MAAP_BIN" >&2
        missing=1
    fi
    if [[ "$RUN_SHAPER" == "1" && ! -x "$SHAPER_BIN" ]]; then
        echo "Missing executable: $SHAPER_BIN" >&2
        missing=1
    fi

    if [[ "$RUN_GPTP" == "1" && ! -d "$GPTP_CWD" ]]; then
        echo "Missing directory: $GPTP_CWD" >&2
        missing=1
    fi

    if [[ $missing -ne 0 ]]; then
        exit 1
    fi
}

build_mrpd_profile_if_requested() {
    local profile cflags

    [[ "$RUN_MRPD" != "1" ]] && return 0
    profile="${MRPD_PROFILE,,}"
    [[ -z "$profile" || "$profile" == "keep" ]] && return 0

    case "$profile" in
        default)
            cflags="$MRPD_DEFAULT_CFLAGS"
            ;;
        debug)
            cflags="$MRPD_DEBUG_CFLAGS"
            ;;
        custom)
            if [[ -z "$MRPD_CFLAGS" ]]; then
                echo "ERROR: MRPD_PROFILE=custom requires MRPD_CFLAGS to be set." >&2
                return 1
            fi
            cflags="$MRPD_CFLAGS"
            ;;
        *)
            echo "ERROR: Unsupported MRPD_PROFILE='$MRPD_PROFILE'. Use keep|default|debug|custom." >&2
            return 1
            ;;
    esac

    echo "Building mrpd profile '$profile'..."
    make -C "$MRPD_MAKE_DIR" clean >/dev/null
    make -C "$MRPD_MAKE_DIR" CFLAGS="$cflags"
}

stop_session() {
    local sess="$1"
    tmux kill-session -t "$sess" 2>/dev/null || true
}

cleanup_ipc_paths() {
    local path
    local i
    for path in "$IPC_ENDPOINT_SOCK" "$IPC_AVDECC_SOCK"; do
        for ((i=0; i<IPC_CLEAN_RETRIES; i++)); do
            rm -f "$path" 2>/dev/null || true
            [[ ! -e "$path" ]] && break
            sleep "$IPC_CLEAN_DELAY_SEC"
        done
        if [[ -e "$path" ]]; then
            echo "ERROR: Failed to remove stale IPC path: $path" >&2
            ls -l "$path" >&2 || true
            exit 1
        fi
    done
}

start_session() {
    local sess="$1"
    local cmd="$2"
    stop_session "$sess"
    tmux new-session -d -s "$sess" "$cmd"
}

stop_stack() {
    stop_session "$SESS_HOST"
    stop_session "$SESS_AVDECC"
    stop_session "$SESS_SHAPER"
    stop_session "$SESS_MAAP"
    stop_session "$SESS_MRPD_WATCH"
    stop_session "$SESS_MRPD"
    stop_session "$SESS_GPTP"

    # Also stop non-tmux leftovers to keep runs deterministic.
    pkill -f "[d]aemon_cl $IFACE_DAEMONS" 2>/dev/null || true
    pkill -f "[m]rpd .* -i $IFACE_DAEMONS" 2>/dev/null || true
    pkill -f "[m]aap_daemon -i $IFACE_DAEMONS" 2>/dev/null || true
    pkill -f "[s]haper_daemon" 2>/dev/null || true
    pkill -f "[o]penavb_avdecc -I $IFACE_AVDECC" 2>/dev/null || true
    pkill -f "[o]penavb_host -I $IFACE_HOST" 2>/dev/null || true
}

start_stack() {
    local profile="${STACK_PROFILE,,}"

    require_files
    build_mrpd_profile_if_requested
    require_bins
    stop_stack
    cleanup_ipc_paths

    mkdir -p "$LOG_DIR"
    rm -f \
        "$GPTP_LOG" "$MRPD_LOG" "$MAAP_LOG" "$SHAPER_LOG" \
        "$AVDECC_LOG" "$HOST_LOG" "$MRPD_TALKER_FAILED_LOG"

    if [[ "$profile" == "wav" || "$profile" == "tonegen4wav" || "$profile" == "tonegen-wav" || "$profile" == "tonegen_wav" || "$profile" == "wav-listener" || "$profile" == "wav_listener" ]]; then
        rm -f "$WAV_CAPTURE_FILE"
    fi

    if [[ "$RUN_GPTP" == "1" ]]; then
        start_session "$SESS_GPTP" \
            "cd $GPTP_CWD && exec $GPTP_BIN $IFACE_DAEMONS $GPTP_ARGS >>$GPTP_LOG 2>&1"
        if ! wait_for_gptp_ready; then
            stop_stack
            return 1
        fi
    fi
    if [[ "$RUN_MRPD" == "1" ]]; then
        local mrpd_log_switch
        if [[ -n "$MRPD_LOG_FLAGS" ]]; then
            mrpd_log_switch="-${MRPD_LOG_FLAGS}"
        else
            mrpd_log_switch=""
        fi
        start_session "$SESS_MRPD" \
            "exec stdbuf -oL -eL $MRPD_BIN $mrpd_log_switch -i $IFACE_DAEMONS $MRPD_ARGS >>$MRPD_LOG 2>&1"
        sleep 1
        if [[ "$RUN_MRPD_WATCH" == "1" ]]; then
            start_session "$SESS_MRPD_WATCH" \
                "exec stdbuf -oL tail -n0 -F $MRPD_LOG 2>/dev/null | stdbuf -oL rg --line-buffered 'TALKER FAILED' | stdbuf -oL sed -n 's/^MRPD \\([0-9][0-9]*\\.[0-9][0-9]*\\).*$/\\1/p' >>$MRPD_TALKER_FAILED_LOG 2>&1"
        fi
    fi
    if [[ "$RUN_MAAP" == "1" ]]; then
        start_session "$SESS_MAAP" \
            "exec $MAAP_BIN -i $IFACE_DAEMONS $MAAP_ARGS >>$MAAP_LOG 2>&1"
        sleep 1
    fi
    if [[ "$RUN_SHAPER" == "1" ]]; then
        local shaper_env
        shaper_env="SHAPER_TC_LOG=$(printf '%q' "$SHAPER_TC_LOG")"
        [[ -n "$SHAPER_SKIP_ROOT_QDISC" ]] && shaper_env+=" SHAPER_SKIP_ROOT_QDISC=$(printf '%q' "$SHAPER_SKIP_ROOT_QDISC")"
        [[ -n "$SHAPER_LINK_SPEED_MBPS" ]] && shaper_env+=" SHAPER_LINK_SPEED_MBPS=$(printf '%q' "$SHAPER_LINK_SPEED_MBPS")"
        [[ -n "$SHAPER_CLASSA_PARENT" ]] && shaper_env+=" SHAPER_CLASSA_PARENT=$(printf '%q' "$SHAPER_CLASSA_PARENT")"
        [[ -n "$SHAPER_CLASSB_PARENT" ]] && shaper_env+=" SHAPER_CLASSB_PARENT=$(printf '%q' "$SHAPER_CLASSB_PARENT")"
        [[ -n "$SHAPER_CLASSA_HANDLE" ]] && shaper_env+=" SHAPER_CLASSA_HANDLE=$(printf '%q' "$SHAPER_CLASSA_HANDLE")"
        [[ -n "$SHAPER_CLASSB_HANDLE" ]] && shaper_env+=" SHAPER_CLASSB_HANDLE=$(printf '%q' "$SHAPER_CLASSB_HANDLE")"
        [[ -n "$SHAPER_EGRESS_QMAP" ]] && shaper_env+=" SHAPER_EGRESS_QMAP=$(printf '%q' "$SHAPER_EGRESS_QMAP")"
        [[ -n "$SHAPER_CLASSA_QMAP" ]] && shaper_env+=" SHAPER_CLASSA_QMAP=$(printf '%q' "$SHAPER_CLASSA_QMAP")"
        [[ -n "$SHAPER_CLASSB_QMAP" ]] && shaper_env+=" SHAPER_CLASSB_QMAP=$(printf '%q' "$SHAPER_CLASSB_QMAP")"
        [[ -n "$SHAPER_DEFAULT_QMAP" ]] && shaper_env+=" SHAPER_DEFAULT_QMAP=$(printf '%q' "$SHAPER_DEFAULT_QMAP")"
        [[ -n "$SHAPER_GPTP_QMAP" ]] && shaper_env+=" SHAPER_GPTP_QMAP=$(printf '%q' "$SHAPER_GPTP_QMAP")"
        [[ -n "$SHAPER_CLASSA_QDISC" ]] && shaper_env+=" SHAPER_CLASSA_QDISC=$(printf '%q' "$SHAPER_CLASSA_QDISC")"
        [[ -n "$SHAPER_CLASSA_CBS_OFFLOAD" ]] && shaper_env+=" SHAPER_CLASSA_CBS_OFFLOAD=$(printf '%q' "$SHAPER_CLASSA_CBS_OFFLOAD")"
        [[ -n "$SHAPER_CLASSA_ETF_DELTA_NS" ]] && shaper_env+=" SHAPER_CLASSA_ETF_DELTA_NS=$(printf '%q' "$SHAPER_CLASSA_ETF_DELTA_NS")"
        [[ -n "$SHAPER_CLASSA_ETF_OFFLOAD" ]] && shaper_env+=" SHAPER_CLASSA_ETF_OFFLOAD=$(printf '%q' "$SHAPER_CLASSA_ETF_OFFLOAD")"
        [[ -n "$SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK" ]] && shaper_env+=" SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK=$(printf '%q' "$SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK")"
        [[ -n "$SHAPER_CLASSB_QDISC" ]] && shaper_env+=" SHAPER_CLASSB_QDISC=$(printf '%q' "$SHAPER_CLASSB_QDISC")"
        [[ -n "$SHAPER_CLASSB_CBS_OFFLOAD" ]] && shaper_env+=" SHAPER_CLASSB_CBS_OFFLOAD=$(printf '%q' "$SHAPER_CLASSB_CBS_OFFLOAD")"
        [[ -n "$SHAPER_CLASSB_ETF_DELTA_NS" ]] && shaper_env+=" SHAPER_CLASSB_ETF_DELTA_NS=$(printf '%q' "$SHAPER_CLASSB_ETF_DELTA_NS")"
        [[ -n "$SHAPER_CLASSB_ETF_OFFLOAD" ]] && shaper_env+=" SHAPER_CLASSB_ETF_OFFLOAD=$(printf '%q' "$SHAPER_CLASSB_ETF_OFFLOAD")"
        [[ -n "$SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK" ]] && shaper_env+=" SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK=$(printf '%q' "$SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK")"
        [[ -n "$SHAPER_MQPRIO_HW" ]] && shaper_env+=" SHAPER_MQPRIO_HW=$(printf '%q' "$SHAPER_MQPRIO_HW")"
        [[ -n "$SHAPER_TC_MAP" ]] && shaper_env+=" SHAPER_TC_MAP=$(printf '%q' "$SHAPER_TC_MAP")"
        start_session "$SESS_SHAPER" \
            "exec env $shaper_env $SHAPER_BIN $SHAPER_ARGS >>$SHAPER_LOG 2>&1"
        sleep 1
    fi

    local host_cmd
    local host_env
    host_env="OPENAVB_ENDPOINT_INI=$ACTIVE_ENDPOINT_INI OPENAVB_ENDPOINT_SAVE_INI=$ACTIVE_ENDPOINT_SAVE_INI"
    [[ -n "$OPENAVB_DISABLE_SO_TXTIME" ]] && host_env+=" OPENAVB_DISABLE_SO_TXTIME=$(printf '%q' "$OPENAVB_DISABLE_SO_TXTIME")"
    host_cmd="cd $CFG_DIR && env $host_env $HOST_BIN -I $IFACE_HOST $INI_ARGS >>$HOST_LOG 2>&1"
    local host_attempt=0
    local host_max_attempts=1
    local host_ready_rc=1
    if [[ "$RUN_MRPD" == "1" && "$HOST_SRP_RETRIES" -gt 0 ]]; then
        host_max_attempts=$((HOST_SRP_RETRIES + 1))
    fi
    while (( host_attempt < host_max_attempts )); do
        if (( host_attempt > 0 )); then
            echo "Retrying host startup after SRP initialization failure (attempt $((host_attempt + 1))/$host_max_attempts)." >&2
            sleep "$HOST_SRP_RETRY_DELAY_SEC"
        fi
        : > "$HOST_LOG"
        start_session "$SESS_HOST" "$host_cmd"
        if wait_for_host_endpoint_ready; then
            host_ready_rc=0
            break
        else
            host_ready_rc=$?
        fi
        if (( host_ready_rc == 2 )) && (( host_attempt + 1 < host_max_attempts )); then
            stop_session "$SESS_HOST"
            host_attempt=$((host_attempt + 1))
            continue
        fi
        stop_stack
        return 1
    done
    if (( host_ready_rc != 0 )); then
        stop_stack
        return 1
    fi

    wait_for_host_stream_registration
    wait_for_host_maap_ready

    # Start AVDECC after host/endpoint is ready so CONNECT_TX processing cannot
    # race stream-client registration and return transient talker misbehaving.
    start_session "$SESS_AVDECC" \
        "cd $CFG_DIR && OPENAVB_AVDECC_INI=$ACTIVE_AVDECC_INI exec $AVDECC_BIN -I $IFACE_AVDECC $INI_ARGS >>$AVDECC_LOG 2>&1"

    sleep 1

    # Let host and endpoint settle, but keep this after host startup so controllers
    # don't hit a long pre-host window that can report transient ACMP status 13.
    if [[ "$RUN_MRPD" == "1" && "$SRP_SETTLE_SEC" -gt 0 ]]; then
        sleep "$SRP_SETTLE_SEC"
    fi

    echo "Started."
    [[ "$RUN_GPTP" == "1" ]] && echo "  GPTP session:   $SESS_GPTP"
    [[ "$RUN_MRPD" == "1" ]] && echo "  MRPD session:   $SESS_MRPD"
    [[ "$RUN_MRPD" == "1" && "$RUN_MRPD_WATCH" == "1" ]] && echo "  MRPD watch:     $SESS_MRPD_WATCH"
    [[ "$RUN_MRPD" == "1" ]] && echo "  MRPD profile:   $MRPD_PROFILE"
    [[ "$RUN_MAAP" == "1" ]] && echo "  MAAP session:   $SESS_MAAP"
    [[ "$RUN_SHAPER" == "1" ]] && echo "  SHAPER session: $SESS_SHAPER"
    echo "  AVDECC session: $SESS_AVDECC"
    echo "  HOST session:   $SESS_HOST"
    echo "  Logs:"
    [[ "$RUN_GPTP" == "1" ]] && echo "    $GPTP_LOG"
    [[ "$RUN_MRPD" == "1" ]] && echo "    $MRPD_LOG"
    [[ "$RUN_MRPD" == "1" && "$RUN_MRPD_WATCH" == "1" ]] && echo "    $MRPD_TALKER_FAILED_LOG"
    [[ "$RUN_MAAP" == "1" ]] && echo "    $MAAP_LOG"
    [[ "$RUN_SHAPER" == "1" ]] && echo "    $SHAPER_LOG"
    echo "    $AVDECC_LOG"
    echo "    $HOST_LOG"
    echo "  INI files:$INI_ARGS"
    echo "  Endpoint INI: $ACTIVE_ENDPOINT_INI"
    echo "  AVDECC INI:  $ACTIVE_AVDECC_INI"
}

status_stack() {
    echo "Sessions:"
    tmux ls 2>/dev/null | rg "$SESS_GPTP|$SESS_MRPD|$SESS_MRPD_WATCH|$SESS_MAAP|$SESS_SHAPER|$SESS_AVDECC|$SESS_HOST" || true
    echo
    echo "Processes:"
    pgrep -af "[d]aemon_cl|[m]rpd|[m]aap_daemon|[s]haper_daemon|[o]penavb_avdecc|[o]penavb_host" || true
}

logs_stack() {
    if [[ "$RUN_GPTP" == "1" ]]; then
        echo "== $GPTP_LOG =="
        tail -n 80 "$GPTP_LOG" 2>/dev/null || true
        echo
    fi
    if [[ "$RUN_MRPD" == "1" ]]; then
        echo "== $MRPD_LOG =="
        tail -n 80 "$MRPD_LOG" 2>/dev/null || true
        echo
        if [[ "$RUN_MRPD_WATCH" == "1" ]]; then
            echo "== $MRPD_TALKER_FAILED_LOG =="
            tail -n 80 "$MRPD_TALKER_FAILED_LOG" 2>/dev/null || true
            echo
        fi
    fi
    if [[ "$RUN_MAAP" == "1" ]]; then
        echo "== $MAAP_LOG =="
        tail -n 80 "$MAAP_LOG" 2>/dev/null || true
        echo
    fi
    if [[ "$RUN_SHAPER" == "1" ]]; then
        echo "== $SHAPER_LOG =="
        tail -n 80 "$SHAPER_LOG" 2>/dev/null || true
        echo
    fi
    echo "== $AVDECC_LOG =="
    tail -n 80 "$AVDECC_LOG" 2>/dev/null || true
    echo
    echo "== $HOST_LOG =="
    tail -n 80 "$HOST_LOG" 2>/dev/null || true
}

cmd="${1:-}"
case "$cmd" in
    start)
        start_stack
        ;;
    stop)
        stop_stack
        cleanup_ipc_paths
        ;;
    restart)
        stop_stack
        start_stack
        ;;
    status)
        status_stack
        ;;
    logs)
        logs_stack
        ;;
    *)
        usage
        exit 1
        ;;
esac
