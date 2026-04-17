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
* MODULE SUMMARY : Implements main functions for AVTP.  Includes
* functions to create/destroy and AVTP stream, and to send or receive
* data from that AVTP stream.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include "openavb_platform.h"
#include "openavb_types.h"
#include "openavb_trace.h"
#include "openavb_avtp.h"
#include "openavb_avtp_time_pub.h"
#include "openavb_rawsock.h"
#include "openavb_mediaq.h"

#define	AVB_LOG_COMPONENT	"AVTP"
#include "openavb_log.h"

#define AVTP_V0_HEADER_SIZE 12
#define AVTP_TX_PATH_DIAG_INTERVAL 2048U
#define AVTP_TX_PATH_WARN_NS 500000ULL
#define AVTP_TX_ACQUIRE_WARN_NS 200000ULL
#define AVTP_CRF_SUBTYPE 0x04
#define AVTP_CRF_SEQ_SHIFT 8

static bool avtpShouldLogSparse(U32 count)
{
	return (count < 32U) || ((count % 1024U) == 0U);
}

static void avtpTxPathDiagUpdate(U64 *pMin, U64 *pMax, U64 *pSum, U64 count, U64 value)
{
	if (count == 0) {
		*pMin = value;
		*pMax = value;
		*pSum = value;
		return;
	}

	if (value < *pMin) {
		*pMin = value;
	}
	if (value > *pMax) {
		*pMax = value;
	}
	*pSum += value;
}

static void avtpMaybeLogTxPathDiag(avtp_stream_t *pStream)
{
	U16 streamUid = 0;

	if (!pStream || pStream->tx_path_samples == 0 || pStream->tx_path_samples < AVTP_TX_PATH_DIAG_INTERVAL) {
		return;
	}

	memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
	streamUid = ntohs(streamUid);
	AVB_LOGF_WARNING(
		"AVTP TX PATH DIAG uid=%u subtype=0x%02x packets=%llu intf_avg=%lluns min=%lluns max=%lluns map_avg=%lluns min=%lluns max=%lluns build_avg=%lluns min=%lluns max=%lluns clamp=%llu",
		streamUid,
		pStream->subtype,
		(unsigned long long)pStream->tx_path_samples,
		(unsigned long long)(pStream->tx_path_intf_sum_ns / pStream->tx_path_samples),
		(unsigned long long)pStream->tx_path_intf_min_ns,
		(unsigned long long)pStream->tx_path_intf_max_ns,
		(unsigned long long)(pStream->tx_path_map_sum_ns / pStream->tx_path_samples),
		(unsigned long long)pStream->tx_path_map_min_ns,
		(unsigned long long)pStream->tx_path_map_max_ns,
		(unsigned long long)(pStream->tx_path_build_sum_ns / pStream->tx_path_samples),
		(unsigned long long)pStream->tx_path_build_min_ns,
		(unsigned long long)pStream->tx_path_build_max_ns,
		(unsigned long long)pStream->tx_path_clamp_count);

	pStream->tx_path_samples = 0;
	pStream->tx_path_intf_sum_ns = 0;
	pStream->tx_path_intf_min_ns = 0;
	pStream->tx_path_intf_max_ns = 0;
	pStream->tx_path_map_sum_ns = 0;
	pStream->tx_path_map_min_ns = 0;
	pStream->tx_path_map_max_ns = 0;
	pStream->tx_path_build_sum_ns = 0;
	pStream->tx_path_build_min_ns = 0;
	pStream->tx_path_build_max_ns = 0;
	pStream->tx_path_clamp_count = 0;
	pStream->tx_emit_seq_valid = FALSE;
	pStream->tx_last_emit_seq = 0;
	pStream->tx_emit_gap_log_count = 0;
}

static bool avtpExtractTxSequence(avtp_stream_t *pStream, const U8 *pAvtpFrame, U8 *pSeq)
{
	if (!pStream || !pAvtpFrame || !pSeq) {
		return FALSE;
	}

	if (pStream->subtype == AVTP_CRF_SUBTYPE) {
		U32 subtypeData = 0;
		memcpy(&subtypeData, pAvtpFrame, sizeof(subtypeData));
		subtypeData = ntohl(subtypeData);
		*pSeq = (U8)((subtypeData >> AVTP_CRF_SEQ_SHIFT) & 0xFF);
		return TRUE;
	}

	*pSeq = pAvtpFrame[2];
	return TRUE;
}

static U64 avtpMaybePaceSubmit(avtp_stream_t *pStream, U64 launchTimeNs)
{
	U64 nowNs = 0;
	U64 submitAdvanceNs = 0;
	U64 targetSubmitNs = 0;
	U64 remainingNs = 0;
	U64 monoStartNs = 0;
	U64 monoEndNs = 0;

	if (!pStream || launchTimeNs == 0) {
		return 0;
	}
	/*
	 * CRF already carries an explicit launch target from the mapper and is
	 * submitted as a single stream, unlike grouped AAF which owns pacing at
	 * the group layer. When the OpenAvnu gPTP walltime diverges from the
	 * kernel CLOCK_TAI domain, submit pacing can sleep for seconds and then
	 * hand rawsock a launch time that is already in the past. Bypass pacing
	 * here and let ETF/TXTIME enforce the mapper-provided launch time.
	 */
	if (pStream->subtype == AVTP_CRF_SUBTYPE) {
		return 0;
	}
	if (pStream->tx_skip_submit_pacing) {
		return 0;
	}

	submitAdvanceNs = ((U64)pStream->tx_submit_ahead_usec + (U64)pStream->tx_submit_skew_usec) * 1000ULL;
	if (submitAdvanceNs == 0 || launchTimeNs <= submitAdvanceNs) {
		return 0;
	}

	targetSubmitNs = launchTimeNs - submitAdvanceNs;
	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &monoStartNs);
	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs)) {
		return 0;
	}

	if (nowNs >= targetSubmitNs) {
		if (pStream->tx_submit_log_count < 16 && nowNs > targetSubmitNs) {
			U16 streamUid = 0;
			memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
			streamUid = ntohs(streamUid);
			AVB_LOGF_INFO(
				"AVTP TX submit window: uid=%u subtype=0x%02x submit=%" PRIu64 " now=%" PRIu64 " delta=%" PRId64 "ns ahead=%u skew=%u",
				streamUid,
				pStream->subtype,
				targetSubmitNs,
				nowNs,
				(int64_t)(targetSubmitNs - nowNs),
				pStream->tx_submit_ahead_usec,
				pStream->tx_submit_skew_usec);
			pStream->tx_submit_log_count++;
		}
		return 0;
	}

	remainingNs = targetSubmitNs - nowNs;
	while (remainingNs > 100000ULL) {
		SLEEP_NSEC(remainingNs - 50000ULL);
		if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs) || nowNs >= targetSubmitNs) {
			break;
		}
		remainingNs = targetSubmitNs - nowNs;
	}

	if (nowNs < targetSubmitNs) {
		SPIN_UNTIL_NSEC(targetSubmitNs);
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
	}

	if (pStream->tx_submit_log_count < 16) {
		U16 streamUid = 0;
		memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
		streamUid = ntohs(streamUid);
		AVB_LOGF_INFO(
			"AVTP TX submit window: uid=%u subtype=0x%02x submit=%" PRIu64 " now=%" PRIu64 " delta=%" PRId64 "ns ahead=%u skew=%u",
			streamUid,
			pStream->subtype,
			targetSubmitNs,
			nowNs,
			(int64_t)(targetSubmitNs - nowNs),
			pStream->tx_submit_ahead_usec,
			pStream->tx_submit_skew_usec);
		pStream->tx_submit_log_count++;
	}

	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &monoEndNs);
	if (monoEndNs >= monoStartNs) {
		U64 paceDurationNs = monoEndNs - monoStartNs;
		if (paceDurationNs >= 5000000ULL && pStream->tx_submit_warn_log_count < 32) {
			U16 streamUid = 0;
			memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
			streamUid = ntohs(streamUid);
			AVB_LOGF_WARNING(
				"AVTP TX submit slow: uid=%u subtype=0x%02x launch=%" PRIu64 " submit=%" PRIu64 " final_now=%" PRIu64 " duration=%" PRIu64 "ns ahead=%u skew=%u",
				streamUid,
				pStream->subtype,
				launchTimeNs,
				targetSubmitNs,
				nowNs,
				paceDurationNs,
				pStream->tx_submit_ahead_usec,
				pStream->tx_submit_skew_usec);
			pStream->tx_submit_warn_log_count++;
		}
		return paceDurationNs;
	}

	return 0;
}

