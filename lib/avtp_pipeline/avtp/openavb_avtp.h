/*************************************************************************************************************
Copyright (c) 2012-2015, Symphony Teleca Corporation, a Harman International Industries, Incorporated company
Copyright (c) 2016-2017, Harman International Industries, Incorporated
All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 
1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS LISTED "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS LISTED BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
Attributions: The inih library portion of the source code is licensed from 
Brush Technology and Ben Hoyt - Copyright (c) 2009, Brush Technology and Copyright (c) 2009, Ben Hoyt. 
Complete license and copyright information can be found at 
https://github.com/benhoyt/inih/commit/74d2ca064fb293bc60a77b0bd068075b293cf175.
*************************************************************************************************************/

/*
* HEADER SUMMARY : Declare the main functions for AVTP.  Includes
* functions to create/destroy and AVTP stream, and to send or receive
* data from that AVTP stream.
*/

#ifndef AVB_AVTP_H
#define AVB_AVTP_H 1

#include "openavb_platform.h"
#include "openavb_intf_pub.h"
#include "openavb_map_pub.h"
#include "openavb_rawsock.h"
#include "openavb_timestamp.h"

#define ETHERTYPE_AVTP 0x22F0
#define ETHERTYPE_8021Q 0x8100
#define ETHERNET_8021Q_OCTETS 4
#define TS_PACKET_LEN  188
#define SRC_PACKET_LEN 192
#define CIP_HEADER_LEN 8

// Length of Ethernet frame header, with and without 802.1Q tag
#define ETH_HDR_LEN			14
#define ETH_HDR_LEN_VLAN	18

// AVTP Headers
#define AVTP_COMMON_STREAM_DATA_HDR_LEN	24

//#define OPENAVB_AVTP_REPORT_RX_STATS 1
#define OPENAVB_AVTP_REPORT_INTERVAL 100

typedef struct {
	// These are significant only for RX data
	U32					timestamp;  // delivery timestamp
	bool				bComplete;	// not waiting for more data
#ifdef OPENAVB_AVTP_REPORT_RX_STATS
	U32					rxCnt, lateCnt, earlyCnt;
	U32					maxLate, maxEarly;
	struct timespec		lastTime;
#endif
} avtp_rx_info_t;
	
typedef struct {
	U8						*data;	// pointer to data
	avtp_rx_info_t			rx;		// re-assembly info
} avtp_info_t;

typedef struct {
	media_q_t 				mediaq;
} avtp_state_t;

typedef struct
{
	U32 stream_start;
	U32 stream_stop;
	U32 stream_interrupted;
	U32 media_locked;
	U32 media_unlocked;
	U32 seq_num_mismatch;
	U32 media_reset;
	U32 timestamp_uncertain;
	U32 timestamp_valid;
	U32 timestamp_not_valid;
	U32 unsupported_format;
	U32 late_timestamp;
	U32 early_timestamp;
	U32 frames_rx;
	U32 frames_tx;
} openavb_avtp_diag_counters_t;

static inline void openavbAvtpDiagCountersAccumulate(
	openavb_avtp_diag_counters_t *pDst,
	const openavb_avtp_diag_counters_t *pSrc)
{
	if (!pDst || !pSrc) {
		return;
	}

	pDst->stream_start += pSrc->stream_start;
	pDst->stream_stop += pSrc->stream_stop;
	pDst->stream_interrupted += pSrc->stream_interrupted;
	pDst->media_locked += pSrc->media_locked;
	pDst->media_unlocked += pSrc->media_unlocked;
	pDst->seq_num_mismatch += pSrc->seq_num_mismatch;
	pDst->media_reset += pSrc->media_reset;
	pDst->timestamp_uncertain += pSrc->timestamp_uncertain;
	pDst->timestamp_valid += pSrc->timestamp_valid;
	pDst->timestamp_not_valid += pSrc->timestamp_not_valid;
	pDst->unsupported_format += pSrc->unsupported_format;
	pDst->late_timestamp += pSrc->late_timestamp;
	pDst->early_timestamp += pSrc->early_timestamp;
	pDst->frames_rx += pSrc->frames_rx;
	pDst->frames_tx += pSrc->frames_tx;
}


/* Info associated with an AVTP stream (RX or TX).
 *
 * The void* handle that is returned to the client
 * really is a pointer to an avtp_stream_t.
 * 
 * TODO: This passed around as void * handle can be typed since the avtp_stream_t is
 * now seen by the talker / listern module.
 * 
 */
