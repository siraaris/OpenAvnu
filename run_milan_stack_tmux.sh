#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_PARENT="$(cd "$REPO_ROOT/.." && pwd)"
MILAN_BUS32SPLIT_DIR="$REPO_ROOT/lib/avtp_pipeline/platform/Linux/intf_bus32split"
MILAN_TEST_CONFIG_DIR="$REPO_ROOT/test_configs/milan"
MILAN_ENDPOINT_INI="$MILAN_TEST_CONFIG_DIR/endpoint.ini"
MILAN_AVDECC_INI="$MILAN_TEST_CONFIG_DIR/avdecc.ini"
MILAN_GPTP_CFG_INI="$MILAN_TEST_CONFIG_DIR/gptp_cfg.ini"
GPTP_REPO_ROOT="${GPTP_REPO_ROOT:-$REPO_PARENT/gptp}"
CONTROL_USER="${OPENAVNU_CONTROL_USER:-${SUDO_USER:-$(id -un)}}"
CONTROL_HOME="$(getent passwd "$CONTROL_USER" 2>/dev/null | awk -F: 'NR==1 { print $6 }')"
[[ -z "$CONTROL_HOME" ]] && CONTROL_HOME="${HOME:-/root}"
CONTROL_GROUP="$(id -gn "$CONTROL_USER" 2>/dev/null || id -gn)"
OPENAVNU_CONFIG_DIR="${OPENAVNU_CONFIG_DIR:-$CONTROL_HOME/.config/openavnu}"
OPENAVNU_STATE_INI="${OPENAVNU_STATE_INI:-$OPENAVNU_CONFIG_DIR/state.ini}"

SESS_GPTP="${SESS_GPTP:-gptp_run}"
SESS_PHC2SYS="${SESS_PHC2SYS:-phc2sys_run}"
SESS_MRPD="${SESS_MRPD:-mrpd_run}"
SESS_MRPD_WATCH="${SESS_MRPD_WATCH:-mrpd_watch_run}"
SESS_MAAP="${SESS_MAAP:-maap_run}"
SESS_SHAPER="${SESS_SHAPER:-shaper_run}"
SESS_AVDECC="${SESS_AVDECC:-avdecc_run}"
SESS_HOST="${SESS_HOST:-host_run}"

CFG_DIR="${CFG_DIR:-$REPO_ROOT}"
AUDIO_INI_0="${AUDIO_INI_0:-$MILAN_BUS32SPLIT_DIR/bus32split_milan_0.ini}"
AUDIO_INI_1="${AUDIO_INI_1:-$MILAN_BUS32SPLIT_DIR/bus32split_milan_1.ini}"
AUDIO_INI_2="${AUDIO_INI_2:-$MILAN_BUS32SPLIT_DIR/bus32split_milan_2.ini}"
AUDIO_INI_3="${AUDIO_INI_3:-$MILAN_BUS32SPLIT_DIR/bus32split_milan_3.ini}"
SPLIT32_CRF_TALKER_INI="${SPLIT32_CRF_TALKER_INI:-$MILAN_BUS32SPLIT_DIR/bus32split_milan_crf_talker.ini}"
SPLIT32_CRF_LISTENER_INI="${SPLIT32_CRF_LISTENER_INI:-$MILAN_BUS32SPLIT_DIR/bus32split_milan_crf_listener.ini}"
CRF_TONEGEN_INI="${CRF_TONEGEN_INI:-$MILAN_TEST_CONFIG_DIR/crf_talker_milan.ini}"
CRF_LISTENER_INI="${CRF_LISTENER_INI:-$MILAN_TEST_CONFIG_DIR/crf_listener_milan.ini}"
TONEGEN_INI_0="${TONEGEN_INI_0:-$MILAN_TEST_CONFIG_DIR/tonegen_milan_0.ini}"
TONEGEN_INI_1="${TONEGEN_INI_1:-$MILAN_TEST_CONFIG_DIR/tonegen_milan_1.ini}"
TONEGEN_INI_2="${TONEGEN_INI_2:-$MILAN_TEST_CONFIG_DIR/tonegen_milan_2.ini}"
TONEGEN_INI_3="${TONEGEN_INI_3:-$MILAN_TEST_CONFIG_DIR/tonegen_milan_3.ini}"
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
STOCK_IGB_CLASSA_ETF_OFFLOAD="${STOCK_IGB_CLASSA_ETF_OFFLOAD:-1}"
STOCK_IGB_CLASSB_CBS_OFFLOAD="${STOCK_IGB_CLASSB_CBS_OFFLOAD:-0}"
STOCK_IGB_CLASSB_ETF_OFFLOAD="${STOCK_IGB_CLASSB_ETF_OFFLOAD:-0}"
STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC="${STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC:-12}"
BUS32_AAF_DEFERRED_START_BASE_USEC="${BUS32_AAF_DEFERRED_START_BASE_USEC:-500000}"
BUS32_AAF_DEFERRED_START_STEP_USEC="${BUS32_AAF_DEFERRED_START_STEP_USEC:-250000}"
BUS32_CRF_DIAG_ENABLE="${BUS32_CRF_DIAG_ENABLE:-1}"
BUS32_CRF_DIAG_LOG_EVERY_PACKETS="${BUS32_CRF_DIAG_LOG_EVERY_PACKETS:-500}"
BUS32_CRF_DIAG_JITTER_THRESH_NS="${BUS32_CRF_DIAG_JITTER_THRESH_NS:-250000}"
WAV_LISTENER_INI="${WAV_LISTENER_INI:-$REPO_ROOT/test_configs/aaf_wav_listener_8ch.ini}"
WAV_CAPTURE_FILE="${WAV_CAPTURE_FILE:-/tmp/openavnu_rme_capture_8ch.wav}"

TONEGEN_DEFAULT_INI_FILES="${TONEGEN_DEFAULT_INI_FILES:-$TONEGEN_INI_0,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_1,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_2,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_3,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $CRF_TONEGEN_INI,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC $CRF_LISTENER_INI}"
WAV_TONEGEN_DEFAULT_INI_FILES="${WAV_TONEGEN_DEFAULT_INI_FILES:-$TONEGEN_INI_0,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_1,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_2,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $TONEGEN_INI_3,max_transit_usec=$TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_min_lead_usec=$TONEGEN_TX_MIN_LEAD_USEC,intf_nv_fixed_ts_runtime_lead_usec=$TONEGEN_FIXED_TS_RUNTIME_LEAD_USEC,map_nv_selected_clock_follow_updates=$TONEGEN_SELECTED_CLOCK_FOLLOW_UPDATES,map_nv_selected_clock_warmup_usec=$TONEGEN_SELECTED_CLOCK_WARMUP_USEC $CRF_TONEGEN_INI,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC $CRF_LISTENER_INI $WAV_LISTENER_INI,intf_nv_file_name_rx=$WAV_CAPTURE_FILE}"
STOCK_IGB_IFACE_HOST="${STOCK_IGB_IFACE_HOST:-sendmmsg:enp2s0}"
STOCK_IGB_IFACE_AVDECC="${STOCK_IGB_IFACE_AVDECC:-enp2s0}"
STOCK_IGB_IFACE_DAEMONS="${STOCK_IGB_IFACE_DAEMONS:-enp2s0}"
STOCK_IGB_SHAPER_TC_MAP="${STOCK_IGB_SHAPER_TC_MAP:-3 3 2 1 3 3 0 0 3 3 3 3 3 3 3 3}"
STOCK_IGB_MQPRIO_CLASSA_PARENT="${STOCK_IGB_MQPRIO_CLASSA_PARENT:-1:2}"
STOCK_IGB_MQPRIO_CLASSB_PARENT="${STOCK_IGB_MQPRIO_CLASSB_PARENT:-1:3}"
STOCK_IGB_MQPRIO_CLASSA_HANDLE="${STOCK_IGB_MQPRIO_CLASSA_HANDLE:-2}"
STOCK_IGB_MQPRIO_CLASSB_HANDLE="${STOCK_IGB_MQPRIO_CLASSB_HANDLE:-3}"
INI_FILES="${INI_FILES:-}"
STACK_PROFILE="${STACK_PROFILE:-split32}"

AVDECC_BIN="${AVDECC_BIN:-$REPO_ROOT/lib/avtp_pipeline/build_avdecc/platform/Linux/avb_avdecc/openavb_avdecc}"
HOST_BIN="${HOST_BIN:-$REPO_ROOT/lib/avtp_pipeline/build/platform/Linux/avb_host/openavb_host}"
GPTP_BIN="${GPTP_BIN:-$GPTP_REPO_ROOT/linux/build/obj/daemon_cl}"
PHC2SYS_BIN="${PHC2SYS_BIN:-$(command -v phc2sys 2>/dev/null || true)}"
MRPD_BIN="${MRPD_BIN:-$REPO_ROOT/daemons/mrpd/mrpd}"
MAAP_BIN="${MAAP_BIN:-$REPO_ROOT/daemons/maap/linux/build/maap_daemon}"
SHAPER_BIN="${SHAPER_BIN:-$REPO_ROOT/daemons/shaper/shaper_daemon}"
MRPD_MAKE_DIR="${MRPD_MAKE_DIR:-$REPO_ROOT/daemons/mrpd}"
MRPQ_BIN="${MRPQ_BIN:-$REPO_ROOT/examples/mrp_client/mrpq}"
MRPD_QUERY_BIN="${MRPD_QUERY_BIN:-}"
ETHTOOL_BIN="${ETHTOOL_BIN:-$(command -v ethtool 2>/dev/null || true)}"
TC_BIN="${TC_BIN:-$(command -v tc 2>/dev/null || true)}"

IFACE_AVDECC="${IFACE_AVDECC:-enp2s0}"
IFACE_HOST="${IFACE_HOST:-sendmmsg:enp2s0}"
IFACE_DAEMONS="${IFACE_DAEMONS:-enp2s0}"

ENDPOINT_INI="${ENDPOINT_INI:-$MILAN_ENDPOINT_INI}"
ENDPOINT_SAVE_INI="${ENDPOINT_SAVE_INI:-$OPENAVNU_STATE_INI}"
AVDECC_INI="${AVDECC_INI:-$MILAN_AVDECC_INI}"

ACTIVE_ENDPOINT_INI="$ENDPOINT_INI"
ACTIVE_ENDPOINT_SAVE_INI="$ENDPOINT_SAVE_INI"
ACTIVE_AVDECC_INI="$AVDECC_INI"

RUN_GPTP="${RUN_GPTP:-1}"
RUN_PHC2SYS="${RUN_PHC2SYS:-1}"
RUN_MRPD="${RUN_MRPD:-1}"
RUN_MRPD_WATCH="${RUN_MRPD_WATCH:-1}"
RUN_MAAP="${RUN_MAAP:-1}"
RUN_SHAPER="${RUN_SHAPER:-1}"
PERSIST_MILAN_INFRA="${PERSIST_MILAN_INFRA:-1}"
NIC_TUNE="${NIC_TUNE:-${RING_CHECK:-1}}"
RING_IFACE="${RING_IFACE:-$IFACE_DAEMONS}"
RING_RX_TARGET="${RING_RX_TARGET:-512}"
RING_TX_TARGET="${RING_TX_TARGET:-512}"
NIC_COMBINED_QUEUES="${NIC_COMBINED_QUEUES:-4}"
NIC_COALESCE_RX_USECS="${NIC_COALESCE_RX_USECS:-0}"
NIC_COALESCE_TX_USECS="${NIC_COALESCE_TX_USECS:-0}"