#if IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
static S64 avtpSignedDeltaNs(U64 newer, U64 older)
{
	return (newer >= older)
		? (S64)(newer - older)
		: -((S64)(older - newer));
}

static U64 avtpApplySignedOffsetNs(U64 baseNs, S64 offsetNs)
{
	if (offsetNs >= 0) {
		return baseNs + (U64)offsetNs;
	}

	U64 absOffsetNs = (U64)(-offsetNs);
	return (baseNs > absOffsetNs) ? (baseNs - absOffsetNs) : 0ULL;
}

static bool avtpCalcLaunchTimeFromTimestamp(avtp_stream_t *pStream, const U8 *pAvtpFrame, U64 *launchTimeNsec, U64 *timestampTimeNsec)
{
	if (!pStream || !pAvtpFrame || !launchTimeNsec) {
		return FALSE;
	}

	// TV (timestamp valid) bit is LSB of byte 1 in AVTP v0 header.
	if ((pAvtpFrame[1] & 0x01) == 0) {
		return FALSE;
	}

	U32 avtpTs;
	memcpy(&avtpTs, pAvtpFrame + AVTP_V0_HEADER_SIZE, sizeof(avtpTs));
	avtpTs = ntohl(avtpTs);

	avtp_time_t tmp = {0};
	openavbAvtpTimeSetToTimestamp(&tmp, avtpTs);
	if (!tmp.bTimestampValid) {
		return FALSE;
	}

	U64 launchNs = tmp.timeNsec;
	U64 transitNs = pStream->max_transit_usec * 1000ULL;
	if (launchNs > transitNs) {
		launchNs -= transitNs;
	}

	if (timestampTimeNsec) {
		*timestampTimeNsec = tmp.timeNsec;
	}
	*launchTimeNsec = launchNs;
	return TRUE;
}
#endif

// Maximum time that AVTP RX/TX calls should block before returning
#define AVTP_MAX_BLOCK_USEC (1 * MICROSECONDS_PER_SECOND)

/*
 * This is broken out into a function, so that we can close and reopen
 * the socket if we detect a problem receiving frames.
 */
static openavbRC openAvtpSock(avtp_stream_t *pStream)
{
	if (pStream->tx) {
		pStream->rawsock = openavbRawsockOpen(pStream->ifname, FALSE, TRUE, ETHERTYPE_AVTP, pStream->frameLen, pStream->nbuffers);
	}
	else {
#ifndef UBUNTU
		// This is the normal case for most of our supported platforms
		pStream->rawsock = openavbRawsockOpen(pStream->ifname, TRUE, FALSE, ETHERTYPE_8021Q, pStream->frameLen, pStream->nbuffers);
#else
		pStream->rawsock = openavbRawsockOpen(pStream->ifname, TRUE, FALSE, ETHERTYPE_AVTP, pStream->frameLen, pStream->nbuffers);
#endif
	}

	if (pStream->rawsock != NULL) {
		openavbSetRxSignalMode(pStream->rawsock, pStream->bRxSignalMode);

		if (!pStream->tx) {
			// Set the multicast address that we want to receive
			openavbRawsockRxMulticast(pStream->rawsock, TRUE, pStream->dest_addr.ether_addr_octet);
		}
		AVB_RC_RET(OPENAVB_AVTP_SUCCESS);
	}

	AVB_RC_LOG_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_RAWSOCK_OPEN));
}


// Evaluate the AVTP timestamp. Only valid for common AVTP stream subtypes.
// CRF repurposes the bytes at the common timestamp offset for packet_info, so
// generic timestamp smoothing/counting must not touch subtype 0x04 packets.
#define AVTP_SUBTYPE_CRF                0x04
#define HIDX_AVTP_HIDE7_TV1			1
#define HIDX_AVTP_HIDE7_TU1			3
#define HIDX_AVTP_TIMESPAMP32		12
static void processTimestampEval(avtp_stream_t *pStream, U8 *pHdr)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP_DETAIL);

	if (pStream->tsEval && pStream->subtype != AVTP_SUBTYPE_CRF) {
		bool tsValid =  (pHdr[HIDX_AVTP_HIDE7_TV1] & 0x01) ? TRUE : FALSE;
		bool tsUncertain = (pHdr[HIDX_AVTP_HIDE7_TU1] & 0x01) ? TRUE : FALSE;

		if (tsValid && !tsUncertain) {
			U32 ts = ntohl(*(U32 *)(&pHdr[HIDX_AVTP_TIMESPAMP32]));
			U32 tsSmoothed = openavbTimestampEvalTimestamp(pStream->tsEval, ts);
			if (tsSmoothed != ts) {
				*(U32 *)(&pHdr[HIDX_AVTP_TIMESPAMP32]) = htonl(tsSmoothed);
			}
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVTP_DETAIL);
}

static void openavbAvtpCountLateEarlyTimestamp(avtp_stream_t *pStream, U32 avtpTimestamp, bool timestampValid, bool timestampUncertain)
{
	avtp_time_t avtpTime;
	S32 deltaUsec;

	if (!pStream || !timestampValid || timestampUncertain) {
		return;
	}

	if (pStream->subtype == AVTP_SUBTYPE_CRF) {
		return;
	}

	memset(&avtpTime, 0, sizeof(avtpTime));
	openavbAvtpTimeSetToTimestamp(&avtpTime, avtpTimestamp);
	openavbAvtpTimeSetTimestampValid(&avtpTime, TRUE);
	openavbAvtpTimeSetTimestampUncertain(&avtpTime, FALSE);

	deltaUsec = openavbAvtpTimeUsecDelta(&avtpTime);
	if (deltaUsec < 0) {
		pStream->diag.late_timestamp++;
	}
	else if (deltaUsec > (S32)MICROSECONDS_PER_SECOND) {
		pStream->diag.early_timestamp++;
	}
}


/* Initialize AVTP for talking
 */