typedef struct
{
	// TX socket?
	bool tx;
	// Interface name
	char* ifname;
	// Number of rawsock buffers
	U16 nbuffers;
	// The rawsock library handle.  Used to send or receive frames.
	void *rawsock;
	bool owns_rawsock;
	// The streamID - in network form
	U8 streamIDnet[8];
	// The destination address for stream
	struct ether_addr dest_addr;
	// The AVTP subtype; it determines the encapsulation
	U8 subtype;
	// Max Transit - value added to current time to get play time
	U64 max_transit_usec;
	// Queue-ahead pacing used before submitting the packet into rawsock/qdisc.
	U32 tx_submit_ahead_usec;
	U32 tx_submit_skew_usec;
	bool tx_skip_submit_pacing;
	U32 tx_fwmark;
	U8 tx_src_addr[ETH_ALEN];
	U8 tx_dest_addr[ETH_ALEN];
	bool tx_vlan;
	U16 tx_vlan_id;
	U8 tx_vlan_pcp;
	// Max frame size
	U16 frameLen;
	// AVTP sequence number
	U8 avtp_sequence_num;
	// AVTP MR bit state; toggled when media clock source changes.
	bool media_restart_toggle;
	// Paused state of the stream
	bool bPause;
	// Encapsulation-specific state information
	avtp_state_t state;
	// RX info for data sample currently being received
	avtp_info_t info;
	// Mapping callbacks
	openavb_map_cb_t *pMapCB;
	// Interface callbacks
	openavb_intf_cb_t *pIntfCB;
	// MediaQ
	media_q_t *pMediaQ;
	bool bRxSignalMode;

	// TX frame buffer
	U8* pBuf;
	// Ethernet header length
	U32 ethHdrLen;
	
	// Timestamp evaluation related
	openavb_timestamp_eval_t tsEval;

	// Stat related	
	// RX frames lost
	int nLost;
	// Bytes sent or recieved
	U64 bytes;
	// Per-stream launch debug line count
	U32 tx_launch_log_count;
	U32 tx_launch_margin_log_count;
	// Per-stream submit debug line count
	U32 tx_submit_log_count;
	U32 tx_submit_warn_log_count;
	// Small local launch offset learned from map_lt_calc_cb relative to packet timestamp.
	bool tx_map_launch_offset_valid;
	S64 tx_map_launch_offset_ns;
	U32 tx_map_launch_mismatch_log_count;
	// Per-stream TX path instrumentation.
	U32 tx_path_log_count;
	U32 tx_acquire_log_count;
	U32 tx_send_warn_log_count;
	U64 tx_path_samples;
	U64 tx_path_intf_sum_ns;
	U64 tx_path_intf_min_ns;
	U64 tx_path_intf_max_ns;
	U64 tx_path_map_sum_ns;
	U64 tx_path_map_min_ns;
	U64 tx_path_map_max_ns;
	U64 tx_path_build_sum_ns;
	U64 tx_path_build_min_ns;
	U64 tx_path_build_max_ns;
	U64 tx_path_clamp_count;
	bool tx_emit_seq_valid;
	U8 tx_last_emit_seq;
	U32 tx_emit_gap_log_count;

	// Milan / AECP diagnostic counters for the current AVTP session.
	openavb_avtp_diag_counters_t diag;
	bool rx_mr_valid;
	bool rx_last_mr;
	bool rx_tu_valid;
	bool rx_last_tu;
	bool tx_tu_valid;
	bool tx_last_tu;
	
} avtp_stream_t;


typedef void (*avtp_listener_callback_fn)(void *pv, avtp_info_t *data);

// tx/rx
openavbRC openavbAvtpTxInit(media_q_t *pMediaQ,
					openavb_map_cb_t *pMapCB,
					openavb_intf_cb_t *pIntfCB,
					char* ifname,
					AVBStreamID_t *streamID,
					U8* destAddr,
					U32 max_transit_usec,
					U32 tx_submit_ahead_usec,
					U32 tx_submit_skew_usec,
					U32 fwmark,
					U16 vlanID,
					U8  vlanPCP,
					U16 nbuffers,
					void **pStream_out);

openavbRC openavbAvtpTx(void *pv, bool bSend, bool txBlockingInIntf);

openavbRC openavbAvtpRxInit(media_q_t *pMediaQ, 
					openavb_map_cb_t *pMapCB,
					openavb_intf_cb_t *pIntfCB,
					char* ifname,
					AVBStreamID_t *streamID,
					U8* destAddr,
					U16 nbuffers,
					bool rxSignalMode,
					void **pStream_out);

openavbRC openavbAvtpRx(void *handle);

void openavbAvtpConfigTimsstampEval(void *handle, U32 tsInterval, U32 reportInterval, bool smoothing, U32 tsMaxJitter, U32 tsMaxDrift);

void openavbAvtpPause(void *handle, bool bPause);
void openavbAvtpRequestMediaRestart(void *handle);

void openavbAvtpShutdownTalker(void *handle);
void openavbAvtpShutdownListener(void *handle);

int openavbAvtpTxBufferLevel(void *handle);

int openavbAvtpRxBufferLevel(void *handle);

int openavbAvtpLost(void *handle);

U64 openavbAvtpBytes(void *handle);

void openavbAvtpGetDiagCounters(void *handle, openavb_avtp_diag_counters_t *pCounters);

#endif //AVB_AVTP_H