GPTP_CWD="${GPTP_CWD:-$GPTP_REPO_ROOT/linux/build/obj}"
GPTP_ARGS="${GPTP_ARGS:--F $MILAN_GPTP_CFG_INI}"
PHC2SYS_SOURCE="${PHC2SYS_SOURCE:-/dev/ptp0}"
PHC2SYS_CLOCK="${PHC2SYS_CLOCK:-CLOCK_REALTIME}"
PHC2SYS_RATE_HZ="${PHC2SYS_RATE_HZ:-8}"
PHC2SYS_SAMPLES="${PHC2SYS_SAMPLES:-5}"
PHC2SYS_OFFSET_NS="${PHC2SYS_OFFSET_NS:-0}"
PHC2SYS_ARGS="${PHC2SYS_ARGS:--s $PHC2SYS_SOURCE -c $PHC2SYS_CLOCK -R $PHC2SYS_RATE_HZ -N $PHC2SYS_SAMPLES -O $PHC2SYS_OFFSET_NS -m}"
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
SHAPER_INIT_IFACE="${SHAPER_INIT_IFACE:-$IFACE_DAEMONS}"
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
PHC2SYS_READY_WAIT_SEC="${PHC2SYS_READY_WAIT_SEC:-5}"
MRPD_READY_WAIT_SEC="${MRPD_READY_WAIT_SEC:-20}"
PHC2SYS_LOG_WINDOW_MIN="${PHC2SYS_LOG_WINDOW_MIN:-15}"
PHC2SYS_LOG_TRIM_EVERY="${PHC2SYS_LOG_TRIM_EVERY:-120}"
PHC2SYS_LOG_MAX_LINES="${PHC2SYS_LOG_MAX_LINES:-$((PHC2SYS_RATE_HZ * PHC2SYS_LOG_WINDOW_MIN * 60))}"
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
PROCESS_STOP_WAIT_SEC="${PROCESS_STOP_WAIT_SEC:-2}"
TMUX_STOP_GRACE_SEC="${TMUX_STOP_GRACE_SEC:-3}"
HOST_STOP_SETTLE_SEC="${HOST_STOP_SETTLE_SEC:-1}"
POST_STOP_SETTLE_SEC="${POST_STOP_SETTLE_SEC:-2}"

LOG_DIR="${LOG_DIR:-/var/log/3sb/openavnu}"
SYSTEM_LOG="${SYSTEM_LOG:-$LOG_DIR/system.log}"
GPTP_LOG="${GPTP_LOG:-$LOG_DIR/daemon_cl.log}"
PHC2SYS_LOG="${PHC2SYS_LOG:-$LOG_DIR/phc2sys.log}"
MRPD_LOG="${MRPD_LOG:-$LOG_DIR/mrpd.log}"
MRPD_TALKER_FAILED_LOG="${MRPD_TALKER_FAILED_LOG:-$LOG_DIR/mrpd_talker_failed.log}"
MAAP_LOG="${MAAP_LOG:-$LOG_DIR/maap.log}"
SHAPER_LOG="${SHAPER_LOG:-$LOG_DIR/shaper.log}"
AVDECC_LOG="${AVDECC_LOG:-$LOG_DIR/openavb_avdecc.log}"
HOST_LOG="${HOST_LOG:-$LOG_DIR/openavb_host.log}"
INFRA_STATE_FILE="${INFRA_STATE_FILE:-$LOG_DIR/infra.state}"
HOST_LOG_LEVEL="${HOST_LOG_LEVEL:-warning}"
HOST_LOG_TRIM_EVERY="${HOST_LOG_TRIM_EVERY:-250}"
HOST_LOG_MAX_LINES="${HOST_LOG_MAX_LINES:-12000}"

INI_LIST_ARR=()
INI_ARGS=""
EXPECTED_UIDS=()
EXPECTED_MAAP_UIDS=()
STARTUP_PROGRESS_ACTIVE=0
SHAPER_PRESET_CLASSA_BW=""
SHAPER_PRESET_CLASSA_MAX_FRAME=""
SHAPER_PRESET_CLASSB_BW=""
SHAPER_PRESET_CLASSB_MAX_FRAME=""
SHAPER_PRESET_SUMMARY=""

usage() {
    cat <<'EOF'
Usage:
  run_milan_stack_tmux.sh <start|stop|restart|infra-stop|status|logs|help>

Profiles:
  split32  32-channel bus32split source -> 4 x 8-channel AAF Milan talkers
           plus Milan CRF talker and CRF listener.
  tonegen  4 x 8-channel AAF Milan tone generators plus CRF talker/listener.
  wav      tonegen profile plus 8-channel WAV listener capture.
  crf      CRF talker plus CRF listener only.
  custom   Use INI_FILES exactly as provided.

Defaults:
  STACK_PROFILE=split32
  CFG_DIR=<repo root>
  SPLIT32_CRF_TALKER_INI=<repo>/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_crf_talker.ini
  SPLIT32_CRF_LISTENER_INI=<repo>/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_crf_listener.ini
  CRF_TONEGEN_INI=<repo>/test_configs/milan/crf_talker_milan.ini
  CRF_LISTENER_INI=<repo>/test_configs/milan/crf_listener_milan.ini
  ENDPOINT_INI=<repo>/test_configs/milan/endpoint.ini
  AVDECC_INI=<repo>/test_configs/milan/avdecc.ini
  ENDPOINT_SAVE_INI=<user config>/openavnu/state.ini
  GPTP_ARGS="-F <repo>/test_configs/milan/gptp_cfg.ini"
  RUN_PHC2SYS=1 (-s /dev/ptp0 -c CLOCK_REALTIME -R 8 -N 5 -O 0 -m)
  PERSIST_MILAN_INFRA=1 (keep gPTP/phc2sys/shaper/tc seeded across stop/start)
  NIC_TUNE=1 (--set-eee off, -C rx/tx-usecs 0, -G rx/tx 512, -K offloads off,
              -A pause off, -L combined 4)
  PHC2SYS_LOG_WINDOW_MIN=15 (retain roughly the last 15 minutes of phc2sys logs)
  HOST_LOG_LEVEL=warning (OPENAVB_LOG_LEVEL for openavb_host: none|error|warning|info|status|debug|verbose)
  HOST_LOG_MAX_LINES=12000 (cap retained host log lines)
  WAV_LISTENER_INI=<repo>/test_configs/aaf_wav_listener_8ch.ini
  IFACE_AVDECC=enp2s0
  IFACE_HOST=sendmmsg:enp2s0
  IFACE_DAEMONS=enp2s0

Custom profile:
  INI_FILES="/abs/path/file.ini /abs/path/other.ini,key=value"

Notes:
  - Invoking with no command or with help prints this summary.
  - CRF INIs are automatically moved to the end of INI_FILES so clock streams
    enumerate after audio streams in controllers.
  - `start`/`stop`/`restart` act on STREAM only: `openavb_host` and `openavb_avdecc`.
  - INFRA is `gPTP`, `phc2sys`, `MRPD`, `MRPD watch`, `MAAP`, `shaper`, NIC tuning,
    and resident `tc`. Use `infra-stop` to pull that down explicitly.
  - With PERSIST_MILAN_INFRA=1, `start` reuses INFRA only if the requested run
    config matches the currently seeded infra state.
EOF
}

progress_begin() {
    printf "%s -" "$1"
    STARTUP_PROGRESS_ACTIVE=1
}

progress_tick() {
    [[ "$STARTUP_PROGRESS_ACTIVE" == "1" ]] && printf "-"
}

progress_finish() {
    local detail="${1:-OK}"
    if [[ "$STARTUP_PROGRESS_ACTIVE" == "1" ]]; then
        printf "> %s\n" "$detail"
        STARTUP_PROGRESS_ACTIVE=0
    fi
}

progress_retry() {
    if [[ "$STARTUP_PROGRESS_ACTIVE" == "1" ]]; then
        printf " (Retry) "
    fi
}

progress_abort() {
    if [[ "$STARTUP_PROGRESS_ACTIVE" == "1" ]]; then
        printf "\n"
        STARTUP_PROGRESS_ACTIVE=0
    fi
}

log_startup_note() {
    printf '%s\n' "$1" >>"$SYSTEM_LOG"
}

parse_openavb_log_level() {
    case "${1,,}" in
        0|none) printf '0\n' ;;
        1|error) printf '1\n' ;;
        2|warning|warn) printf '2\n' ;;
        3|info) printf '3\n' ;;
        4|status) printf '4\n' ;;
        5|debug) printf '5\n' ;;
        6|verbose) printf '6\n' ;;
        *) return 1 ;;
    esac
}

host_log_level_value() {
    local parsed
    if parsed="$(parse_openavb_log_level "$HOST_LOG_LEVEL" 2>/dev/null)"; then
        printf '%s\n' "$parsed"
    else
        printf '4\n'
    fi
}

host_log_includes_info() {
    local level
    level="$(host_log_level_value)"
    [[ "$level" -ge 3 ]]
}

phc2sys_enabled() {
    case "${RUN_PHC2SYS,,}" in
        1|true|yes|on)
            return 0
            ;;
        auto)
            [[ -n "$PHC2SYS_BIN" ]]
            return
            ;;
        *)
            return 1
            ;;
    esac
}

persistent_infra_enabled() {
    case "${PERSIST_MILAN_INFRA,,}" in
        1|true|yes|on)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

persistent_infra_running() {
    session_up "$SESS_SHAPER" || session_up "$SESS_GPTP" || session_up "$SESS_PHC2SYS" || \
        session_up "$SESS_MRPD" || session_up "$SESS_MRPD_WATCH" || session_up "$SESS_MAAP"
}

clear_infra_state() {
    rm -f "$INFRA_STATE_FILE"
}