openavbRC openavbAvtpTxInit(
	media_q_t *pMediaQ,
	openavb_map_cb_t *pMapCB,
	openavb_intf_cb_t *pIntfCB,
	char *ifname,
	AVBStreamID_t *streamID,
	U8 *destAddr,
	U32 max_transit_usec,
	U32 tx_submit_ahead_usec,
	U32 tx_submit_skew_usec,
	U32 fwmark,
	U16 vlanID,
	U8  vlanPCP,
	U16 nbuffers,
	void **pStream_out)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);
	AVB_LOG_DEBUG("Initialize");

	*pStream_out = NULL;

	// Malloc the structure to hold state information
	avtp_stream_t *pStream = calloc(1, sizeof(avtp_stream_t));
	if (!pStream) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_OUT_OF_MEMORY), AVB_TRACE_AVTP);
	}
	pStream->tx = TRUE;
	pStream->owns_rawsock = TRUE;

	pStream->pMediaQ = pMediaQ;
	pStream->pMapCB = pMapCB;
	pStream->pIntfCB = pIntfCB;

	pStream->pMapCB->map_tx_init_cb(pStream->pMediaQ);
	pStream->pIntfCB->intf_tx_init_cb(pStream->pMediaQ);
	if (pStream->pIntfCB->intf_set_stream_uid_cb) {
		pStream->pIntfCB->intf_set_stream_uid_cb(pStream->pMediaQ, streamID->uniqueID);
	}

	// Set the frame length
	pStream->frameLen = pStream->pMapCB->map_max_data_size_cb(pStream->pMediaQ) + ETH_HDR_LEN_VLAN;

	// and the latency
	pStream->max_transit_usec = max_transit_usec;
	pStream->tx_submit_ahead_usec = tx_submit_ahead_usec;
	pStream->tx_submit_skew_usec = tx_submit_skew_usec;

	// and save other stuff needed to (re)open the socket
	pStream->ifname = strdup(ifname);
	pStream->nbuffers = nbuffers;

	// Open a raw socket
	openavbRC rc = openAvtpSock(pStream);
	if (IS_OPENAVB_FAILURE(rc)) {
		free(pStream);
		AVB_RC_LOG_TRACE_RET(rc, AVB_TRACE_AVTP);
	}

	// Create the AVTP frame header for the frames we'll send
	hdr_info_t hdrInfo;

	U8 srcAddr[ETH_ALEN];
	if (openavbRawsockGetAddr(pStream->rawsock, srcAddr)) {
		hdrInfo.shost = srcAddr;
		memcpy(pStream->tx_src_addr, srcAddr, ETH_ALEN);
	}
	else {
		openavbRawsockClose(pStream->rawsock);
		free(pStream);
		AVB_LOG_ERROR("Failed to get source MAC address");
		AVB_RC_TRACE_RET(OPENAVB_AVTP_FAILURE, AVB_TRACE_AVTP);
	}

	hdrInfo.dhost = destAddr;
	memcpy(pStream->tx_dest_addr, destAddr, ETH_ALEN);
	if (vlanPCP != 0 || vlanID != 0) {
		hdrInfo.vlan = TRUE;
		hdrInfo.vlan_pcp = vlanPCP;
		hdrInfo.vlan_vid = vlanID;
		AVB_LOGF_DEBUG("VLAN pcp=%d vid=%d", hdrInfo.vlan_pcp, hdrInfo.vlan_vid);
	}
	else {
		hdrInfo.vlan = FALSE;
	}
	pStream->tx_vlan = hdrInfo.vlan;
	pStream->tx_vlan_pcp = vlanPCP;
	pStream->tx_vlan_id = vlanID;
	openavbRawsockTxSetHdr(pStream->rawsock, &hdrInfo);

	// Remember the AVTP subtype and streamID
	pStream->subtype = pStream->pMapCB->map_subtype_cb();

	memcpy(pStream->streamIDnet, streamID->addr, ETH_ALEN);
        U16 *pStreamUID = (U16 *)((U8 *)(pStream->streamIDnet) + ETH_ALEN);
       *pStreamUID = htons(streamID->uniqueID);

	// Set the fwmark - used to steer packets into the right traffic control queue
	pStream->tx_fwmark = fwmark;
	openavbRawsockTxSetMark(pStream->rawsock, fwmark);

	*pStream_out = (void *)pStream;
	AVB_RC_TRACE_RET(OPENAVB_AVTP_SUCCESS, AVB_TRACE_AVTP);
}

#ifdef OPENAVB_AVTP_REPORT_RX_STATS
static void inline rxDeliveryStats(avtp_rx_info_t *rxInfo,
	struct timespec *tmNow,
	U32 early, U32 late)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);

	rxInfo->rxCnt++;

	if (late > 0) {
		rxInfo->lateCnt++;
		if (late > rxInfo->maxLate)
			rxInfo->maxLate = late;
	}
	if (early > 0) {
		rxInfo->earlyCnt++;
		if (early > rxInfo->maxEarly)
			rxInfo->maxEarly = early;
	}

	if (rxInfo->lastTime.tv_sec == 0) {
		rxInfo->lastTime.tv_sec = tmNow->tv_sec;
		rxInfo->lastTime.tv_nsec = tmNow->tv_nsec;
	}
	else if ((tmNow->tv_sec > (rxInfo->lastTime.tv_sec + OPENAVB_AVTP_REPORT_INTERVAL))
		|| ((tmNow->tv_sec == (rxInfo->lastTime.tv_sec + OPENAVB_AVTP_REPORT_INTERVAL))
			&& (tmNow->tv_nsec > rxInfo->lastTime.tv_nsec))) {
		AVB_LOGF_INFO("Stream %d seconds, %lu samples: %lu late, max=%lums, %lu early, max=%lums",
			OPENAVB_AVTP_REPORT_INTERVAL, (unsigned long)rxInfo->rxCnt,
			(unsigned long)rxInfo->lateCnt, (unsigned long)rxInfo->maxLate / NANOSECONDS_PER_MSEC,
			(unsigned long)rxInfo->earlyCnt, (unsigned long)rxInfo->maxEarly / NANOSECONDS_PER_MSEC);
		rxInfo->maxLate = 0;
		rxInfo->lateCnt = 0;
		rxInfo->maxEarly = 0;
		rxInfo->earlyCnt = 0;
		rxInfo->rxCnt = 0;
		rxInfo->lastTime.tv_sec = tmNow->tv_sec;
		rxInfo->lastTime.tv_nsec = tmNow->tv_nsec;
	}

#if 0
	if (++txCnt >= 1000) {}
#endif
	AVB_TRACE_EXIT(AVB_TRACE_AVTP);
}
#endif

static openavbRC fillAvtpHdr(avtp_stream_t *pStream, U8 *pFill)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP_DETAIL);

	switch (pStream->pMapCB->map_avtp_version_cb()) {
		default:
			AVB_RC_LOG_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_INVALID_AVTP_VERSION));
		case 0:
			//
			// - 1 bit 		cd (control/data indicator)	= 0 (stream data)
			// - 7 bits 	subtype  					= as configured
			*pFill++ = pStream->subtype & 0x7F;
			// - 1 bit 		sv (stream valid)			= 1
			// - 3 bits 	AVTP version				= binary 000
			// - 1 bit		mr (media restart)			= toggled when clock changes
			// - 1 bit		r (reserved)				= 0
			// - 1 bit		gv (gateway valid)			= 0
			// - 1 bit		tv (timestamp valid)		= 1
			*pFill++ = (pStream->media_restart_toggle ? 0x89 : 0x81);
			// - 8 bits		sequence num				= increments with each frame
			*pFill++ = pStream->avtp_sequence_num;
			// - 7 bits		reserved					= 0;
			// - 1 bit		tu (timestamp uncertain)	= 1 when no PTP sync
			// TODO: set tu correctly
			*pFill++ = 0;
			// - 8 bytes    stream_id
			memcpy(pFill, (U8 *)&pStream->streamIDnet, 8);
			break;
	}
	AVB_RC_TRACE_RET(OPENAVB_AVTP_SUCCESS, AVB_TRACE_AVTP_DETAIL);
}

/* Send a frame
 */
openavbRC openavbAvtpTx(void *pv, bool bSend, bool txBlockingInIntf)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP_DETAIL);

	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (!pStream) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT), AVB_TRACE_AVTP_DETAIL);
	}

	U8 * pAvtpFrame,*pFill;
	U32 avtpFrameLen, frameLen;
	tx_cb_ret_t txCBResult = TX_CB_RET_PACKET_NOT_READY;

	// Get a TX buf if we don't already have one.
	//   (We keep the TX buf in our stream data, so that if we don't
	//    get data from the mapping module, we can use the buf next time.)
	if (!pStream->pBuf) {
		hdr_info_t hdrInfo;
		U64 acquireStartNs = 0;
		U64 hdrDoneNs = 0;
		U64 markDoneNs = 0;
		U64 getDoneNs = 0;
		U64 hdrDurationNs = 0;
		U64 markDurationNs = 0;
		U64 getDurationNs = 0;
		U64 acquireDurationNs = 0;
		U16 streamUid = 0;

		(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &acquireStartNs);
		memset(&hdrInfo, 0, sizeof(hdrInfo));
		hdrInfo.shost = pStream->tx_src_addr;
		hdrInfo.dhost = pStream->tx_dest_addr;
		hdrInfo.vlan = pStream->tx_vlan;
		hdrInfo.vlan_pcp = pStream->tx_vlan_pcp;
		hdrInfo.vlan_vid = pStream->tx_vlan_id;

		openavbRawsockTxSetHdr(pStream->rawsock, &hdrInfo);
		(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &hdrDoneNs);
		openavbRawsockTxSetMark(pStream->rawsock, pStream->tx_fwmark);
		(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &markDoneNs);

		pStream->pBuf = (U8 *)openavbRawsockGetTxFrame(pStream->rawsock, TRUE, &frameLen);
		(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &getDoneNs);
		if (hdrDoneNs >= acquireStartNs) {
			hdrDurationNs = hdrDoneNs - acquireStartNs;
		}
		if (markDoneNs >= hdrDoneNs) {
			markDurationNs = markDoneNs - hdrDoneNs;
		}
		if (getDoneNs >= markDoneNs) {
			getDurationNs = getDoneNs - markDoneNs;
		}
		if (getDoneNs >= acquireStartNs) {
			acquireDurationNs = getDoneNs - acquireStartNs;
		}
		if (acquireDurationNs >= AVTP_TX_ACQUIRE_WARN_NS && pStream->tx_acquire_log_count < 64) {
			memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
			streamUid = ntohs(streamUid);
			AVB_LOGF_WARNING(
				"AVTP TX ACQUIRE SLOW uid=%u subtype=0x%02x hdr=%lluns mark=%lluns get=%lluns total=%lluns fwmark=%u has_buf=%d",
				streamUid,
				pStream->subtype,
				(unsigned long long)hdrDurationNs,
				(unsigned long long)markDurationNs,
				(unsigned long long)getDurationNs,
				(unsigned long long)acquireDurationNs,
				pStream->tx_fwmark,
				pStream->pBuf ? 1 : 0);
			pStream->tx_acquire_log_count++;
		}
		if (pStream->pBuf) {
			assert(frameLen >= pStream->frameLen);
			// Fill in the Ethernet header
			openavbRawsockTxFillHdr(pStream->rawsock, pStream->pBuf, &pStream->ethHdrLen);
		}
	}

	if (pStream->pBuf) {
		// AVTP frame starts right after the Ethernet header
		pAvtpFrame = pFill = pStream->pBuf + pStream->ethHdrLen;
		avtpFrameLen = pStream->frameLen - pStream->ethHdrLen;

		// Fill the AVTP Header. This must be done before calling the interface and mapping modules.
		openavbRC rc = fillAvtpHdr(pStream, pFill);
		if (IS_OPENAVB_FAILURE(rc)) {
			AVB_RC_LOG_TRACE_RET(rc, AVB_TRACE_AVTP_DETAIL);
		}

		U64 timeNsec = 0;
		U64 intfStartNs = 0;
		U64 intfDoneNs = 0;
		U64 mapDoneNs = 0;
		U64 intfDurationNs = 0;
		U64 mapDurationNs = 0;
		U64 buildDurationNs = 0;

		(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &intfStartNs);

		if (!txBlockingInIntf) {
			// Call interface module to read data
			pStream->pIntfCB->intf_tx_cb(pStream->pMediaQ);
			(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &intfDoneNs);

#if IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
			// Prefer mapping-provided launch time when available (e.g. CRF),
			// otherwise use the media queue timestamp.
			bool haveLaunchTime = FALSE;
			if (pStream->pMapCB->map_lt_calc_cb) {
				haveLaunchTime = pStream->pMapCB->map_lt_calc_cb(pStream->pMediaQ, &timeNsec);
			}
			if (!haveLaunchTime) {
				media_q_item_t* item = openavbMediaQTailLock(pStream->pMediaQ, true);
				if (item) {
					timeNsec = item->pAvtpTime->timeNsec;
					openavbMediaQTailUnlock(pStream->pMediaQ);
				}
			}
#elif ATL_LAUNCHTIME_ENABLED
			if( pStream->pMapCB->map_lt_calc_cb ) {
				pStream->pMapCB->map_lt_calc_cb(pStream->pMediaQ, &timeNsec);
			}
#endif

			// Call mapping module to move data into AVTP frame
			txCBResult = pStream->pMapCB->map_tx_cb(pStream->pMediaQ, pAvtpFrame, &avtpFrameLen);
			(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &mapDoneNs);

			pStream->bytes += avtpFrameLen;
		}
		else {

#if IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
			bool haveLaunchTime = FALSE;
			if (pStream->pMapCB->map_lt_calc_cb) {
				haveLaunchTime = pStream->pMapCB->map_lt_calc_cb(pStream->pMediaQ, &timeNsec);
			}
			if (!haveLaunchTime) {
				media_q_item_t* item = openavbMediaQTailLock(pStream->pMediaQ, true);
				if (item) {
					timeNsec = item->pAvtpTime->timeNsec;
					openavbMediaQTailUnlock(pStream->pMediaQ);
				}
			}
#elif ATL_LAUNCHTIME_ENABLED
			if( pStream->pMapCB->map_lt_calc_cb ) {
				pStream->pMapCB->map_lt_calc_cb(pStream->pMediaQ, &timeNsec);
			}
#endif

			// Blocking in interface mode. Pull from media queue for tx first
			if ((txCBResult = pStream->pMapCB->map_tx_cb(pStream->pMediaQ, pAvtpFrame, &avtpFrameLen)) == TX_CB_RET_PACKET_NOT_READY) {
				(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &mapDoneNs);
				// Call interface module to read data
				pStream->pIntfCB->intf_tx_cb(pStream->pMediaQ);
				(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &intfDoneNs);
			}
			else {
				(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &mapDoneNs);
				intfDoneNs = mapDoneNs;
				pStream->bytes += avtpFrameLen;
			}
		}

		if (intfDoneNs >= intfStartNs) {
			intfDurationNs = intfDoneNs - intfStartNs;
		}
		if (mapDoneNs >= intfDoneNs) {
			mapDurationNs = mapDoneNs - intfDoneNs;
		}
		if (mapDoneNs >= intfStartNs) {
			buildDurationNs = mapDoneNs - intfStartNs;
		}

		avtpTxPathDiagUpdate(&pStream->tx_path_intf_min_ns, &pStream->tx_path_intf_max_ns,
			&pStream->tx_path_intf_sum_ns, pStream->tx_path_samples, intfDurationNs);
		avtpTxPathDiagUpdate(&pStream->tx_path_map_min_ns, &pStream->tx_path_map_max_ns,
			&pStream->tx_path_map_sum_ns, pStream->tx_path_samples, mapDurationNs);
		avtpTxPathDiagUpdate(&pStream->tx_path_build_min_ns, &pStream->tx_path_build_max_ns,
			&pStream->tx_path_build_sum_ns, pStream->tx_path_samples, buildDurationNs);
		pStream->tx_path_samples++;
		if ((intfDurationNs >= AVTP_TX_PATH_WARN_NS ||
				mapDurationNs >= AVTP_TX_PATH_WARN_NS ||
				buildDurationNs >= AVTP_TX_PATH_WARN_NS) &&
				pStream->tx_path_log_count < 32) {
			U16 streamUid = 0;
			memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
			streamUid = ntohs(streamUid);
			AVB_LOGF_WARNING(
				"TX OUTLIER ROW flags=.Y. stage=avtp_path stream=%u subtype=0x%02x intf_ns=%" PRIu64 " map_ns=%" PRIu64 " build_ns=%" PRIu64 " warn_ns=%u packet_ready=%u slow_intf=%u slow_map=%u slow_build=%u",
				streamUid,
				pStream->subtype,
				intfDurationNs,
				mapDurationNs,
				buildDurationNs,
				(unsigned)AVTP_TX_PATH_WARN_NS,
				(txCBResult != TX_CB_RET_PACKET_NOT_READY) ? 1u : 0u,
				(intfDurationNs >= AVTP_TX_PATH_WARN_NS) ? 1u : 0u,
				(mapDurationNs >= AVTP_TX_PATH_WARN_NS) ? 1u : 0u,
				(buildDurationNs >= AVTP_TX_PATH_WARN_NS) ? 1u : 0u);
			AVB_LOGF_WARNING(
				"AVTP TX PATH SLOW uid=%u subtype=0x%02x intf=%lluns map=%lluns build=%lluns packet_ready=%d",
				streamUid,
				pStream->subtype,
				(unsigned long long)intfDurationNs,
				(unsigned long long)mapDurationNs,
				(unsigned long long)buildDurationNs,
				(txCBResult != TX_CB_RET_PACKET_NOT_READY) ? 1 : 0);
			pStream->tx_path_log_count++;
		}
		avtpMaybeLogTxPathDiag(pStream);

		// If we got data from the mapping module and stream is not paused,
		// notify the raw sockets.
		if (txCBResult != TX_CB_RET_PACKET_NOT_READY && !pStream->bPause) {
			bool txTimestampValid = (pAvtpFrame[HIDX_AVTP_HIDE7_TV1] & 0x01) ? TRUE : FALSE;
			bool txTimestampUncertain = (pAvtpFrame[HIDX_AVTP_HIDE7_TU1] & 0x01) ? TRUE : FALSE;
			U8 txWireSeq = 0;
			bool haveTxWireSeq = avtpExtractTxSequence(pStream, pAvtpFrame, &txWireSeq);

			if (pStream->tsEval) {
				processTimestampEval(pStream, pAvtpFrame);
			}

			pStream->diag.frames_tx++;
			if (txTimestampValid) {
				pStream->diag.timestamp_valid++;
			}
			else {
				pStream->diag.timestamp_not_valid++;
			}
			if (pStream->tx_tu_valid && pStream->tx_last_tu != txTimestampUncertain) {
				pStream->diag.timestamp_uncertain++;
			}
			pStream->tx_tu_valid = TRUE;
			pStream->tx_last_tu = txTimestampUncertain;

			// Increment the sequence number now that we are sure this is a good packet.
			pStream->avtp_sequence_num++;
			// Mark the frame "ready to send".
			#if IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
				{
					U64 launchTimeNs = timeNsec;
					U64 tsDerivedLaunchNs = 0;
					U64 tsTimeNs = 0;
					bool haveMapLaunchTime = FALSE;
					bool haveTimestampLaunch = FALSE;

				if (pStream->pMapCB->map_lt_calc_cb) {
					haveMapLaunchTime = pStream->pMapCB->map_lt_calc_cb(pStream->pMediaQ, &launchTimeNs);
				}

					haveTimestampLaunch = avtpCalcLaunchTimeFromTimestamp(
						pStream,
						pAvtpFrame,
						&tsDerivedLaunchNs,
						&tsTimeNs);

					if (haveTimestampLaunch) {
						if (haveMapLaunchTime) {
							S64 observedOffsetNs = avtpSignedDeltaNs(launchTimeNs, tsDerivedLaunchNs);
							if (llabs(observedOffsetNs) <= 1000000LL) {
								pStream->tx_map_launch_offset_ns = observedOffsetNs;
								pStream->tx_map_launch_offset_valid = TRUE;
							}
							else {
								S64 fallbackOffsetNs = pStream->tx_map_launch_offset_valid
									? pStream->tx_map_launch_offset_ns
									: 0LL;
								U64 correctedLaunchNs = avtpApplySignedOffsetNs(tsDerivedLaunchNs, fallbackOffsetNs);
								if (pStream->tx_map_launch_mismatch_log_count < 32) {
									U16 streamUid = 0;
									memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
									streamUid = ntohs(streamUid);
									AVB_LOGF_WARNING(
										"TX OUTLIER ROW flags=.Y. stage=avtp stream=%u map_launch=%" PRIu64 " ts_launch=%" PRIu64 " final_launch=%" PRIu64 " ts=%" PRIu64 " now=0 observed_offset_ns=%" PRId64 " fallback_offset_ns=%" PRId64 " clamp=0 event=map_mismatch",
										streamUid,
										launchTimeNs,
										tsDerivedLaunchNs,
										correctedLaunchNs,
										tsTimeNs,
										observedOffsetNs,
										fallbackOffsetNs);
									AVB_LOGF_WARNING(
										"AVTP TX map launch mismatch: uid=%u subtype=0x%02x map_launch=%" PRIu64 " ts_launch=%" PRIu64 " observed_offset=%" PRId64 "ns fallback_offset=%" PRId64 "ns corrected_launch=%" PRIu64,
										streamUid,
										pStream->subtype,
										launchTimeNs,
										tsDerivedLaunchNs,
										observedOffsetNs,
										fallbackOffsetNs,
										correctedLaunchNs);
									pStream->tx_map_launch_mismatch_log_count++;
								}
								launchTimeNs = correctedLaunchNs;
							}
						}
						else {
							S64 fallbackOffsetNs = pStream->tx_map_launch_offset_valid
								? pStream->tx_map_launch_offset_ns
								: 0LL;
							launchTimeNs = avtpApplySignedOffsetNs(tsDerivedLaunchNs, fallbackOffsetNs);
						}
					}
					else if (!haveMapLaunchTime) {
						media_q_item_t* item = openavbMediaQTailLock(pStream->pMediaQ, true);
						if (item) {
							launchTimeNs = item->pAvtpTime->timeNsec;
							openavbMediaQTailUnlock(pStream->pMediaQ);
						}
					}

					{
						U64 nowNs = 0;
						U64 originalLaunchTimeNs = launchTimeNs;
						CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
						U64 tsLeadNs = (tsTimeNs > 0 && tsTimeNs >= nowNs) ? (tsTimeNs - nowNs) :
							(tsTimeNs > 0 ? 0ULL : 0ULL);

						// Guard against pathological launch targets (seconds behind/ahead of wall clock)
						// after scheduling stalls. Rebase to a bounded lead to avoid bursts of early/late packets.
						U64 safeLeadNs = pStream->max_transit_usec * 1000ULL;
						if (safeLeadNs < 200000ULL) {
							safeLeadNs = 200000ULL;     // 0.2 ms minimum lead
						}
						if (safeLeadNs > 20000000ULL) {
							safeLeadNs = 20000000ULL;   // 20 ms maximum lead
						}

						int64_t deltaLaunchNs = (int64_t)(launchTimeNs - nowNs);
						if (deltaLaunchNs < -5000000LL || deltaLaunchNs > ((int64_t)safeLeadNs + 50000000LL)) {
							launchTimeNs = nowNs + safeLeadNs;
							deltaLaunchNs = (int64_t)(launchTimeNs - nowNs);
							pStream->tx_path_clamp_count++;
							if (pStream->tx_path_log_count < 64) {
								U16 streamUid = 0;
								memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
								streamUid = ntohs(streamUid);
								AVB_LOGF_WARNING(
									"TX OUTLIER ROW flags=.Y. stage=avtp stream=%u map_launch=%" PRIu64 " ts_launch=%" PRIu64 " final_launch=%" PRIu64 " ts=%" PRIu64 " now=%" PRIu64 " observed_offset_ns=%" PRId64 " fallback_offset_ns=0 clamp=1 event=launch_clamp",
									streamUid,
									originalLaunchTimeNs,
									tsDerivedLaunchNs,
									launchTimeNs,
									tsTimeNs,
									nowNs,
									(int64_t)(originalLaunchTimeNs - nowNs),
									0LL);
								AVB_LOGF_WARNING(
									"AVTP TX launch clamped: uid=%u subtype=0x%02x original_launch=%" PRIu64 " clamped_launch=%" PRIu64 " now=%" PRIu64 " original_delta=%" PRId64 "ns final_delta=%" PRId64 "ns ts=%" PRIu64 " ts_lead=%" PRIu64 "ns safe_lead=%" PRIu64 "ns intf=%lluns map=%lluns build=%lluns",
									streamUid,
									pStream->subtype,
									originalLaunchTimeNs,
									launchTimeNs,
									nowNs,
									(int64_t)(originalLaunchTimeNs - nowNs),
									deltaLaunchNs,
									tsTimeNs,
									tsLeadNs,
									safeLeadNs,
									(unsigned long long)intfDurationNs,
									(unsigned long long)mapDurationNs,
									(unsigned long long)buildDurationNs);
								pStream->tx_path_log_count++;
							}
						}

						if (pStream->tx_launch_log_count < 16 ||
								deltaLaunchNs < 500000LL ||
								deltaLaunchNs > (int64_t)(safeLeadNs + 5000000ULL)) {
							U16 streamUid = 0;
							memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
							streamUid = ntohs(streamUid);
							AVB_LOGF_INFO(
								"AVTP TX launch window: uid=%u subtype=0x%02x launch=%" PRIu64 " now=%" PRIu64 " delta=%" PRId64 "ns ts=%" PRIu64 " ts_lead=%" PRIu64 "ns safe_lead=%" PRIu64 "ns map_lt=%d",
								streamUid,
								pStream->subtype,
								launchTimeNs,
								nowNs,
								deltaLaunchNs,
								tsTimeNs,
								tsLeadNs,
								safeLeadNs,
								haveMapLaunchTime ? 1 : 0);
							pStream->tx_launch_log_count++;
						}

						if (pStream->subtype == AVTP_CRF_SUBTYPE) {
							U64 submitAdvanceNs = ((U64)pStream->tx_submit_ahead_usec +
								(U64)pStream->tx_submit_skew_usec) * 1000ULL;
							U64 warnLeadNs = submitAdvanceNs / 2ULL;
							const char *event = NULL;

							if (warnLeadNs < 250000ULL) {
								warnLeadNs = 250000ULL;
							}

							if (deltaLaunchNs < 0) {
								event = "late_launch";
							}
							else if ((U64)deltaLaunchNs < warnLeadNs) {
								event = "low_lead";
							}

							if (event && avtpShouldLogSparse(pStream->tx_launch_margin_log_count)) {
								U16 streamUid = 0;
								memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
								streamUid = ntohs(streamUid);
								AVB_LOGF_WARNING(
									"TX OUTLIER ROW flags=.Y. stage=avtp_margin event=%s stream=%u subtype=0x%02x launch=%" PRIu64 " now=%" PRIu64 " launch_lead_ns=%" PRId64 " warn_lead_ns=%" PRIu64 " submit_ahead_ns=%" PRIu64 " submit_skew_ns=%" PRIu64 " ts=%" PRIu64 " ts_lead_ns=%" PRIu64 " map_launch=%" PRIu64 " map_lt=%u intf_ns=%" PRIu64 " map_ns=%" PRIu64 " build_ns=%" PRIu64,
									event,
									streamUid,
									pStream->subtype,
									launchTimeNs,
									nowNs,
									deltaLaunchNs,
									warnLeadNs,
									submitAdvanceNs,
									(U64)pStream->tx_submit_skew_usec * 1000ULL,
									tsTimeNs,
									tsLeadNs,
									originalLaunchTimeNs,
									haveMapLaunchTime ? 1u : 0u,
									intfDurationNs,
									mapDurationNs,
									buildDurationNs);
							}
							if (event) {
								pStream->tx_launch_margin_log_count++;
							}
						}
					}
					if (pStream->subtype == AVTP_CRF_SUBTYPE && haveTxWireSeq) {
						if (pStream->tx_emit_seq_valid) {
							U8 expectedSeq = (U8)(pStream->tx_last_emit_seq + 1);
							if (txWireSeq != expectedSeq && pStream->tx_emit_gap_log_count < 64) {
								U16 streamUid = 0;
								U8 lost = (U8)(txWireSeq - expectedSeq);
								memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
								streamUid = ntohs(streamUid);
								AVB_LOGF_WARNING(
									"TX OUTLIER ROW flags=..Z stage=crf_emit stream=%u subtype=0x%02x seq=%u expected=%u lost=%u launch=%" PRIu64 " packet_ready=1 bSend=%u event=seq_gap_before_send",
									streamUid,
									pStream->subtype,
									(unsigned)txWireSeq,
									(unsigned)expectedSeq,
									(unsigned)lost,
									launchTimeNs,
									bSend ? 1u : 0u);
								pStream->tx_emit_gap_log_count++;
							}
						}
						pStream->tx_emit_seq_valid = TRUE;
						pStream->tx_last_emit_seq = txWireSeq;
					}
					{
						U64 sendStageStartNs = 0;
						U64 sendStageMidNs = 0;
						U64 sendStageReadyNs = 0;
						U64 sendStageEndNs = 0;
						U64 paceDurationNs = 0;
						U64 txReadyDurationNs = 0;
						U64 sendDurationNs = 0;

						(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &sendStageStartNs);
						paceDurationNs = avtpMaybePaceSubmit(pStream, launchTimeNs);
						(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &sendStageMidNs);
						openavbRawsockTxFrameReady(pStream->rawsock, pStream->pBuf, avtpFrameLen + pStream->ethHdrLen, launchTimeNs);
						(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &sendStageReadyNs);
						if (sendStageReadyNs >= sendStageMidNs) {
							txReadyDurationNs = sendStageReadyNs - sendStageMidNs;
						}
						if (bSend) {
							openavbRawsockSend(pStream->rawsock);
						}
						(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &sendStageEndNs);
						if (sendStageEndNs >= sendStageReadyNs) {
							sendDurationNs = sendStageEndNs - sendStageReadyNs;
						}
						if ((paceDurationNs >= 5000000ULL ||
								txReadyDurationNs >= 5000000ULL ||
								sendDurationNs >= 5000000ULL) &&
								pStream->tx_send_warn_log_count < 32) {
							U16 streamUid = 0;
							memcpy(&streamUid, pStream->streamIDnet + ETH_ALEN, sizeof(streamUid));
							streamUid = ntohs(streamUid);
							AVB_LOGF_WARNING(
								"AVTP TX send stage slow: uid=%u subtype=0x%02x pace=%" PRIu64 "ns ready=%" PRIu64 "ns send=%" PRIu64 "ns bSend=%d launch=%" PRIu64,
								streamUid,
								pStream->subtype,
								paceDurationNs,
								txReadyDurationNs,
								sendDurationNs,
								bSend ? 1 : 0,
								launchTimeNs);
							pStream->tx_send_warn_log_count++;
						}
					}
				}
			#else
			openavbRawsockTxFrameReady(pStream->rawsock, pStream->pBuf, avtpFrameLen + pStream->ethHdrLen, timeNsec);
			#endif
			// Send if requested
			#if !(IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED)
			if (bSend)
				openavbRawsockSend(pStream->rawsock);
			#endif
			// Drop our reference to it
			pStream->pBuf = NULL;
		}
		else {
			// If this cycle didn't produce a packet, release any checked-out TX buffer.
			// This prevents buffersOut drifting ahead of buffersReady when batching.
			if (pStream->pBuf) {
				(void)openavbRawsockRelTxFrame(pStream->rawsock, pStream->pBuf);
				pStream->pBuf = NULL;
			}

			// End-of-cycle flush: if prior frames were queued in this wake, send them now
			// even when the last map callback reported packet-not-ready.
			if (bSend) {
				int pending = openavbRawsockTxBufLevel(pStream->rawsock);
				if (pending > 0) {
					(void)openavbRawsockSend(pStream->rawsock);
				}
			}
			AVB_RC_TRACE_RET(OPENAVB_AVTP_FAILURE, AVB_TRACE_AVTP_DETAIL);
		}
	}
	else {
		AVB_RC_TRACE_RET(OPENAVB_AVTP_FAILURE, AVB_TRACE_AVTP_DETAIL);
	}

	AVB_RC_TRACE_RET(OPENAVB_AVTP_SUCCESS, AVB_TRACE_AVTP_DETAIL);
}