compute_infra_signature() {
    local nic_tune_active=0
    local phc2sys_active=0

    nic_tune_enabled && nic_tune_active=1
    phc2sys_enabled && phc2sys_active=1

    cat <<EOF
version=1
run_gptp=$RUN_GPTP
run_phc2sys=$phc2sys_active
run_mrpd=$RUN_MRPD
run_mrpd_watch=$RUN_MRPD_WATCH
run_maap=$RUN_MAAP
run_shaper=$RUN_SHAPER
daemon_iface=$IFACE_DAEMONS
ring_iface=$RING_IFACE
nic_tune=$nic_tune_active
ring_rx_target=$RING_RX_TARGET
ring_tx_target=$RING_TX_TARGET
combined_queues=$NIC_COMBINED_QUEUES
coalesce_rx_usecs=$NIC_COALESCE_RX_USECS
coalesce_tx_usecs=$NIC_COALESCE_TX_USECS
gptp_bin=$GPTP_BIN
gptp_cwd=$GPTP_CWD
gptp_args=$GPTP_ARGS
phc2sys_bin=$PHC2SYS_BIN
phc2sys_args=$PHC2SYS_ARGS
mrpd_bin=$MRPD_BIN
mrpd_args=$MRPD_ARGS
mrpd_log_flags=$MRPD_LOG_FLAGS
maap_bin=$MAAP_BIN
maap_args=$MAAP_ARGS
shaper_bin=$SHAPER_BIN
shaper_args=$SHAPER_ARGS
shaper_init_iface=$SHAPER_INIT_IFACE
shaper_link_speed_mbps=$SHAPER_LINK_SPEED_MBPS
shaper_skip_root_qdisc=$SHAPER_SKIP_ROOT_QDISC
shaper_classa_parent=$SHAPER_CLASSA_PARENT
shaper_classb_parent=$SHAPER_CLASSB_PARENT
shaper_classa_handle=$SHAPER_CLASSA_HANDLE
shaper_classb_handle=$SHAPER_CLASSB_HANDLE
shaper_classa_qdisc=$SHAPER_CLASSA_QDISC
shaper_classb_qdisc=$SHAPER_CLASSB_QDISC
shaper_classa_cbs_offload=$SHAPER_CLASSA_CBS_OFFLOAD
shaper_classb_cbs_offload=$SHAPER_CLASSB_CBS_OFFLOAD
shaper_classa_etf_delta_ns=$SHAPER_CLASSA_ETF_DELTA_NS
shaper_classb_etf_delta_ns=$SHAPER_CLASSB_ETF_DELTA_NS
shaper_classa_etf_offload=$SHAPER_CLASSA_ETF_OFFLOAD
shaper_classb_etf_offload=$SHAPER_CLASSB_ETF_OFFLOAD
shaper_classa_etf_skip_sock_check=$SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK
shaper_classb_etf_skip_sock_check=$SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK
shaper_mqprio_hw=$SHAPER_MQPRIO_HW
shaper_tc_map=$SHAPER_TC_MAP
shaper_egress_qmap=$SHAPER_EGRESS_QMAP
shaper_classa_qmap=$SHAPER_CLASSA_QMAP
shaper_classb_qmap=$SHAPER_CLASSB_QMAP
shaper_default_qmap=$SHAPER_DEFAULT_QMAP
shaper_gptp_qmap=$SHAPER_GPTP_QMAP
EOF
}

infra_state_matches() {
    local desired current
    [[ -f "$INFRA_STATE_FILE" ]] || return 1
    desired="$(compute_infra_signature)"
    current="$(cat "$INFRA_STATE_FILE" 2>/dev/null || true)"
    [[ -n "$current" && "$desired" == "$current" ]]
}

write_infra_state() {
    mkdir -p "$(dirname "$INFRA_STATE_FILE")"
    compute_infra_signature >"$INFRA_STATE_FILE"
}

nic_tune_enabled() {
    case "${NIC_TUNE,,}" in
        1|true|yes|on)
            return 0
            ;;
        auto)
            [[ -n "$ETHTOOL_BIN" ]]
            return
            ;;
        *)
            return 1
            ;;
    esac
}

apply_stock_igb_shaper_defaults() {
    [[ -z "$SHAPER_LINK_SPEED_MBPS" ]] && SHAPER_LINK_SPEED_MBPS="1000"
    [[ -z "$SHAPER_EGRESS_QMAP" ]] && SHAPER_EGRESS_QMAP="1"
    [[ -z "$SHAPER_GPTP_QMAP" ]] && SHAPER_GPTP_QMAP="0"
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

apply_milan_runtime_defaults() {
    IFACE_HOST="$STOCK_IGB_IFACE_HOST"
    IFACE_AVDECC="$STOCK_IGB_IFACE_AVDECC"
    IFACE_DAEMONS="$STOCK_IGB_IFACE_DAEMONS"
    [[ -z "${RING_IFACE:-}" || "$RING_IFACE" == "$IFACE_DAEMONS" ]] && RING_IFACE="$STOCK_IGB_IFACE_DAEMONS"
    apply_stock_igb_shaper_defaults
}

apply_milan_tonegen_timing_defaults() {
    [[ "$TONEGEN_TX_MIN_LEAD_USEC" == "250" ]] && TONEGEN_TX_MIN_LEAD_USEC="$STOCK_IGB_AAF_TX_MIN_LEAD_USEC"
    [[ "$CRF_TONEGEN_LAUNCH_LEAD_USEC" == "1200" ]] && CRF_TONEGEN_LAUNCH_LEAD_USEC="$STOCK_IGB_CRF_LAUNCH_LEAD_USEC"
}

apply_stack_profile() {
    local profile="${STACK_PROFILE,,}"
    ACTIVE_ENDPOINT_INI="$ENDPOINT_INI"
    ACTIVE_ENDPOINT_SAVE_INI="$ENDPOINT_SAVE_INI"
    ACTIVE_AVDECC_INI="$AVDECC_INI"

    case "$profile" in
        split32)
            [[ "$CRF_TONEGEN_LAUNCH_LEAD_USEC" == "1200" ]] && CRF_TONEGEN_LAUNCH_LEAD_USEC="$STOCK_IGB_CRF_LAUNCH_LEAD_USEC"
            apply_milan_runtime_defaults
            INI_FILES="$AUDIO_INI_0,ifname=$IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=$BUS32_AAF_DEFERRED_START_BASE_USEC,map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=0,map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$AUDIO_INI_1,ifname=$IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=$((BUS32_AAF_DEFERRED_START_BASE_USEC + BUS32_AAF_DEFERRED_START_STEP_USEC)),map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=$STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC,map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$AUDIO_INI_2,ifname=$IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=$((BUS32_AAF_DEFERRED_START_BASE_USEC + (BUS32_AAF_DEFERRED_START_STEP_USEC * 2))),map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=$((STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC * 2)),map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$AUDIO_INI_3,ifname=$IFACE_HOST,max_transit_usec=$STOCK_IGB_AAF_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$STOCK_IGB_AAF_MAX_TRANSMIT_DEFICIT_USEC,fixed_timestamp=1,intf_nv_fixed_ts_runtime_lead_usec=0,deferred_start=selected_clock,deferred_start_stable_usec=$((BUS32_AAF_DEFERRED_START_BASE_USEC + (BUS32_AAF_DEFERRED_START_STEP_USEC * 3))),map_nv_tx_min_lead_usec=$STOCK_IGB_AAF_TX_MIN_LEAD_USEC,map_nv_tx_launch_skew_usec=$((STOCK_IGB_AAF_LAUNCH_SKEW_STEP_USEC * 3)),map_nv_selected_clock_mute_usec=$TONEGEN_SELECTED_CLOCK_MUTE_USEC,map_nv_selected_clock_trim_usec=$BUS32_AAF_SELECTED_CLOCK_TRIM_USEC \
$SPLIT32_CRF_TALKER_INI,ifname=$IFACE_HOST,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC,map_nv_crf_diag_enable=$BUS32_CRF_DIAG_ENABLE,map_nv_crf_diag_log_every_packets=$BUS32_CRF_DIAG_LOG_EVERY_PACKETS,map_nv_crf_diag_jitter_thresh_ns=$BUS32_CRF_DIAG_JITTER_THRESH_NS \
$SPLIT32_CRF_LISTENER_INI,ifname=$IFACE_HOST"
            ;;
        tonegen)
            apply_milan_tonegen_timing_defaults
            apply_milan_runtime_defaults
            INI_FILES="${TONEGEN_DEFAULT_INI_FILES% $CRF_LISTENER_INI} $CRF_LISTENER_INI,ifname=$IFACE_HOST"
            ;;
        wav)
            apply_milan_tonegen_timing_defaults
            apply_milan_runtime_defaults
            INI_FILES="${WAV_TONEGEN_DEFAULT_INI_FILES/ $CRF_LISTENER_INI / $CRF_LISTENER_INI,ifname=$IFACE_HOST }"
            ;;
        crf)
            [[ "$CRF_TONEGEN_LAUNCH_LEAD_USEC" == "1200" ]] && CRF_TONEGEN_LAUNCH_LEAD_USEC="$STOCK_IGB_CRF_LAUNCH_LEAD_USEC"
            apply_milan_runtime_defaults
            INI_FILES="$CRF_TONEGEN_INI,ifname=$IFACE_HOST,max_transit_usec=$CRF_TONEGEN_MAX_TRANSIT_USEC,max_transmit_deficit_usec=$CRF_TONEGEN_MAX_TRANSMIT_DEFICIT_USEC,map_nv_tx_rate=$CRF_TONEGEN_TX_RATE,map_nv_crf_launch_lead_usec=$CRF_TONEGEN_LAUNCH_LEAD_USEC $CRF_LISTENER_INI,ifname=$IFACE_HOST"
            ;;
        custom)
            : # respect INI_FILES exactly as supplied
            ;;
        *)
            echo "ERROR: Unsupported STACK_PROFILE='$STACK_PROFILE'. Use split32|tonegen|wav|crf|custom." >&2
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

extract_ini_section_key() {
    local ini_path="$1"
    local section="$2"
    local key="$3"
    awk -F= -v section="$section" -v key="$key" '
        BEGIN {
            in_section = 0
        }
        /^[[:space:]]*[#;]/ { next }
        /^[[:space:]]*\[/ {
            line = $0
            gsub(/^[[:space:]]*\[/, "", line)
            gsub(/\][[:space:]]*$/, "", line)
            in_section = (line == section)
            next
        }
        in_section {
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

extract_ini_entry_override() {
    local ini_entry="$1"
    local key="$2"
    local suffix token name value

    [[ "$ini_entry" == *,* ]] || return 1
    suffix="${ini_entry#*,}"
    IFS=',' read -r -a _ini_override_tokens <<< "$suffix"
    for token in "${_ini_override_tokens[@]}"; do
        [[ "$token" == *=* ]] || continue
        name="${token%%=*}"
        value="${token#*=}"
        if [[ "$name" == "$key" ]]; then
            printf '%s\n' "$value"
            return 0
        fi
    done
    return 1
}

extract_ini_entry_value() {
    local ini_entry="$1"
    local ini_path="$2"
    local key="$3"
    local value

    value="$(extract_ini_entry_override "$ini_entry" "$key" || true)"
    if [[ -n "$value" ]]; then
        printf '%s\n' "$value"
        return 0
    fi

    extract_ini_key "$ini_path" "$key"
}

aaf_packet_sample_bytes() {
    local audio_type="${1,,}"
    local audio_bit_depth="$2"

    if [[ "$audio_type" == "float" ]]; then
        [[ "$audio_bit_depth" == "32" ]] || return 1
        printf '4\n'
        return 0
    fi

    case "$audio_bit_depth" in
        32) printf '4\n' ;;
        24) printf '3\n' ;;
        16) printf '2\n' ;;
        *) return 1 ;;
    esac
}