openavbRC openavbAvtpRxInit(
	media_q_t *pMediaQ,
	openavb_map_cb_t *pMapCB,
	openavb_intf_cb_t *pIntfCB,
	char *ifname,
	AVBStreamID_t *streamID,
	U8 *daddr,
	U16 nbuffers,
	bool rxSignalMode,
	void **pStream_out)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);
	AVB_LOG_DEBUG("Initialize");

	*pStream_out = NULL;

	if (!pMapCB) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_MAPPING_CB_NOT_SET), AVB_TRACE_AVTP);
	}
	if (!pIntfCB) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_INTERFACE_CB_NOT_SET), AVB_TRACE_AVTP);
	}
	if (!daddr) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT), AVB_TRACE_AVTP);
	}

	avtp_stream_t *pStream = calloc(1, sizeof(avtp_stream_t));
	if (!pStream) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_OUT_OF_MEMORY), AVB_TRACE_AVTP);
	}
	pStream->tx = FALSE;
	pStream->nLost = -1;

	pStream->pMediaQ = pMediaQ;
	pStream->pMapCB = pMapCB;
	pStream->pIntfCB = pIntfCB;

	pStream->pMapCB->map_rx_init_cb(pStream->pMediaQ);
	pStream->pIntfCB->intf_rx_init_cb(pStream->pMediaQ);
	if (pStream->pIntfCB->intf_set_stream_uid_cb) {
		pStream->pIntfCB->intf_set_stream_uid_cb(pStream->pMediaQ, streamID->uniqueID);
	}

	// Set the frame length
	pStream->frameLen = pStream->pMapCB->map_max_data_size_cb(pStream->pMediaQ) + ETH_HDR_LEN_VLAN;

	// Save the streamID
	memcpy(pStream->streamIDnet, streamID->addr, ETH_ALEN);
        U16 *pStreamUID = (U16 *)((U8 *)(pStream->streamIDnet) + ETH_ALEN);
       *pStreamUID = htons(streamID->uniqueID);

	// and the destination MAC address
	memcpy(pStream->dest_addr.ether_addr_octet, daddr, ETH_ALEN);

	// and other stuff needed to (re)open the socket
	pStream->ifname = strdup(ifname);
	pStream->nbuffers = nbuffers;
	pStream->bRxSignalMode = rxSignalMode;

	openavbRC rc = openAvtpSock(pStream);
	if (IS_OPENAVB_FAILURE(rc)) {
		free(pStream);
		AVB_RC_LOG_TRACE_RET(rc, AVB_TRACE_AVTP);
	}

	// Save the AVTP subtype
	pStream->subtype = pStream->pMapCB->map_subtype_cb();

	*pStream_out = (void *)pStream;
	AVB_RC_TRACE_RET(OPENAVB_AVTP_SUCCESS, AVB_TRACE_AVTP);
}

static void x_avtpRxFrame(avtp_stream_t *pStream, U8 *pFrame, U32 frameLen)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP_DETAIL);
	IF_LOG_INTERVAL(4096) AVB_LOGF_DEBUG("pFrame=%p, len=%u", pFrame, frameLen);
	U8 subtype, flags, flags2, rxSeq, nLost, avtpVersion;
	U8 *pRead = pFrame;

	// AVTP Header
	//
	// Check control/data bit.  We only expect data packets.
	if (0 == (*pRead & 0x80)) {
		// - 7 bits 	subtype
		subtype = *pRead++ & 0x7F;
		flags   = *pRead++;
		avtpVersion = (flags >> 4) & 0x07;

		// Check AVTPDU version, BZ 106
		if (0 == avtpVersion) {
			bool mediaRestart;
			bool timestampValid;
			bool timestampUncertain;
			U32 avtpTimestamp;

			if (subtype != pStream->subtype) {
				pStream->diag.unsupported_format++;
				AVB_TRACE_EXIT(AVB_TRACE_AVTP_DETAIL);
				return;
			}

			rxSeq = *pRead++;

			if (pStream->nLost == -1) {
				// first frame received, don't check for mismatch
				pStream->nLost = 0;
			}
			else if (pStream->avtp_sequence_num != rxSeq) {
				nLost = (rxSeq - pStream->avtp_sequence_num)
					+ (rxSeq < pStream->avtp_sequence_num ? 256 : 0);
				AVB_LOGF_INFO("AVTP sequence mismatch: expected: %3u,\tgot: %3u,\tlost %3d",
					pStream->avtp_sequence_num, rxSeq, nLost);
				pStream->nLost += nLost;
				pStream->diag.seq_num_mismatch++;
			}
			pStream->avtp_sequence_num = rxSeq + 1;

			pStream->bytes += frameLen;

			flags2 = *pRead++;
			mediaRestart = (flags & 0x08) ? TRUE : FALSE;
			timestampValid = (flags & 0x01) ? TRUE : FALSE;
			timestampUncertain = (flags2 & 0x01) ? TRUE : FALSE;
			avtpTimestamp = ntohl(*(U32 *)(&pFrame[HIDX_AVTP_TIMESPAMP32]));

			pStream->diag.frames_rx++;
			if (timestampValid) {
				pStream->diag.timestamp_valid++;
			}
			else {
				pStream->diag.timestamp_not_valid++;
			}
			if (pStream->rx_mr_valid && pStream->rx_last_mr != mediaRestart) {
				pStream->diag.media_reset++;
			}
			pStream->rx_mr_valid = TRUE;
			pStream->rx_last_mr = mediaRestart;
			if (pStream->rx_tu_valid && pStream->rx_last_tu != timestampUncertain) {
				pStream->diag.timestamp_uncertain++;
			}
			pStream->rx_tu_valid = TRUE;
			pStream->rx_last_tu = timestampUncertain;
			openavbAvtpCountLateEarlyTimestamp(
				pStream,
				avtpTimestamp,
				timestampValid,
				timestampUncertain);
			IF_LOG_INTERVAL(4096) AVB_LOGF_DEBUG("subtype=%u, sv=%u, ver=%u, mr=%u, tv=%u tu=%u",
				subtype, flags & 0x80, avtpVersion,
				flags & 0x08, flags & 0x01, flags2 & 0x01);

			pRead += 8;

			if (pStream->tsEval) {
				processTimestampEval(pStream, pFrame);
			}

			pStream->pMapCB->map_rx_cb(pStream->pMediaQ, pFrame, frameLen);

			// NOTE : This is a redundant call. It is handled in avtpTryRx()
			// pStream->pIntfCB->intf_rx_cb(pStream->pMediaQ);

			pStream->info.rx.bComplete = TRUE;

			// to prevent unused variable warnings
			(void)subtype;
			(void)flags2;
		}
		else {
			AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_INVALID_AVTP_VERSION));
		}
	}
	else {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_IGNORING_CONTROL_PACKET));
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVTP_DETAIL);
}