compute_aaf_talker_shaper_values() {
    local ini_entry="$1"
    local ini_path="$2"
    local sr_class="$3"
    local max_interval_frames="$4"
    local tx_rate audio_rate audio_channels audio_bit_depth audio_type
    local packet_sample_bytes frames_per_packet payload_size max_frame_size
    local measurement_interval bandwidth_bps

    tx_rate="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_nv_tx_rate" || true)"
    [[ -z "$tx_rate" ]] && tx_rate="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_nv_tx_interval" || true)"
    [[ -z "$tx_rate" ]] && tx_rate="4000"

    audio_rate="$(extract_ini_entry_value "$ini_entry" "$ini_path" "intf_nv_audio_rate" || true)"
    audio_channels="$(extract_ini_entry_value "$ini_entry" "$ini_path" "intf_nv_audio_channels" || true)"
    audio_bit_depth="$(extract_ini_entry_value "$ini_entry" "$ini_path" "intf_nv_audio_bit_depth" || true)"
    audio_type="$(extract_ini_entry_value "$ini_entry" "$ini_path" "intf_nv_audio_type" || true)"
    [[ -z "$audio_type" ]] && audio_type="int"

    [[ "$tx_rate" =~ ^[0-9]+$ && "$tx_rate" -gt 0 ]] || return 1
    [[ "$audio_rate" =~ ^[0-9]+$ && "$audio_rate" -gt 0 ]] || return 1
    [[ "$audio_channels" =~ ^[0-9]+$ && "$audio_channels" -gt 0 ]] || return 1
    [[ "$audio_bit_depth" =~ ^[0-9]+$ && "$audio_bit_depth" -gt 0 ]] || return 1

    packet_sample_bytes="$(aaf_packet_sample_bytes "$audio_type" "$audio_bit_depth" || true)"
    [[ "$packet_sample_bytes" =~ ^[0-9]+$ && "$packet_sample_bytes" -gt 0 ]] || return 1

    frames_per_packet=$(( audio_rate / tx_rate ))
    if (( audio_rate % tx_rate != 0 )); then
        frames_per_packet=$((frames_per_packet + 1))
    fi
    (( frames_per_packet > 0 )) || return 1

    payload_size=$((frames_per_packet * packet_sample_bytes * audio_channels))
    # AAF talker maxFrameSize = payload + 24 bytes AVTP/AAF header, then the
    # endpoint adds 18 bytes L2/FCS overhead before reserving shaper bandwidth.
    max_frame_size=$((payload_size + 24 + 18))
    if [[ "$sr_class" == "A" ]]; then
        measurement_interval=125
    else
        measurement_interval=250
    fi
    bandwidth_bps=$(( ((1000000 * max_frame_size * 8 * max_interval_frames) + measurement_interval - 1) / measurement_interval ))

    printf '%s %s\n' "$bandwidth_bps" "$max_frame_size"
}

compute_crf_talker_shaper_values() {
    local ini_entry="$1"
    local ini_path="$2"
    local sr_class="$3"
    local max_interval_frames="$4"
    local base_freq timestamp_interval timestamps_per_pdu tx_rate
    local measurement_interval max_frame_size bandwidth_bps denominator

    base_freq="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_nv_crf_base_freq" || true)"
    timestamp_interval="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_nv_crf_timestamp_interval" || true)"
    timestamps_per_pdu="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_nv_crf_timestamps_per_pdu" || true)"
    tx_rate="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_nv_tx_rate" || true)"
    [[ -z "$base_freq" ]] && base_freq="48000"
    [[ -z "$timestamp_interval" ]] && timestamp_interval="96"
    [[ -z "$timestamps_per_pdu" ]] && timestamps_per_pdu="1"

    [[ "$base_freq" =~ ^[0-9]+$ && "$base_freq" -gt 0 ]] || return 1
    [[ "$timestamp_interval" =~ ^[0-9]+$ && "$timestamp_interval" -gt 0 ]] || return 1
    [[ "$timestamps_per_pdu" =~ ^[0-9]+$ && "$timestamps_per_pdu" -gt 0 ]] || return 1

    if [[ -z "$tx_rate" ]]; then
        denominator=$((timestamp_interval * timestamps_per_pdu))
        (( denominator > 0 )) || return 1
        tx_rate=$(( (base_freq + (denominator / 2)) / denominator ))
    fi
    [[ "$tx_rate" =~ ^[0-9]+$ && "$tx_rate" -gt 0 ]] || return 1

    if [[ "$sr_class" == "A" ]]; then
        measurement_interval=125
    else
        measurement_interval=250
    fi

    # CRF talker maxFrameSize = packed AVTP CRF header (20 bytes) plus N
    # timestamps (8 bytes each), then endpoint adds 18 bytes L2/FCS overhead.
    max_frame_size=$((20 + (timestamps_per_pdu * 8) + 18))
    bandwidth_bps=$(( ((1000000 * max_frame_size * 8 * max_interval_frames) + measurement_interval - 1) / measurement_interval ))

    printf '%s %s\n' "$bandwidth_bps" "$max_frame_size"
}

compute_ini_entry_shaper_values() {
    local ini_entry="$1"
    local ini_path="$2"
    local role map_fn sr_class max_interval_frames values

    role="$(extract_ini_entry_value "$ini_entry" "$ini_path" "role" || true)"
    role="${role,,}"
    [[ "$role" == "talker" ]] || return 1

    sr_class="$(extract_ini_entry_value "$ini_entry" "$ini_path" "sr_class" || true)"
    sr_class="${sr_class^^}"
    [[ "$sr_class" == "A" || "$sr_class" == "B" ]] || return 2

    max_interval_frames="$(extract_ini_entry_value "$ini_entry" "$ini_path" "max_interval_frames" || true)"
    [[ -z "$max_interval_frames" ]] && max_interval_frames="1"
    [[ "$max_interval_frames" =~ ^[0-9]+$ && "$max_interval_frames" -gt 0 ]] || return 2

    map_fn="$(extract_ini_entry_value "$ini_entry" "$ini_path" "map_fn" || true)"
    case "$map_fn" in
        openavbMapAVTPAudioInitialize)
            values="$(compute_aaf_talker_shaper_values "$ini_entry" "$ini_path" "$sr_class" "$max_interval_frames" || true)"
            [[ -n "$values" ]] || return 2
            printf '%s %s %s\n' "$sr_class" ${values}
            ;;
        openavbMapCrfInitialize)
            values="$(compute_crf_talker_shaper_values "$ini_entry" "$ini_path" "$sr_class" "$max_interval_frames" || true)"
            [[ -n "$values" ]] || return 2
            printf '%s %s %s\n' "$sr_class" ${values}
            ;;
        *)
            return 2
            ;;
    esac
}