/*
 * Try to receive some data.
 *
 * Keeps state information in pStream.
 * Look at pStream->info for the received data.
 */
static void avtpTryRx(avtp_stream_t *pStream)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP_DETAIL);

	U8         *pBuf = NULL;   // pointer to buffer containing rcvd frame, if any
	U8         *pAvtpPdu;      // pointer to AVTP PDU within Ethernet frame
	U32         offsetToFrame; // offset into pBuf where Ethernet frame begins (bytes)
	U32         frameLen;      // length of the Ethernet frame (bytes)
	int         hdrLen;        // length of the Ethernet frame header (bytes)
	U32         avtpPduLen;    // length of the AVTP PDU (bytes)
	hdr_info_t  hdrInfo;       // Ethernet header contents
	U32         timeout;

	while (!pBuf) {
		if (!openavbMediaQUsecTillTail(pStream->pMediaQ, &timeout)) {
			// No mediaQ item available therefore wait for a new packet
			timeout = AVTP_MAX_BLOCK_USEC;
			pBuf = (U8 *)openavbRawsockGetRxFrame(pStream->rawsock, timeout, &offsetToFrame, &frameLen);
			if (!pBuf) {
				AVB_TRACE_EXIT(AVB_TRACE_AVTP_DETAIL);
				return;
			}
		}
		else if (timeout == 0) {
			// Process the pending media queue item and after check for available incoming packets
			pStream->pIntfCB->intf_rx_cb(pStream->pMediaQ);

			// Previously would check for new packets but disabled to favor presentation times.
			// pBuf = (U8 *)openavbRawsockGetRxFrame(pStream->rawsock, OPENAVB_RAWSOCK_NONBLOCK, &offsetToFrame, &frameLen);
		}
		else {
			if (timeout > AVTP_MAX_BLOCK_USEC)
				timeout = AVTP_MAX_BLOCK_USEC;
			if (timeout < RAWSOCK_MIN_TIMEOUT_USEC)
				timeout = RAWSOCK_MIN_TIMEOUT_USEC;

			pBuf = (U8 *)openavbRawsockGetRxFrame(pStream->rawsock, timeout, &offsetToFrame, &frameLen);
			if (!pBuf)
				pStream->pIntfCB->intf_rx_cb(pStream->pMediaQ);
		}
	}

	hdrLen = openavbRawsockRxParseHdr(pStream->rawsock, pBuf, &hdrInfo);
	if (hdrLen < 0) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_PARSING_FRAME_HEADER));
	}
	else {
		pAvtpPdu = pBuf + offsetToFrame + hdrLen;
		avtpPduLen = frameLen - hdrLen;
		x_avtpRxFrame(pStream, pAvtpPdu, avtpPduLen);
	}
	openavbRawsockRelRxFrame(pStream->rawsock, pBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVTP_DETAIL);
}

int openavbAvtpTxBufferLevel(void *pv)
{
	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (!pStream) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT));
		return 0;
	}
	return openavbRawsockTxBufLevel(pStream->rawsock);
}

int openavbAvtpRxBufferLevel(void *pv)
{
	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (!pStream) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT));
		return 0;
	}
	return openavbRawsockRxBufLevel(pStream->rawsock);
}

int openavbAvtpLost(void *pv)
{
	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (!pStream) {
		// Quietly return. Since this can be called before a stream is available.
		return 0;
	}
	int count = pStream->nLost;
	pStream->nLost = 0;
	if (count < 0) {
		return 0;
	}
	return count;
}

U64 openavbAvtpBytes(void *pv)
{
	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (!pStream) {
		// Quietly return. Since this can be called before a stream is available.
		return 0;
	}

	U64 bytes = pStream->bytes;
	pStream->bytes = 0;
	return bytes;
}

void openavbAvtpGetDiagCounters(void *pv, openavb_avtp_diag_counters_t *pCounters)
{
	avtp_stream_t *pStream = (avtp_stream_t *)pv;

	if (!pCounters) {
		return;
	}

	memset(pCounters, 0, sizeof(*pCounters));
	if (!pStream) {
		return;
	}

	*pCounters = pStream->diag;
}

openavbRC openavbAvtpRx(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP_DETAIL);

	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (!pStream) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT), AVB_TRACE_AVTP_DETAIL);
	}

	// Check our socket, and potentially receive some data.
	avtpTryRx(pStream);

	// See if there's a complete (re-assembled) data sample.
	if (pStream->info.rx.bComplete) {
		pStream->info.rx.bComplete = FALSE;
		AVB_RC_TRACE_RET(OPENAVB_AVTP_SUCCESS, AVB_TRACE_AVTP_DETAIL);
	}

	AVB_RC_TRACE_RET(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVBAVTP_RC_NO_FRAMES_PROCESSED), AVB_TRACE_AVTP_DETAIL);
}

void openavbAvtpConfigTimsstampEval(void *handle, U32 tsInterval, U32 reportInterval, bool smoothing, U32 tsMaxJitter, U32 tsMaxDrift)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);

	avtp_stream_t *pStream = (avtp_stream_t *)handle;
	if (!pStream) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT));
		AVB_TRACE_EXIT(AVB_TRACE_AVTP);
		return;
	}

	pStream->tsEval = openavbTimestampEvalNew();
	openavbTimestampEvalInitialize(pStream->tsEval, tsInterval);
	openavbTimestampEvalSetReport(pStream->tsEval, reportInterval);
	if (smoothing) {
		openavbTimestampEvalSetSmoothing(pStream->tsEval, tsMaxJitter, tsMaxDrift);
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVTP);
}

void openavbAvtpPause(void *handle, bool bPause)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);

	avtp_stream_t *pStream = (avtp_stream_t *)handle;
	if (!pStream) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT));
		AVB_TRACE_EXIT(AVB_TRACE_AVTP);
		return;
	}

	pStream->bPause = bPause;

	// AVDECC_TODO:  Do something with the bPause value!

	AVB_TRACE_EXIT(AVB_TRACE_AVTP);
}

void openavbAvtpRequestMediaRestart(void *handle)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);

	avtp_stream_t *pStream = (avtp_stream_t *)handle;
	if (!pStream) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVTP_FAILURE | OPENAVB_RC_INVALID_ARGUMENT));
		AVB_TRACE_EXIT(AVB_TRACE_AVTP);
		return;
	}

	pStream->media_restart_toggle = !pStream->media_restart_toggle;
	pStream->diag.media_reset++;
	AVB_LOGF_INFO("AVTP media restart toggled: mr=%u", pStream->media_restart_toggle ? 1 : 0);

	AVB_TRACE_EXIT(AVB_TRACE_AVTP);
}

void openavbAvtpShutdownTalker(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);
	AVB_LOG_DEBUG("Shutdown");

	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (pStream) {
		pStream->pIntfCB->intf_end_cb(pStream->pMediaQ);
		pStream->pMapCB->map_end_cb(pStream->pMediaQ);

		// close the rawsock
		if (pStream->rawsock && pStream->owns_rawsock) {
			openavbRawsockClose(pStream->rawsock);
		}
		pStream->rawsock = NULL;

		if (pStream->ifname)
			free(pStream->ifname);

		// free the malloc'd stream info
		free(pStream);
	}
	AVB_TRACE_EXIT(AVB_TRACE_AVTP);
	return;
}

void openavbAvtpShutdownListener(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVTP);
	AVB_LOG_DEBUG("Shutdown");

	avtp_stream_t *pStream = (avtp_stream_t *)pv;
	if (pStream) {
		// close the rawsock
		if (pStream->rawsock && pStream->owns_rawsock) {
			openavbRawsockClose(pStream->rawsock);
		}
		pStream->rawsock = NULL;

		pStream->pIntfCB->intf_end_cb(pStream->pMediaQ);
		pStream->pMapCB->map_end_cb(pStream->pMediaQ);

		if (pStream->ifname)
			free(pStream->ifname);

		// free the malloc'd stream info
		free(pStream);
	}
	AVB_TRACE_EXIT(AVB_TRACE_AVTP);
	return;
}