compute_shaper_presets_from_inis() {
    local log_result="${1:-1}"
    local ini_entry ini_path role sr_class values bandwidth max_frame
    local classa_bw=0 classa_max_frame=0 classa_talkers=0 classa_disabled=0
    local classb_bw=0 classb_max_frame=0 classb_talkers=0 classb_disabled=0
    local -a disabled_notes=()

    SHAPER_PRESET_CLASSA_BW=""
    SHAPER_PRESET_CLASSA_MAX_FRAME=""
    SHAPER_PRESET_CLASSB_BW=""
    SHAPER_PRESET_CLASSB_MAX_FRAME=""
    SHAPER_PRESET_SUMMARY=""

    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        role="$(extract_ini_entry_value "$ini_entry" "$ini_path" "role" || true)"
        role="${role,,}"
        [[ "$role" == "talker" ]] || continue

        sr_class="$(extract_ini_entry_value "$ini_entry" "$ini_path" "sr_class" || true)"
        sr_class="${sr_class^^}"
        [[ "$sr_class" == "A" || "$sr_class" == "B" ]] || continue

        if ! values="$(compute_ini_entry_shaper_values "$ini_entry" "$ini_path" 2>/dev/null)"; then
            if [[ "$sr_class" == "A" ]]; then
                classa_disabled=1
            else
                classb_disabled=1
            fi
            disabled_notes+=("${sr_class}:${ini_path##*/}")
            continue
        fi

        read -r sr_class bandwidth max_frame <<< "$values"
        if [[ "$sr_class" == "A" ]]; then
            classa_bw=$((classa_bw + bandwidth))
            (( max_frame > classa_max_frame )) && classa_max_frame="$max_frame"
            classa_talkers=$((classa_talkers + 1))
        else
            classb_bw=$((classb_bw + bandwidth))
            (( max_frame > classb_max_frame )) && classb_max_frame="$max_frame"
            classb_talkers=$((classb_talkers + 1))
        fi
    done

    if (( classa_talkers > 0 && classa_disabled == 0 )); then
        SHAPER_PRESET_CLASSA_BW="$classa_bw"
        SHAPER_PRESET_CLASSA_MAX_FRAME="$classa_max_frame"
        SHAPER_PRESET_SUMMARY+="ClassA=${classa_bw}bps/${classa_max_frame}B "
    fi
    if (( classb_talkers > 0 && classb_disabled == 0 )); then
        SHAPER_PRESET_CLASSB_BW="$classb_bw"
        SHAPER_PRESET_CLASSB_MAX_FRAME="$classb_max_frame"
        SHAPER_PRESET_SUMMARY+="ClassB=${classb_bw}bps/${classb_max_frame}B "
    fi
    SHAPER_PRESET_SUMMARY="${SHAPER_PRESET_SUMMARY% }"

    if (( log_result != 0 && ${#disabled_notes[@]} > 0 )); then
        log_system_note "Shaper presets disabled for unsupported talker INIs: ${disabled_notes[*]}"
    fi
    if (( log_result != 0 )) && [[ -n "$SHAPER_PRESET_SUMMARY" ]]; then
        log_system_note "Computed expected class totals from active INIs: $SHAPER_PRESET_SUMMARY"
    elif (( log_result != 0 )); then
        log_system_note "No expected class totals computed from active INIs; runtime reservations will shape classes live."
    fi
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

            # Listener stream IDs with an omitted or all-zero source MAC are
            # intentionally unresolved placeholders. AVDECC/controller bind will
            # supply the remote talker stream ID later, so they must not be
            # rejected up front as local tuple collisions.
            if [[ "$role" == "listener" && ( -z "$stream_addr" || "$stream_addr" == "00:00:00:00:00:00" ) ]]; then
                :
            elif [[ -n "$stream_addr" ]]; then
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
            if [[ "$dest" == "00:00:00:00:00:00" ]]; then
                continue
            fi
            if [[ -n "${talker_dest_owner[$dest]:-}" && "${talker_dest_owner[$dest]}" != "$ini_path" ]]; then
                echo "ERROR: Duplicate talker dest_addr=$dest in $ini_path and ${talker_dest_owner[$dest]}" >&2
                exit 1
            fi
            talker_dest_owner[$dest]="$ini_path"
        fi
    done
}

validate_dynamic_addressing_requirements() {
    local maap_port
    local maap_enabled=0
    local ini_entry ini_path role uid stream_addr dest deferred_start
    local -a talkers_need_maap=()
    local -a deferred_talkers_need_maap=()
    local -a listeners_need_bind=()
    local -a listeners_need_dest=()

    maap_port="$(extract_ini_section_key "$ACTIVE_ENDPOINT_INI" "maap" "port" || true)"
    if [[ "$maap_port" =~ ^[0-9]+$ ]] && (( maap_port > 0 )); then
        maap_enabled=1
    fi

    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        role="$(extract_ini_key "$ini_path" "role" || true)"
        role="${role,,}"
        uid="$(extract_ini_key "$ini_path" "stream_uid" || true)"
        deferred_start=""
        if [[ "$ini_entry" == *,deferred_start=* ]]; then
            deferred_start="${ini_entry#*,deferred_start=}"
            deferred_start="${deferred_start%%,*}"
            deferred_start="${deferred_start,,}"
        fi

        stream_addr="$(extract_ini_key "$ini_path" "stream_addr" || true)"
        stream_addr="${stream_addr,,}"
        dest="$(extract_ini_key "$ini_path" "dest_addr" || true)"
        dest="${dest,,}"

        if [[ "$role" == "talker" && ( -z "$dest" || "$dest" == "00:00:00:00:00:00" ) ]]; then
            talkers_need_maap+=("${ini_path##*/}:${uid:-?}")
            if [[ "$deferred_start" == "selected_clock" || "$deferred_start" == "selected-clock" ]]; then
                deferred_talkers_need_maap+=("${ini_path##*/}:${uid:-?}")
            fi
        fi

        if [[ "$role" == "listener" && ( -z "$stream_addr" || "$stream_addr" == "00:00:00:00:00:00" ) ]]; then
            listeners_need_bind+=("${ini_path##*/}:${uid:-?}")
        fi

        if [[ "$role" == "listener" && ( -z "$dest" || "$dest" == "00:00:00:00:00:00" ) ]]; then
            listeners_need_dest+=("${ini_path##*/}:${uid:-?}")
        fi
    done

    if (( ${#talkers_need_maap[@]} > 0 )) && (( ! maap_enabled )); then
        echo "ERROR: Talker INIs rely on dynamic dest_addr assignment, but [maap] port is disabled in $ACTIVE_ENDPOINT_INI." >&2
        echo "Affected streams: ${talkers_need_maap[*]}" >&2
        echo "Enable MAAP in endpoint.ini or set explicit talker dest_addr values before starting." >&2
        exit 1
    fi

    if (( ${#talkers_need_maap[@]} > 0 )); then
        echo "Notice: dynamic talker dest_addr via MAAP for ${talkers_need_maap[*]} (endpoint [maap] port=${maap_port:-0})."
    fi

    if (( ${#deferred_talkers_need_maap[@]} > 0 )); then
        echo "Notice: deferred selected_clock talkers will keep dest_addr unresolved until their clock-stable start is released: ${deferred_talkers_need_maap[*]}."
    fi

    if (( ${#listeners_need_bind[@]} > 0 )); then
        echo "Notice: unresolved listener stream_addr for ${listeners_need_bind[*]}; controller/AVDECC bind must resolve these listeners."
    fi

    if (( ${#listeners_need_dest[@]} > 0 )); then
        echo "Notice: unresolved listener dest_addr for ${listeners_need_dest[*]}; SRP/AVDECC must supply the destination MAC or these listeners need an explicit dest_addr."
    fi
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
    local ini_entry ini_path role uid dest deferred_start initial_state
    for ini_entry in "${INI_LIST_ARR[@]}"; do
        ini_path="$(resolve_ini_path "${ini_entry%%,*}")"
        role="$(extract_ini_key "$ini_path" "role" || true)"
        role="${role,,}"
        [[ "$role" != "talker" ]] && continue
        deferred_start=""
        if [[ "$ini_entry" == *,deferred_start=* ]]; then
            deferred_start="${ini_entry#*,deferred_start=}"
            deferred_start="${deferred_start%%,*}"
            deferred_start="${deferred_start,,}"
        fi
        if [[ "$deferred_start" == "selected_clock" || "$deferred_start" == "selected-clock" ]]; then
            continue
        fi
        initial_state="$(extract_ini_key "$ini_path" "initial_state" || true)"
        initial_state="${initial_state,,}"
        if [[ "$initial_state" == "stopped" ]]; then
            continue
        fi
        uid="$(extract_ini_key "$ini_path" "stream_uid" || true)"
        [[ -z "$uid" ]] && continue
        dest="$(extract_ini_key "$ini_path" "dest_addr" || true)"
        dest="${dest,,}"
        # No explicit dest_addr, or an all-zero placeholder, means startup uses
        # MAAP to provide the final multicast destination before controller connect.
        # Only include talkers that can actually run before AVDECC is up. Deferred
        # selected_clock talkers and initial_state=stopped talkers resolve later.
        if [[ -z "$dest" || "$dest" == "00:00:00:00:00:00" ]]; then
            EXPECTED_MAAP_UIDS+=("$uid")
        fi
    done
}

wait_for_host_stream_registration() {
    local timeout="$STREAM_READY_WAIT_SEC"
    host_log_includes_info || return 0
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
            return 0
        fi
        progress_tick
        sleep 1
    done

    log_startup_note "Host stream readiness timeout after ${timeout}s. Missing UIDs by register-pattern: ${missing[*]} (running-clients=${running_clients}/${expected_count})"
    return 0
}

wait_for_host_maap_ready() {
    local timeout="$MAAP_READY_WAIT_SEC"
    local expected="${#EXPECTED_MAAP_UIDS[@]}"
    [[ "$timeout" -le 0 || "$expected" -le 0 ]] && return 0

    local deadline=$((SECONDS + timeout))
    local count_alloc=0
    local count_shaper=0
    while (( SECONDS < deadline )); do
        count_alloc="$(awk '
            /Endpoint MAAP] (INFO|WARNING): Allocated MAAP address/ { c++ }
            END { print c + 0 }
        ' "$HOST_LOG" 2>/dev/null || echo 0)"
        if [[ "$RUN_SHAPER" == "1" ]]; then
            count_shaper="$(awk '
                /SHAPER Main] INFO: The received command is "-ri / { c++ }
                END { print c + 0 }
            ' "$SHAPER_LOG" 2>/dev/null || echo 0)"
            if (( count_shaper > count_alloc )); then
                count_alloc="$count_shaper"
            fi
        fi
        if (( count_alloc >= expected )); then
            return 0
        fi
        progress_tick
        sleep 1
    done

    log_startup_note "Host MAAP readiness timeout after ${timeout}s (${count_alloc}/${expected} allocations observed)."
    log_startup_note "AVDECC may expose incomplete talker stream info until MAAP settles."
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
        if [[ -S "$IPC_ENDPOINT_SOCK" ]]; then
            if host_log_includes_info; then
                if rg -q "$marker_re" "$HOST_LOG"; then
                    return 0
                fi
            else
                return 0
            fi
        fi

        # SRP startup can race mrpd readiness. Signal a retryable status so the
        # caller can restart host without tearing down the full stack.
        if rg -q "Failed to initialize SRP|Make sure that mrpd daemon is started\\." "$HOST_LOG"; then
            log_startup_note "Host SRP initialization not ready yet. Will retry host startup. See $HOST_LOG"
            return 2
        fi

        if rg -q \
            "Failed to initialize QMgr|Failed to initialize MAAP|Failed to initialize Shaper|PTP failed to start - Exiting|Failed to start endpoint thread|connect failed .*are you running as root\\?|init failed .*driver really loaded" \
            "$HOST_LOG"; then
            progress_abort
            echo "ERROR: Host endpoint startup failure detected. See $HOST_LOG" >&2
            tail -n 80 "$HOST_LOG" >&2 || true
            return 1
        fi

        progress_tick
        sleep 1
    done

    if rg -q "Failed to initialize SRP|Make sure that mrpd daemon is started\\." "$HOST_LOG"; then
        log_startup_note "Host endpoint SRP startup not ready after ${timeout}s. Will retry host startup. See $HOST_LOG"
        return 2
    fi

    progress_abort
    echo "ERROR: Host endpoint not ready after ${timeout}s." >&2
    if [[ ! -S "$IPC_ENDPOINT_SOCK" ]]; then
        echo "Hint: endpoint IPC socket missing: $IPC_ENDPOINT_SOCK" >&2
    fi
    if host_log_includes_info; then
        echo "Hint: no endpoint readiness markers found in $HOST_LOG." >&2
    else
        echo "Hint: host log level '$HOST_LOG_LEVEL' suppresses info-level readiness markers." >&2
    fi
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
            progress_abort
            echo "ERROR: gPTP startup reported a hard failure. See $GPTP_LOG" >&2
            tail -n 80 "$GPTP_LOG" >&2 || true
            return 1
        fi
        progress_tick
        sleep 1
    done

    log_startup_note "gPTP readiness markers not observed after ${timeout}s; continuing startup."
    return 0
}

wait_for_phc2sys_ready() {
    local timeout="$PHC2SYS_READY_WAIT_SEC"
    [[ "$timeout" -le 0 ]] && return 0

    local deadline=$((SECONDS + timeout))
    while (( SECONDS < deadline )); do
        if tmux has-session -t "$SESS_PHC2SYS" 2>/dev/null && [[ -s "$PHC2SYS_LOG" ]]; then
            return 0
        fi
        progress_tick
        sleep 1
    done

    if ! tmux has-session -t "$SESS_PHC2SYS" 2>/dev/null; then
        progress_abort
        echo "ERROR: phc2sys stopped during startup. See $PHC2SYS_LOG" >&2
        tail -n 80 "$PHC2SYS_LOG" >&2 || true
        return 1
    fi

    log_startup_note "phc2sys did not emit log output after ${timeout}s; continuing startup."
    return 0
}

resolve_mrpd_query_bin() {
    if [[ -n "$MRPD_QUERY_BIN" ]]; then
        [[ -x "$MRPD_QUERY_BIN" ]] || return 1
        printf '%s\n' "$MRPD_QUERY_BIN"
        return 0
    fi

    [[ -x "$MRPQ_BIN" ]] || return 1
    printf '%s\n' "$MRPQ_BIN"
}

mrpd_srp_domain_query_ready() {
    local query_bin="$1"
    local query_output

    if ! query_output="$("$query_bin" 2>/dev/null)"; then
        return 1
    fi

    printf '%s\n' "$query_output" | rg -q 'D:C=[0-9]+,P=[0-9]+,V=[0-9A-Fa-f]{4}(,|$)'
}

wait_for_mrpd_srp_ready() {
    local timeout="$MRPD_READY_WAIT_SEC"
    local query_bin

    [[ "$RUN_MRPD" != "1" || "$timeout" -le 0 ]] && return 0

    if ! query_bin="$(resolve_mrpd_query_bin)"; then
        log_startup_note "MRPD SRP readiness helper unavailable; proceeding without S?? preflight."
        return 0
    fi

    local deadline=$((SECONDS + timeout))
    while (( SECONDS < deadline )); do
        if mrpd_srp_domain_query_ready "$query_bin"; then
            return 0
        fi

        if ! tmux has-session -t "$SESS_MRPD" 2>/dev/null && ! pgrep -f "[m]rpd .* -i $IFACE_DAEMONS" >/dev/null 2>&1; then
            progress_abort
            echo "ERROR: MRPD stopped during startup. See $MRPD_LOG" >&2
            tail -n 80 "$MRPD_LOG" >&2 || true
            return 1
        fi

        progress_tick
        sleep 1
    done

    progress_abort
    echo "ERROR: MRPD SRP domain not ready after ${timeout}s. See $MRPD_LOG" >&2
    echo "Hint: no MSRP domain response was observed from S?? queries via $query_bin." >&2
    echo "Hint: check link state, peer/controller readiness, and gPTP lock." >&2
    tail -n 80 "$MRPD_LOG" >&2 || true
    return 1
}

path_is_within_control_home() {
    local path="$1"
    [[ "$path" == "$CONTROL_HOME" || "$path" == "$CONTROL_HOME/"* ]]
}

ensure_state_ini_path() {
    local state_dir created_dir=0 created_file=0
    state_dir="$(dirname "$ACTIVE_ENDPOINT_SAVE_INI")"

    if [[ ! -d "$state_dir" ]]; then
        mkdir -p "$state_dir"
        created_dir=1
    fi
    if [[ ! -e "$ACTIVE_ENDPOINT_SAVE_INI" ]]; then
        : > "$ACTIVE_ENDPOINT_SAVE_INI"
        created_file=1
    fi

    if [[ "$EUID" -eq 0 && -n "${SUDO_USER:-}" ]] && path_is_within_control_home "$ACTIVE_ENDPOINT_SAVE_INI"; then
        (( created_dir )) && chown "$CONTROL_USER:$CONTROL_GROUP" "$state_dir" 2>/dev/null || true
        (( created_file )) && chown "$CONTROL_USER:$CONTROL_GROUP" "$ACTIVE_ENDPOINT_SAVE_INI" 2>/dev/null || true
    fi
}

read_current_ring_value() {
    local iface="$1"
    local key="$2"
    "$ETHTOOL_BIN" -g "$iface" 2>/dev/null | awk -v key="$key" '
        /^Current hardware settings:/ { in_current = 1; next }
        in_current {
            gsub(/^[[:space:]]+/, "", $0)
            if ($1 == key ":") {
                print $2
                exit
            }
        }
    '
}

read_current_channel_value() {
    local iface="$1"
    local key="$2"
    "$ETHTOOL_BIN" -l "$iface" 2>/dev/null | awk -v key="$key" '
        /^Current hardware settings:/ { in_current = 1; next }
        in_current {
            gsub(/^[[:space:]]+/, "", $0)
            if ($1 == key ":") {
                print $2
                exit
            }
        }
    '
}

read_ethtool_named_value() {
    local subcmd="$1"
    local iface="$2"
    local key="$3"
    "$ETHTOOL_BIN" "$subcmd" "$iface" 2>/dev/null | awk -F':' -v key="$key" '
        {
            lhs = $1
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", lhs)
            if (lhs == key) {
                rhs = $2
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", rhs)
                print rhs
                exit
            }
        }
    '
}

log_system_command() {
    {
        printf '$'
        printf ' %q' "$@"
        printf '\n'
        "$@"
        printf '\n'
    } >>"$SYSTEM_LOG" 2>&1
}

run_ethtool_step() {
    local label="$1"
    shift

    {
        printf '[%s]\n' "$label" >>"$SYSTEM_LOG"
        log_system_command "$ETHTOOL_BIN" "$@"
    } || {
        progress_abort
        echo "ERROR: NIC tuning step failed: $label. See $SYSTEM_LOG" >&2
        return 1
    }
}

log_system_note() {
    printf '%s\n' "$1" >>"$SYSTEM_LOG"
}

cleanup_tc_state() {
    local iface="${IFACE_DAEMONS:-}"
    [[ -z "$iface" || -z "${TC_BIN:-}" ]] && return 0

    "$TC_BIN" filter del dev "$iface" egress >/dev/null 2>&1 || true
    "$TC_BIN" filter del dev "$iface" ingress >/dev/null 2>&1 || true
    "$TC_BIN" qdisc del dev "$iface" clsact >/dev/null 2>&1 || true
}

verify_ring_buffers() {
    local current_rx current_tx
    current_rx="$(read_current_ring_value "$RING_IFACE" "RX")"
    current_tx="$(read_current_ring_value "$RING_IFACE" "TX")"

    if [[ -z "$current_rx" || -z "$current_tx" ]]; then
        progress_abort
        echo "ERROR: Unable to read ring parameters after NIC tuning for $RING_IFACE." >&2
        return 1
    fi
    if (( current_rx != RING_RX_TARGET || current_tx != RING_TX_TARGET )); then
        progress_abort
        echo "ERROR: NIC ring settings do not match target on $RING_IFACE (rx=$current_rx tx=$current_tx, expected rx=$RING_RX_TARGET tx=$RING_TX_TARGET)." >&2
        return 1
    fi
}

ensure_nic_tuning() {
    local eee_status rx_usecs tx_usecs ring_rx ring_tx
    local tso_state gso_state gro_state lro_state rxvlan_state txvlan_state
    local pause_autoneg pause_rx pause_tx combined

    if ! nic_tune_enabled; then
        [[ "${NIC_TUNE,,}" == "auto" ]] && printf "NIC Tune - skipped.\n"
        return 0
    fi

    progress_begin "NIC Tune"
    {
        echo "NIC tuning target: iface=$RING_IFACE rx=$RING_RX_TARGET tx=$RING_TX_TARGET combined=$NIC_COMBINED_QUEUES"
        echo "[before]"
        log_system_command "$ETHTOOL_BIN" --show-eee "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -c "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -g "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -k "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -a "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -l "$RING_IFACE"
    } >>"$SYSTEM_LOG" 2>&1

    eee_status="$(read_ethtool_named_value --show-eee "$RING_IFACE" "EEE status")"
    if [[ -z "$eee_status" || "${eee_status,,}" != "disabled" ]]; then
        run_ethtool_step "disable eee" --set-eee "$RING_IFACE" eee off
    else
        log_system_note "[disable eee] already disabled; skipping"
    fi

    rx_usecs="$(read_ethtool_named_value -c "$RING_IFACE" "rx-usecs")"
    tx_usecs="$(read_ethtool_named_value -c "$RING_IFACE" "tx-usecs")"
    if [[ -z "$rx_usecs" || -z "$tx_usecs" || "$rx_usecs" != "$NIC_COALESCE_RX_USECS" || "$tx_usecs" != "$NIC_COALESCE_TX_USECS" ]]; then
        run_ethtool_step "disable coalescing" -C "$RING_IFACE" rx-usecs "$NIC_COALESCE_RX_USECS" tx-usecs "$NIC_COALESCE_TX_USECS"
    else
        log_system_note "[disable coalescing] already rx-usecs=$rx_usecs tx-usecs=$tx_usecs; skipping"
    fi

    ring_rx="$(read_current_ring_value "$RING_IFACE" "RX")"
    ring_tx="$(read_current_ring_value "$RING_IFACE" "TX")"
    if [[ -z "$ring_rx" || -z "$ring_tx" || "$ring_rx" != "$RING_RX_TARGET" || "$ring_tx" != "$RING_TX_TARGET" ]]; then
        run_ethtool_step "set ring" -G "$RING_IFACE" rx "$RING_RX_TARGET" tx "$RING_TX_TARGET"
    else
        log_system_note "[set ring] already rx=$ring_rx tx=$ring_tx; skipping"
    fi

    tso_state="$(read_ethtool_named_value -k "$RING_IFACE" "tcp-segmentation-offload")"
    if [[ -z "$tso_state" || "${tso_state,,}" != "off" ]]; then
        run_ethtool_step "disable tso" -K "$RING_IFACE" tso off
    else
        log_system_note "[disable tso] already off; skipping"
    fi

    gso_state="$(read_ethtool_named_value -k "$RING_IFACE" "generic-segmentation-offload")"
    if [[ -z "$gso_state" || "${gso_state,,}" != "off" ]]; then
        run_ethtool_step "disable gso" -K "$RING_IFACE" gso off
    else
        log_system_note "[disable gso] already off; skipping"
    fi

    gro_state="$(read_ethtool_named_value -k "$RING_IFACE" "generic-receive-offload")"
    if [[ -z "$gro_state" || "${gro_state,,}" != "off" ]]; then
        run_ethtool_step "disable gro" -K "$RING_IFACE" gro off
    else
        log_system_note "[disable gro] already off; skipping"
    fi

    lro_state="$(read_ethtool_named_value -k "$RING_IFACE" "large-receive-offload")"
    if [[ -z "$lro_state" || "${lro_state,,}" != "off [fixed]" && "${lro_state,,}" != "off" ]]; then
        run_ethtool_step "disable lro" -K "$RING_IFACE" lro off
    else
        log_system_note "[disable lro] already off; skipping"
    fi

    rxvlan_state="$(read_ethtool_named_value -k "$RING_IFACE" "rx-vlan-offload")"
    if [[ -z "$rxvlan_state" || "${rxvlan_state,,}" != "off" ]]; then
        run_ethtool_step "disable rxvlan" -K "$RING_IFACE" rxvlan off
    else
        log_system_note "[disable rxvlan] already off; skipping"
    fi

    txvlan_state="$(read_ethtool_named_value -k "$RING_IFACE" "tx-vlan-offload")"
    if [[ -z "$txvlan_state" || "${txvlan_state,,}" != "off" ]]; then
        run_ethtool_step "disable txvlan" -K "$RING_IFACE" txvlan off
    else
        log_system_note "[disable txvlan] already off; skipping"
    fi

    pause_autoneg="$(read_ethtool_named_value -a "$RING_IFACE" "Autonegotiate")"
    pause_rx="$(read_ethtool_named_value -a "$RING_IFACE" "RX")"
    pause_tx="$(read_ethtool_named_value -a "$RING_IFACE" "TX")"
    if [[ -z "$pause_autoneg" || -z "$pause_rx" || -z "$pause_tx" || "${pause_autoneg,,}" != "off" || "${pause_rx,,}" != "off" || "${pause_tx,,}" != "off" ]]; then
        run_ethtool_step "disable pause frames" -A "$RING_IFACE" autoneg off rx off tx off
    else
        log_system_note "[disable pause frames] already autoneg=$pause_autoneg rx=$pause_rx tx=$pause_tx; skipping"
    fi

    combined="$(read_current_channel_value "$RING_IFACE" "Combined")"
    if [[ -z "$combined" || "$combined" != "$NIC_COMBINED_QUEUES" ]]; then
        run_ethtool_step "set combined queues" -L "$RING_IFACE" combined "$NIC_COMBINED_QUEUES"
    else
        log_system_note "[set combined queues] already combined=$combined; skipping"
    fi

    verify_ring_buffers

    {
        echo "[after]"
        log_system_command "$ETHTOOL_BIN" --show-eee "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -c "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -g "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -k "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -a "$RING_IFACE"
        log_system_command "$ETHTOOL_BIN" -l "$RING_IFACE"
    } >>"$SYSTEM_LOG" 2>&1

    pause_autoneg="$(read_ethtool_named_value -a "$RING_IFACE" "Autonegotiate")"
    pause_rx="$(read_ethtool_named_value -a "$RING_IFACE" "RX")"
    pause_tx="$(read_ethtool_named_value -a "$RING_IFACE" "TX")"
    if [[ -n "$pause_autoneg" && -n "$pause_rx" && -n "$pause_tx" && ( "${pause_autoneg,,}" != "off" || "${pause_rx,,}" != "off" || "${pause_tx,,}" != "off" ) ]]; then
        log_system_note "WARNING: pause frames remain autoneg=$pause_autoneg rx=$pause_rx tx=$pause_tx on $RING_IFACE after NIC tuning."
    fi

    progress_finish
}

run_pre_start_checks() {
    ensure_state_ini_path
    ensure_nic_tuning
}

require_files() {
    apply_stack_profile
    reorder_crf_inis_last
    build_ini_args
    validate_stream_ids_and_dests
    validate_dynamic_addressing_requirements
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

    compute_shaper_presets_from_inis
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
    if phc2sys_enabled && [[ -z "$PHC2SYS_BIN" || ! -x "$PHC2SYS_BIN" ]]; then
        echo "Missing executable: phc2sys (set PHC2SYS_BIN or RUN_PHC2SYS=0)" >&2
        missing=1
    fi
    if nic_tune_enabled && [[ -z "$ETHTOOL_BIN" || ! -x "$ETHTOOL_BIN" ]]; then
        echo "Missing executable: ethtool (set ETHTOOL_BIN or NIC_TUNE=0)" >&2
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
    if phc2sys_enabled && [[ ! -e "$PHC2SYS_SOURCE" ]]; then
        echo "Missing PTP source device: $PHC2SYS_SOURCE" >&2
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
    local i

    tmux has-session -t "$sess" 2>/dev/null || return 0

    tmux send-keys -t "$sess" C-c 2>/dev/null || true
    for ((i=0; i<TMUX_STOP_GRACE_SEC; i++)); do
        tmux has-session -t "$sess" 2>/dev/null || return 0
        sleep 1
    done

    tmux kill-session -t "$sess" 2>/dev/null || true
}

session_up() {
    local sess="$1"
    tmux has-session -t "$sess" 2>/dev/null
}

reset_log_file() {
    local log_path="$1"
    mkdir -p "$(dirname "$log_path")"
    : >"$log_path"
}

wait_for_process_name_exit() {
    local proc_name="$1"
    local i

    for ((i=0; i<PROCESS_STOP_WAIT_SEC; i++)); do
        pgrep -x "$proc_name" >/dev/null 2>&1 || return 0
        sleep 1
    done

    ! pgrep -x "$proc_name" >/dev/null 2>&1
}

terminate_process_name() {
    local proc_name="$1"

    pgrep -x "$proc_name" >/dev/null 2>&1 || return 0

    pkill -INT -x "$proc_name" 2>/dev/null || true
    wait_for_process_name_exit "$proc_name" && return 0

    pkill -TERM -x "$proc_name" 2>/dev/null || true
    wait_for_process_name_exit "$proc_name" && return 0

    pkill -KILL -x "$proc_name" 2>/dev/null || true
    wait_for_process_name_exit "$proc_name" && return 0

    return 1
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

print_kv() {
    printf "  %-13s %s\n" "$1:" "$2"
}

print_sub_kv() {
    printf "    %-11s %s\n" "$1" "$2"
}

print_component_status() {
    local label="$1"
    local sess="$2"
    local proc_pattern="${3:-}"
    local session_state="down"
    local proc_state="n/a"
    local pids="-"

    tmux has-session -t "$sess" 2>/dev/null && session_state="up"
    if [[ -n "$proc_pattern" ]]; then
        pids="$(pgrep -f "$proc_pattern" 2>/dev/null | paste -sd, -)" || true
        if [[ -n "$pids" ]]; then
            proc_state="up"
        else
            proc_state="down"
            pids="-"
        fi
    fi

    printf "    %-10s session=%-4s proc=%-4s pids=%s\n" "$label" "$session_state" "$proc_state" "$pids"
}

stop_profile_stack() {
    stop_session "$SESS_HOST"
    if [[ "$HOST_STOP_SETTLE_SEC" -gt 0 ]]; then
        sleep "$HOST_STOP_SETTLE_SEC"
    fi
    stop_session "$SESS_AVDECC"

    # Also stop non-tmux leftovers to keep stream-layer runs deterministic.
    pkill -f "[o]penavb_avdecc -I $IFACE_AVDECC" 2>/dev/null || true
    pkill -f "[o]penavb_host -I $IFACE_HOST" 2>/dev/null || true
}

stop_all_stack() {
    stop_profile_stack
    stop_session "$SESS_MAAP"
    stop_session "$SESS_MRPD_WATCH"
    stop_session "$SESS_MRPD"
    stop_session "$SESS_SHAPER"
    stop_session "$SESS_PHC2SYS"
    stop_session "$SESS_GPTP"

    # Also stop non-tmux leftovers to keep runs deterministic.
    pkill -f "[m]aap_daemon -i $IFACE_DAEMONS" 2>/dev/null || true
    pkill -f "[m]rpd .* -i $IFACE_DAEMONS" 2>/dev/null || true
    terminate_process_name "daemon_cl" || true
    terminate_process_name "phc2sys" || true
    pkill -f "[s]haper_daemon" 2>/dev/null || true
    cleanup_tc_state
    clear_infra_state
}

stop_stack() {
    if persistent_infra_enabled; then
        stop_profile_stack
    else
        stop_all_stack
    fi

    if [[ "$POST_STOP_SETTLE_SEC" -gt 0 ]]; then
        sleep "$POST_STOP_SETTLE_SEC"
    fi
}

start_stack() {
    local profile="${STACK_PROFILE,,}"
    local ini_entry
    local reuse_persistent_infra=0
    local infra_recycle_reason=""

    require_files
    build_mrpd_profile_if_requested
    require_bins
    if persistent_infra_enabled; then
        if persistent_infra_running; then
            if infra_state_matches; then
                reuse_persistent_infra=1
                stop_profile_stack
            else
                infra_recycle_reason="Persistent Milan infra does not match requested run config; recycling seeded infrastructure."
                stop_all_stack
            fi
        else
            clear_infra_state
            stop_profile_stack
        fi
    else
        stop_all_stack
    fi

    if [[ "$POST_STOP_SETTLE_SEC" -gt 0 ]]; then
        sleep "$POST_STOP_SETTLE_SEC"
    fi

    cleanup_ipc_paths

    mkdir -p "$LOG_DIR"
    reset_log_file "$SYSTEM_LOG"
    reset_log_file "$HOST_LOG"
    reset_log_file "$AVDECC_LOG"
    if persistent_infra_enabled && session_up "$SESS_GPTP"; then
        reset_log_file "$GPTP_LOG"
    else
        rm -f "$GPTP_LOG"
    fi
    if persistent_infra_enabled && session_up "$SESS_PHC2SYS"; then
        reset_log_file "$PHC2SYS_LOG"
    else
        rm -f "$PHC2SYS_LOG"
    fi
    if persistent_infra_enabled && session_up "$SESS_SHAPER"; then
        reset_log_file "$SHAPER_LOG"
    else
        rm -f "$SHAPER_LOG"
    fi
    if persistent_infra_enabled && session_up "$SESS_MRPD"; then
        reset_log_file "$MRPD_LOG"
    else
        rm -f "$MRPD_LOG"
    fi
    if persistent_infra_enabled && session_up "$SESS_MRPD_WATCH"; then
        reset_log_file "$MRPD_TALKER_FAILED_LOG"
    else
        rm -f "$MRPD_TALKER_FAILED_LOG"
    fi
    if persistent_infra_enabled && session_up "$SESS_MAAP"; then
        reset_log_file "$MAAP_LOG"
    else
        rm -f "$MAAP_LOG"
    fi

    if ! parse_openavb_log_level "$HOST_LOG_LEVEL" >/dev/null 2>&1; then
        echo "ERROR: Invalid HOST_LOG_LEVEL='$HOST_LOG_LEVEL'. Use none|error|warning|info|status|debug|verbose or 0-6." >&2
        return 1
    fi

    if [[ "$profile" == "wav" ]]; then
        rm -f "$WAV_CAPTURE_FILE"
    fi

    printf "Starting %s.\n" "$profile"
    if (( reuse_persistent_infra )); then
        log_system_note "Reusing persistent Milan infra from $INFRA_STATE_FILE."
    elif [[ -n "$infra_recycle_reason" ]]; then
        log_system_note "$infra_recycle_reason"
    fi
    run_pre_start_checks

    if [[ "$RUN_SHAPER" == "1" ]]; then
        local shaper_env
        shaper_env="SHAPER_TC_LOG=$(printf '%q' "$SHAPER_TC_LOG")"
        [[ -n "$SHAPER_SKIP_ROOT_QDISC" ]] && shaper_env+=" SHAPER_SKIP_ROOT_QDISC=$(printf '%q' "$SHAPER_SKIP_ROOT_QDISC")"
        [[ -n "$SHAPER_LINK_SPEED_MBPS" ]] && shaper_env+=" SHAPER_LINK_SPEED_MBPS=$(printf '%q' "$SHAPER_LINK_SPEED_MBPS")"
        [[ -n "$SHAPER_INIT_IFACE" ]] && shaper_env+=" SHAPER_INIT_IFACE=$(printf '%q' "$SHAPER_INIT_IFACE")"
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
        progress_begin "Shaper"
        if persistent_infra_enabled && session_up "$SESS_SHAPER"; then
            progress_tick
        else
            start_session "$SESS_SHAPER" \
                "exec env $shaper_env $SHAPER_BIN $SHAPER_ARGS >>$SHAPER_LOG 2>&1"
            sleep 1
            progress_tick
        fi
        progress_finish
    fi

    if [[ "$RUN_GPTP" == "1" ]]; then
        progress_begin "gPTP"
        if ! ( persistent_infra_enabled && session_up "$SESS_GPTP" ); then
            start_session "$SESS_GPTP" \
                "cd $GPTP_CWD && exec $GPTP_BIN $IFACE_DAEMONS $GPTP_ARGS >>$GPTP_LOG 2>&1"
        fi
        if ! wait_for_gptp_ready; then
            stop_all_stack
            return 1
        fi
        progress_finish
    fi
    if phc2sys_enabled; then
        local phc2sys_bin_q phc2sys_log_q phc2sys_tmp_q
        printf -v phc2sys_bin_q '%q' "$PHC2SYS_BIN"
        printf -v phc2sys_log_q '%q' "$PHC2SYS_LOG"
        printf -v phc2sys_tmp_q '%q' "${PHC2SYS_LOG}.tmp"
        progress_begin "phc2sys"
        if ! ( persistent_infra_enabled && session_up "$SESS_PHC2SYS" ); then
            start_session "$SESS_PHC2SYS" \
                "count=0; : > $phc2sys_log_q; stdbuf -oL -eL $phc2sys_bin_q $PHC2SYS_ARGS 2>&1 | while IFS= read -r line; do printf '%s\n' \"\$line\" >>$phc2sys_log_q; count=\$((count + 1)); if (( $PHC2SYS_LOG_TRIM_EVERY > 0 && count % $PHC2SYS_LOG_TRIM_EVERY == 0 )); then tail -n $PHC2SYS_LOG_MAX_LINES $phc2sys_log_q > $phc2sys_tmp_q && mv $phc2sys_tmp_q $phc2sys_log_q; fi; done"
        fi
        if ! wait_for_phc2sys_ready; then
            stop_all_stack
            return 1
        fi
        progress_finish
    fi
    if persistent_infra_enabled; then
        write_infra_state
    else
        clear_infra_state
    fi
    if [[ "$RUN_MRPD" == "1" ]]; then
        local mrpd_log_switch
        if [[ -n "$MRPD_LOG_FLAGS" ]]; then
            mrpd_log_switch="-${MRPD_LOG_FLAGS}"
        else
            mrpd_log_switch=""
        fi
        progress_begin "MRPD"
        if ! ( persistent_infra_enabled && session_up "$SESS_MRPD" ); then
            start_session "$SESS_MRPD" \
                "exec stdbuf -oL -eL $MRPD_BIN $mrpd_log_switch -i $IFACE_DAEMONS $MRPD_ARGS >>$MRPD_LOG 2>&1"
        fi
        if ! wait_for_mrpd_srp_ready; then
            stop_all_stack
            return 1
        fi
        progress_finish
        if [[ "$RUN_MRPD_WATCH" == "1" ]]; then
            progress_begin "MRPD Watch"
            if ! ( persistent_infra_enabled && session_up "$SESS_MRPD_WATCH" ); then
                start_session "$SESS_MRPD_WATCH" \
                    "exec stdbuf -oL tail -n0 -F $MRPD_LOG 2>/dev/null | stdbuf -oL rg --line-buffered 'TALKER FAILED' | stdbuf -oL sed -n 's/^MRPD \\([0-9][0-9]*\\.[0-9][0-9]*\\).*$/\\1/p' >>$MRPD_TALKER_FAILED_LOG 2>&1"
            fi
            progress_tick
            progress_finish
        fi
    fi
    if [[ "$RUN_MAAP" == "1" ]]; then
        progress_begin "MAAP Daemon"
        if ! ( persistent_infra_enabled && session_up "$SESS_MAAP" ); then
            start_session "$SESS_MAAP" \
                "exec $MAAP_BIN -i $IFACE_DAEMONS $MAAP_ARGS >>$MAAP_LOG 2>&1"
            sleep 1
        fi
        progress_tick
        progress_finish
    fi

    local host_cmd
    local host_env
    local host_log_q host_tmp_q
    host_env="OPENAVB_ENDPOINT_INI=$ACTIVE_ENDPOINT_INI OPENAVB_ENDPOINT_SAVE_INI=$ACTIVE_ENDPOINT_SAVE_INI"
    host_env+=" OPENAVB_LOG_LEVEL=$(printf '%q' "$HOST_LOG_LEVEL")"
    [[ -n "$OPENAVB_DISABLE_SO_TXTIME" ]] && host_env+=" OPENAVB_DISABLE_SO_TXTIME=$(printf '%q' "$OPENAVB_DISABLE_SO_TXTIME")"
    printf -v host_log_q '%q' "$HOST_LOG"
    printf -v host_tmp_q '%q' "${HOST_LOG}.tmp"
    host_cmd="cd $CFG_DIR && count=0; : > $host_log_q; stdbuf -oL -eL env $host_env $HOST_BIN -I $IFACE_HOST $INI_ARGS 2>&1 | while IFS= read -r line; do printf '%s\n' \"\$line\" >>$host_log_q; count=\$((count + 1)); if (( $HOST_LOG_TRIM_EVERY > 0 && count % $HOST_LOG_TRIM_EVERY == 0 )); then tail -n $HOST_LOG_MAX_LINES $host_log_q > $host_tmp_q && mv $host_tmp_q $host_log_q; fi; done"
    local host_attempt=0
    local host_max_attempts=1
    local host_ready_rc=1
    if [[ "$RUN_MRPD" == "1" && "$HOST_SRP_RETRIES" -gt 0 ]]; then
        host_max_attempts=$((HOST_SRP_RETRIES + 1))
    fi
    progress_begin "Host"
    while (( host_attempt < host_max_attempts )); do
        if (( host_attempt > 0 )); then
            log_startup_note "Retrying host startup (attempt $((host_attempt + 1))/$host_max_attempts)."
            progress_retry
            sleep "$HOST_SRP_RETRY_DELAY_SEC"
        fi
        : > "$HOST_LOG"
        start_session "$SESS_HOST" "$host_cmd"
        if wait_for_host_endpoint_ready; then
            host_ready_rc=0
            progress_finish
            break
        else
            host_ready_rc=$?
        fi
        if (( host_ready_rc == 2 )) && (( host_attempt + 1 < host_max_attempts )); then
            stop_session "$SESS_HOST"
            host_attempt=$((host_attempt + 1))
            continue
        fi
        if (( host_ready_rc == 2 )); then
            progress_abort
            echo "ERROR: Host SRP initialization not ready after ${host_max_attempts} attempts. See $HOST_LOG" >&2
            tail -n 80 "$HOST_LOG" >&2 || true
        fi
        stop_stack
        return 1
    done
    if (( host_ready_rc != 0 )); then
        stop_stack
        return 1
    fi

    progress_begin "Streams"
    wait_for_host_stream_registration
    progress_finish
    if [[ ${#EXPECTED_MAAP_UIDS[@]} -gt 0 && "$MAAP_READY_WAIT_SEC" -gt 0 ]]; then
        progress_begin "MAAP"
    fi
    wait_for_host_maap_ready
    if [[ ${#EXPECTED_MAAP_UIDS[@]} -gt 0 && "$MAAP_READY_WAIT_SEC" -gt 0 ]]; then
        progress_finish
    fi

    # Start AVDECC after host/endpoint is ready so CONNECT_TX processing cannot
    # race stream-client registration and return transient talker misbehaving.
    progress_begin "AVDECC"
    start_session "$SESS_AVDECC" \
        "cd $CFG_DIR && OPENAVB_AVDECC_INI=$ACTIVE_AVDECC_INI exec $AVDECC_BIN -I $IFACE_AVDECC $INI_ARGS >>$AVDECC_LOG 2>&1"

    sleep 1
    progress_tick
    progress_finish

    # Let host and endpoint settle, but keep this after host startup so controllers
    # don't hit a long pre-host window that can report transient ACMP status 13.
    if [[ "$RUN_MRPD" == "1" && "$SRP_SETTLE_SEC" -gt 0 ]]; then
        sleep "$SRP_SETTLE_SEC"
    fi

    echo "Ready"
    print_kv "Profile" "$profile"
    [[ "$RUN_MRPD" == "1" ]] && print_kv "MRPD profile" "$MRPD_PROFILE"
    print_kv "State INI" "$ACTIVE_ENDPOINT_SAVE_INI"
    print_kv "Host log level" "$HOST_LOG_LEVEL"
    echo "  Sessions:"
    [[ "$RUN_GPTP" == "1" ]] && print_sub_kv "gPTP" "$SESS_GPTP"
    phc2sys_enabled && print_sub_kv "phc2sys" "$SESS_PHC2SYS"
    [[ "$RUN_MRPD" == "1" ]] && print_sub_kv "MRPD" "$SESS_MRPD"
    [[ "$RUN_MRPD" == "1" && "$RUN_MRPD_WATCH" == "1" ]] && print_sub_kv "MRPD watch" "$SESS_MRPD_WATCH"
    [[ "$RUN_MAAP" == "1" ]] && print_sub_kv "MAAP" "$SESS_MAAP"
    [[ "$RUN_SHAPER" == "1" ]] && print_sub_kv "Shaper" "$SESS_SHAPER"
    print_sub_kv "AVDECC" "$SESS_AVDECC"
    print_sub_kv "Host" "$SESS_HOST"
    echo "  Logs:"
    print_sub_kv "System" "$SYSTEM_LOG"
    [[ "$RUN_GPTP" == "1" ]] && print_sub_kv "gPTP" "$GPTP_LOG"
    phc2sys_enabled && print_sub_kv "phc2sys" "$PHC2SYS_LOG"
    [[ "$RUN_MRPD" == "1" ]] && print_sub_kv "MRPD" "$MRPD_LOG"
    [[ "$RUN_MRPD" == "1" && "$RUN_MRPD_WATCH" == "1" ]] && print_sub_kv "MRPD watch" "$MRPD_TALKER_FAILED_LOG"
    [[ "$RUN_MAAP" == "1" ]] && print_sub_kv "MAAP" "$MAAP_LOG"
    [[ "$RUN_SHAPER" == "1" ]] && print_sub_kv "Shaper" "$SHAPER_LOG"
    print_sub_kv "AVDECC" "$AVDECC_LOG"
    print_sub_kv "Host" "$HOST_LOG"
    echo "  INIs:"
    for ini_entry in "${INI_LIST_ARR[@]}"; do
        echo "    $(resolve_ini_entry "$ini_entry")"
    done
    print_kv "Endpoint INI" "$ACTIVE_ENDPOINT_INI"
    print_kv "AVDECC INI" "$ACTIVE_AVDECC_INI"
}

status_stack() {
    apply_stack_profile
    reorder_crf_inis_last
    build_ini_args
    compute_shaper_presets_from_inis 0

    echo "Status"
    print_kv "Profile" "${STACK_PROFILE,,}"
    print_kv "Host iface" "$IFACE_HOST"
    print_kv "AVDECC iface" "$IFACE_AVDECC"
    print_kv "Daemon iface" "$IFACE_DAEMONS"
    print_kv "State INI" "$ACTIVE_ENDPOINT_SAVE_INI"
    print_kv "Host log level" "$HOST_LOG_LEVEL"
    print_kv "Infra mode" "$(persistent_infra_enabled && printf 'persistent' || printf 'ephemeral')"
    [[ -n "$SHAPER_PRESET_SUMMARY" ]] && print_kv "INI class load" "$SHAPER_PRESET_SUMMARY"
    echo "  Components:"
    [[ "$RUN_GPTP" == "1" ]] && print_component_status "gPTP" "$SESS_GPTP" "[d]aemon_cl"
    phc2sys_enabled && print_component_status "phc2sys" "$SESS_PHC2SYS" "[p]hc2sys( |$)"
    [[ "$RUN_MRPD" == "1" ]] && print_component_status "MRPD" "$SESS_MRPD" "[m]rpd .* -i $IFACE_DAEMONS"
    [[ "$RUN_MRPD" == "1" && "$RUN_MRPD_WATCH" == "1" ]] && print_component_status "MRPD watch" "$SESS_MRPD_WATCH"
    [[ "$RUN_MAAP" == "1" ]] && print_component_status "MAAP" "$SESS_MAAP" "[m]aap_daemon -i $IFACE_DAEMONS"
    [[ "$RUN_SHAPER" == "1" ]] && print_component_status "Shaper" "$SESS_SHAPER" "[s]haper_daemon"
    print_component_status "AVDECC" "$SESS_AVDECC" "[o]penavb_avdecc -I $IFACE_AVDECC"
    print_component_status "Host" "$SESS_HOST" "[o]penavb_host -I $IFACE_HOST"
    echo "  Logs:"
    print_sub_kv "System" "$SYSTEM_LOG"
    [[ "$RUN_GPTP" == "1" ]] && print_sub_kv "gPTP" "$GPTP_LOG"
    phc2sys_enabled && print_sub_kv "phc2sys" "$PHC2SYS_LOG"
    [[ "$RUN_MRPD" == "1" ]] && print_sub_kv "MRPD" "$MRPD_LOG"
    [[ "$RUN_MRPD" == "1" && "$RUN_MRPD_WATCH" == "1" ]] && print_sub_kv "MRPD watch" "$MRPD_TALKER_FAILED_LOG"
    [[ "$RUN_MAAP" == "1" ]] && print_sub_kv "MAAP" "$MAAP_LOG"
    [[ "$RUN_SHAPER" == "1" ]] && print_sub_kv "Shaper" "$SHAPER_LOG"
    print_sub_kv "AVDECC" "$AVDECC_LOG"
    print_sub_kv "Host" "$HOST_LOG"
}

logs_stack() {
    echo "== $SYSTEM_LOG =="
    tail -n 80 "$SYSTEM_LOG" 2>/dev/null || true
    echo
    if [[ "$RUN_GPTP" == "1" ]]; then
        echo "== $GPTP_LOG =="
        tail -n 80 "$GPTP_LOG" 2>/dev/null || true
        echo
    fi
    if phc2sys_enabled; then
        echo "== $PHC2SYS_LOG =="
        tail -n 80 "$PHC2SYS_LOG" 2>/dev/null || true
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
    ""|help|-h|--help)
        usage
        ;;
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
    infra-stop)
        stop_all_stack
        cleanup_ipc_paths
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
