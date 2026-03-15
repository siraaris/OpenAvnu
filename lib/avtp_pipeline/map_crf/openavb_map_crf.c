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
* MODULE SUMMARY : CRF (Clock Reference Format) mapping module.
*
* Generates AVTP CRF packets according to IEEE 1722 CRF format.
*/

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <endian.h>
#include <math.h>

#include "openavb_types_pub.h"
#include "openavb_trace_pub.h"
#include "openavb_avtp_time_pub.h"
#include "openavb_mediaq_pub.h"
#include "openavb_map_pub.h"
#include "openavb_map_crf_pub.h"
#include "openavb_clock_source_runtime_pub.h"
#include "openavb_aem_types_pub.h"
#include "openavb_time_osal_pub.h"

#define AVB_LOG_COMPONENT "CRF Mapping"
#include "openavb_log_pub.h"

#define AVTP_CRF_SUBTYPE 0x04

#define AVTP_CRF_TYPE_AUDIO_SAMPLE 0x01

#define AVTP_CRF_PULL_MULT_BY_1 0x00

#define SHIFT_SV (31 - 8)
#define SHIFT_MR (31 - 12)
#define SHIFT_FS (31 - 14)
#define SHIFT_TU (31 - 15)
#define SHIFT_SEQ_NUM (31 - 23)

#define SHIFT_PULL (63 - 2)
#define SHIFT_BASE_FREQ (63 - 31)
#define SHIFT_CRF_DATA_LEN (63 - 47)

#define MASK_BASE_FREQ 0x1FFFFFFF
#define MASK_DATA_LEN 0xFFFF
#define MASK_TIMESTAMP_INTERVAL 0xFFFF

#define NSEC_PER_SEC 1000000000ULL
#define CRF_DIAG_DEFAULT_JITTER_THRESH_NS 250000ULL
#define CRF_DIAG_JUMP_FACTOR 4ULL

typedef struct {
	/////////////
	// Config data
	/////////////
	U32 baseFreq;
	U16 timestampInterval;
	U16 timestampsPerPdu;
	U8 pull;
	U8 type;
	U32 txInterval;
	bool txIntervalOverride;
	U32 launchLeadUsec;
	S32 periodAdjustPpb;

	/////////////
	// Variable data
	/////////////
	U32 maxTransitUsec;
	U32 dataLen;
	U32 maxDataSize;
	U64 samplePeriodNs;
	U64 crfPeriodNs;
	U64 roundedMttNs;
	U64 launchLeadNs;
	U64 nextCrfTimeNs;
	bool nextCrfTimeValid;
	U64 lastLaunchTimeNs;
	bool lastLaunchValid;
	U8 seqNum;
	U8 rxExpectedSeqNum;
	bool rxExpectedSeqValid;
	bool isTalker;
	bool rxBypassQueue;

	/////////////
	// Diagnostics
	/////////////
	bool diagEnable;
	U32 diagLogEveryPackets;
	U64 diagJitterThreshNs;
	U64 diagPacketsTx;
	U64 diagPacketsRx;
	U64 diagSeqGapCount;
	U64 diagTsJitterCount;
	U64 diagTsJumpCount;
	U64 diagTsNonMonotonicCount;
	U64 diagTxCatchupCount;
	U64 diagTxCatchupPeriods;
	U64 lastTxFirstCrfNs;
	bool lastTxFirstCrfValid;
	U64 lastRxFirstCrfNs;
	bool lastRxFirstCrfValid;
	bool diagTxLeadWindowValid;
	S64 diagTxLeadErrMinNs;
	S64 diagTxLeadErrMaxNs;
	S64 diagTxLeadErrSumNs;
	S64 diagTxLeadErrAbsMaxNs;
	S64 diagTxLaunchSkewAbsMaxNs;
	U64 diagTxLeadWindowPackets;
	U64 diagTxLeadLogCount;
	U16 runtimeLocationIndex;
	bool runtimeLocationConfigured;
	U32 recoveryGeneration;
} pvt_data_t;

struct avtp_crf_pdu {
	U32 subtype_data;
	U64 stream_id;
	U64 packet_info;
	U64 crf_data[0];
} __attribute__ ((__packed__));

static void crfUpdateDerived(pvt_data_t *pPvtData)
{
	if (!pPvtData)
		return;

	if (pPvtData->baseFreq == 0)
		pPvtData->baseFreq = 48000;
	if (pPvtData->timestampInterval == 0)
		pPvtData->timestampInterval = 96;
	if (pPvtData->timestampsPerPdu == 0)
		pPvtData->timestampsPerPdu = 1;

	pPvtData->dataLen = pPvtData->timestampsPerPdu * sizeof(U64);
	pPvtData->maxDataSize = sizeof(struct avtp_crf_pdu) + pPvtData->dataLen;

	if (!pPvtData->txIntervalOverride) {
		double interval = (double)pPvtData->baseFreq /
			((double)pPvtData->timestampInterval * (double)pPvtData->timestampsPerPdu);
		pPvtData->txInterval = (U32)(interval + 0.5);
	}

	double samplePeriod = (double)NSEC_PER_SEC / (double)pPvtData->baseFreq;
	// Compute CRF period directly from frequency ratio to avoid cumulative
	// quantization error (e.g., 48 kHz / 96 should be exactly 2,000,000 ns).
	double crfPeriod = ((double)NSEC_PER_SEC * (double)pPvtData->timestampInterval) / (double)pPvtData->baseFreq;
	if (pPvtData->periodAdjustPpb != 0) {
		crfPeriod *= (1.0 + ((double)pPvtData->periodAdjustPpb / 1000000000.0));
	}
	double mttNs = (double)pPvtData->maxTransitUsec * 1000.0;

	pPvtData->samplePeriodNs = (U64)llround(samplePeriod);
	pPvtData->crfPeriodNs = (U64)llround(crfPeriod);
	pPvtData->roundedMttNs = (U64)llround(ceil(mttNs / samplePeriod) * samplePeriod);
	pPvtData->launchLeadNs = (U64)pPvtData->launchLeadUsec * 1000ULL;

	pPvtData->nextCrfTimeValid = FALSE;
}

static U16 crfRuntimeResolveLocationIndex(pvt_data_t *pPvtData, U16 locationType)
{
	if (!pPvtData) {
		return OPENAVB_AEM_DESCRIPTOR_INVALID;
	}

	if (pPvtData->runtimeLocationConfigured) {
		return pPvtData->runtimeLocationIndex;
	}

	openavb_clock_source_runtime_t selection = {0};
	if (openavbClockSourceRuntimeGetSelection(&selection) &&
			(selection.clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID) &&
			selection.clock_source_location_type == locationType &&
			selection.clock_source_location_index != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		pPvtData->runtimeLocationIndex = selection.clock_source_location_index;
	}

	return pPvtData->runtimeLocationIndex;
}

// Each configuration name value pair for this mapping will result in this callback being called.
void openavbMapCrfCfgCB(media_q_t *pMediaQ, const char *name, const char *value)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);

	if (pMediaQ) {
		char *pEnd;
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return;
		}

		if (strcmp(name, "map_nv_crf_base_freq") == 0 || strcmp(name, "crf_base_freq") == 0) {
			pPvtData->baseFreq = strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_timestamp_interval") == 0 || strcmp(name, "crf_timestamp_interval") == 0) {
			pPvtData->timestampInterval = (U16)strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_timestamps_per_pdu") == 0 || strcmp(name, "crf_timestamps_per_pdu") == 0) {
			pPvtData->timestampsPerPdu = (U16)strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_pull") == 0 || strcmp(name, "crf_pull") == 0) {
			pPvtData->pull = (U8)strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_type") == 0 || strcmp(name, "crf_type") == 0) {
			pPvtData->type = (U8)strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_tx_rate") == 0 || strcmp(name, "map_nv_tx_interval") == 0) {
			pPvtData->txInterval = strtoul(value, &pEnd, 10);
			pPvtData->txIntervalOverride = TRUE;
		}
		else if (strcmp(name, "map_nv_crf_launch_lead_usec") == 0 || strcmp(name, "crf_launch_lead_usec") == 0) {
			pPvtData->launchLeadUsec = strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_period_adjust_ppb") == 0 || strcmp(name, "crf_period_adjust_ppb") == 0) {
			pPvtData->periodAdjustPpb = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_diag_enable") == 0 || strcmp(name, "crf_diag_enable") == 0) {
			pPvtData->diagEnable = (strtol(value, &pEnd, 10) != 0);
		}
		else if (strcmp(name, "map_nv_crf_diag_log_every_packets") == 0 || strcmp(name, "crf_diag_log_every_packets") == 0) {
			pPvtData->diagLogEveryPackets = (U32)strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_diag_jitter_thresh_ns") == 0 || strcmp(name, "crf_diag_jitter_thresh_ns") == 0) {
			pPvtData->diagJitterThreshNs = strtoull(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_rx_bypass_queue") == 0 || strcmp(name, "crf_rx_bypass_queue") == 0) {
			pPvtData->rxBypassQueue = (strtol(value, &pEnd, 10) != 0);
		}

		crfUpdateDerived(pPvtData);
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

// Returns the AVB subtype for this mapping
U8 openavbMapCrfSubtypeCB()
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return AVTP_CRF_SUBTYPE;
}

// Returns the AVTP version used by this mapping
U8 openavbMapCrfAvtpVersionCB()
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);
	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return 0x00;
}

// Returns the max data size
U16 openavbMapCrfMaxDataSizeCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ && pMediaQ->pPvtMapInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		AVB_TRACE_EXIT(AVB_TRACE_MAP);
		return (U16)pPvtData->maxDataSize;
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return 0;
}

U32 openavbMapCrfTransmitIntervalCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ && pMediaQ->pPvtMapInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		AVB_TRACE_EXIT(AVB_TRACE_MAP);
		return pPvtData->txInterval;
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return 0;
}

void openavbMapCrfGenInitCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

void openavbMapCrfTxInitCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ && pMediaQ->pPvtMapInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		U16 runtimeLocation = crfRuntimeResolveLocationIndex(
			pPvtData,
			OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT);
		pPvtData->isTalker = TRUE;
		pPvtData->seqNum = 0;
		pPvtData->nextCrfTimeValid = FALSE;
		pPvtData->lastLaunchValid = FALSE;
		pPvtData->lastTxFirstCrfValid = FALSE;
		pPvtData->diagPacketsTx = 0;
		pPvtData->diagTxLeadWindowValid = FALSE;
		pPvtData->diagTxLeadErrMinNs = 0;
		pPvtData->diagTxLeadErrMaxNs = 0;
		pPvtData->diagTxLeadErrSumNs = 0;
		pPvtData->diagTxLeadErrAbsMaxNs = 0;
		pPvtData->diagTxLaunchSkewAbsMaxNs = 0;
		pPvtData->diagTxLeadWindowPackets = 0;
		pPvtData->diagTxLeadLogCount = 0;
		openavbClockSourceRuntimeClearCrfTimeForLocation(
			OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
			runtimeLocation);
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

static bool crfGetTime(struct timespec *timeSpec, bool *isUncertain)
{
	if (CLOCK_GETTIME(OPENAVB_CLOCK_WALLTIME, timeSpec)) {
		if (isUncertain)
			*isUncertain = FALSE;
		return TRUE;
	}

	if (CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, timeSpec)) {
		if (isUncertain)
			*isUncertain = TRUE;
		return TRUE;
	}

	if (isUncertain)
		*isUncertain = TRUE;
	return FALSE;
}

static U64 crfTimespecToNs(const struct timespec *timeSpec)
{
	return ((U64)timeSpec->tv_sec * (U64)NSEC_PER_SEC) + (U64)timeSpec->tv_nsec;
}

static U64 crfExpectedPduDeltaNs(U32 baseFreq, U16 timestampInterval, U16 timestampsPerPdu, const pvt_data_t *pPvtData)
{
	if (baseFreq > 0 && timestampInterval > 0 && timestampsPerPdu > 0) {
		double crfPeriod = ((double)NSEC_PER_SEC * (double)timestampInterval) / (double)baseFreq;
		return (U64)llround(crfPeriod * (double)timestampsPerPdu);
	}

	if (pPvtData && pPvtData->crfPeriodNs > 0 && pPvtData->timestampsPerPdu > 0) {
		return pPvtData->crfPeriodNs * pPvtData->timestampsPerPdu;
	}

	return 0;
}

static void crfDiagCheckAndLog(
	pvt_data_t *pPvtData,
	const char *direction,
	U8 seq,
	U64 firstCrfNs,
	U64 expectedDeltaNs,
	bool *pPrevValid,
	U64 *pPrevFirstCrfNs)
{
	if (!pPvtData || !pPvtData->diagEnable || !pPrevValid || !pPrevFirstCrfNs) {
		return;
	}

	if (*pPrevValid) {
		U64 prevFirstNs = *pPrevFirstCrfNs;
		bool nonMonotonic = (firstCrfNs <= prevFirstNs);
		U64 deltaNs = nonMonotonic ? 0 : (firstCrfNs - prevFirstNs);
		U64 absErrNs = 0;
		U64 jitterThresholdNs = pPvtData->diagJitterThreshNs;
		bool jitter = FALSE;
		bool jump = FALSE;

		if (jitterThresholdNs == 0) {
			jitterThresholdNs = CRF_DIAG_DEFAULT_JITTER_THRESH_NS;
		}

		if (expectedDeltaNs > 0 && deltaNs > 0) {
			absErrNs = (deltaNs > expectedDeltaNs) ? (deltaNs - expectedDeltaNs) : (expectedDeltaNs - deltaNs);
			jump = (deltaNs > (expectedDeltaNs * CRF_DIAG_JUMP_FACTOR));
			jitter = (absErrNs > jitterThresholdNs);
		}

		if (nonMonotonic) {
			pPvtData->diagTsNonMonotonicCount++;
			AVB_LOGF_WARNING("CRF DIAG %s non-monotonic timestamp: seq=%u prev=%llu curr=%llu",
				direction,
				(unsigned)seq,
				(unsigned long long)prevFirstNs,
				(unsigned long long)firstCrfNs);
		}
		else if (jump) {
			pPvtData->diagTsJumpCount++;
			AVB_LOGF_WARNING("CRF DIAG %s timestamp jump/rebase: seq=%u delta=%lluns expected=%lluns err=%lluns",
				direction,
				(unsigned)seq,
				(unsigned long long)deltaNs,
				(unsigned long long)expectedDeltaNs,
				(unsigned long long)absErrNs);
		}
		else if (jitter) {
			pPvtData->diagTsJitterCount++;
			AVB_LOGF_INFO("CRF DIAG %s timestamp jitter: seq=%u delta=%lluns expected=%lluns err=%lluns thresh=%lluns",
				direction,
				(unsigned)seq,
				(unsigned long long)deltaNs,
				(unsigned long long)expectedDeltaNs,
				(unsigned long long)absErrNs,
				(unsigned long long)jitterThresholdNs);
		}
	}

	*pPrevFirstCrfNs = firstCrfNs;
	*pPrevValid = TRUE;
}

static void crfDiagSummaryMaybeLog(pvt_data_t *pPvtData, const char *direction)
{
	if (!pPvtData || !pPvtData->diagEnable || pPvtData->diagLogEveryPackets == 0) {
		return;
	}

	U64 packets = (strcmp(direction, "TX") == 0) ? pPvtData->diagPacketsTx : pPvtData->diagPacketsRx;
	if ((packets % pPvtData->diagLogEveryPackets) != 0) {
		return;
	}

	AVB_LOGF_INFO("CRF DIAG %s summary: packets=%llu seq_gaps=%llu jitter=%llu jumps=%llu non_monotonic=%llu",
		direction,
		(unsigned long long)packets,
		(unsigned long long)pPvtData->diagSeqGapCount,
		(unsigned long long)pPvtData->diagTsJitterCount,
		(unsigned long long)pPvtData->diagTsJumpCount,
		(unsigned long long)pPvtData->diagTsNonMonotonicCount);
}

static void crfDiagTxLeadMaybeLog(
	pvt_data_t *pPvtData,
	U64 firstCrfNs,
	U64 nowNs,
	U64 launchTimeNs)
{
	if (!pPvtData || !pPvtData->diagEnable || pPvtData->diagLogEveryPackets == 0) {
		return;
	}

	S64 targetLeadNs = (pPvtData->roundedMttNs >= pPvtData->launchLeadNs)
		? (S64)(pPvtData->roundedMttNs - pPvtData->launchLeadNs)
		: 0;
	S64 actualLeadNs = (firstCrfNs >= nowNs)
		? (S64)(firstCrfNs - nowNs)
		: -((S64)(nowNs - firstCrfNs));
	S64 leadErrNs = actualLeadNs - targetLeadNs;
	S64 launchSkewNs = (launchTimeNs >= nowNs)
		? (S64)(launchTimeNs - nowNs)
		: -((S64)(nowNs - launchTimeNs));
	S64 absLeadErrNs = llabs(leadErrNs);
	S64 absLaunchSkewNs = llabs(launchSkewNs);

	if (!pPvtData->diagTxLeadWindowValid) {
		pPvtData->diagTxLeadWindowValid = TRUE;
		pPvtData->diagTxLeadErrMinNs = leadErrNs;
		pPvtData->diagTxLeadErrMaxNs = leadErrNs;
		pPvtData->diagTxLeadErrSumNs = 0;
		pPvtData->diagTxLeadErrAbsMaxNs = 0;
		pPvtData->diagTxLaunchSkewAbsMaxNs = 0;
		pPvtData->diagTxLeadWindowPackets = 0;
	}

	if (leadErrNs < pPvtData->diagTxLeadErrMinNs) {
		pPvtData->diagTxLeadErrMinNs = leadErrNs;
	}
	if (leadErrNs > pPvtData->diagTxLeadErrMaxNs) {
		pPvtData->diagTxLeadErrMaxNs = leadErrNs;
	}
	pPvtData->diagTxLeadErrSumNs += leadErrNs;
	if (absLeadErrNs > pPvtData->diagTxLeadErrAbsMaxNs) {
		pPvtData->diagTxLeadErrAbsMaxNs = absLeadErrNs;
	}
	if (absLaunchSkewNs > pPvtData->diagTxLaunchSkewAbsMaxNs) {
		pPvtData->diagTxLaunchSkewAbsMaxNs = absLaunchSkewNs;
	}
	pPvtData->diagTxLeadWindowPackets++;

	if ((pPvtData->diagPacketsTx % pPvtData->diagLogEveryPackets) != 0 ||
			pPvtData->diagTxLeadWindowPackets == 0) {
		return;
	}

	S64 avgLeadErrNs = pPvtData->diagTxLeadErrSumNs / (S64)pPvtData->diagTxLeadWindowPackets;
	S64 avgActualLeadNs = targetLeadNs + avgLeadErrNs;
	pPvtData->diagTxLeadLogCount++;
	AVB_LOGF_INFO(
		"CRF TX lead error: avg_err=%lldns avg_actual=%lldns min_err=%lldns max_err=%lldns abs_max=%lldns target_lead=%lldns launch_skew_abs_max=%lldns packets=%llu logs=%llu",
		(long long)avgLeadErrNs,
		(long long)avgActualLeadNs,
		(long long)pPvtData->diagTxLeadErrMinNs,
		(long long)pPvtData->diagTxLeadErrMaxNs,
		(long long)pPvtData->diagTxLeadErrAbsMaxNs,
		(long long)targetLeadNs,
		(long long)pPvtData->diagTxLaunchSkewAbsMaxNs,
		(unsigned long long)pPvtData->diagTxLeadWindowPackets,
		(unsigned long long)pPvtData->diagTxLeadLogCount);

	pPvtData->diagTxLeadWindowValid = FALSE;
	pPvtData->diagTxLeadErrMinNs = 0;
	pPvtData->diagTxLeadErrMaxNs = 0;
	pPvtData->diagTxLeadErrSumNs = 0;
	pPvtData->diagTxLeadErrAbsMaxNs = 0;
	pPvtData->diagTxLaunchSkewAbsMaxNs = 0;
	pPvtData->diagTxLeadWindowPackets = 0;
}

// This talker callback will be called for each AVB observation interval.
tx_cb_ret_t openavbMapCrfTxCB(media_q_t *pMediaQ, U8 *pData, U32 *dataLen)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);

	if (!pMediaQ || !pData || !dataLen) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
	if (!pPvtData) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	struct avtp_crf_pdu *pdu = (struct avtp_crf_pdu *)pData;
	*dataLen = pPvtData->maxDataSize;

	bool tu = FALSE;
	struct timespec now = {0};
	if (!crfGetTime(&now, &tu)) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}
	U64 nowNs = crfTimespecToNs(&now);
	U32 recoveryGeneration = osalClockGetWalltimeRecoveryGeneration();
	U64 pduPeriodNs = pPvtData->crfPeriodNs * pPvtData->timestampsPerPdu;
	if (pduPeriodNs == 0) {
		pduPeriodNs = 1;
	}
	U8 txSeq = pPvtData->seqNum;

	if (recoveryGeneration != pPvtData->recoveryGeneration) {
		if (pPvtData->recoveryGeneration != 0 && pPvtData->nextCrfTimeValid) {
			AVB_LOGF_WARNING(
				"CRF TX clock recovery epoch change: recovery_generation=%u previous=%u",
				recoveryGeneration,
				pPvtData->recoveryGeneration);
		}
		pPvtData->recoveryGeneration = recoveryGeneration;
		pPvtData->nextCrfTimeValid = FALSE;
		pPvtData->lastLaunchValid = FALSE;
		pPvtData->lastTxFirstCrfValid = FALSE;
		pPvtData->diagTxLeadWindowValid = FALSE;
		pPvtData->diagTxLeadErrMinNs = 0;
		pPvtData->diagTxLeadErrMaxNs = 0;
		pPvtData->diagTxLeadErrSumNs = 0;
		pPvtData->diagTxLeadErrAbsMaxNs = 0;
		pPvtData->diagTxLaunchSkewAbsMaxNs = 0;
		pPvtData->diagTxLeadWindowPackets = 0;
	}

	// Maintain a free-running CRF timeline. If we're more than one PDU period
	// late versus the scheduled launch, skip ahead by whole periods and continue.
	U64 baseCrfTime = nowNs + pPvtData->roundedMttNs;
	if (pPvtData->nextCrfTimeValid) {
		baseCrfTime = pPvtData->nextCrfTimeNs;
	}
	else {
		pPvtData->nextCrfTimeValid = TRUE;
	}

	U64 launchTimeNs = baseCrfTime - pPvtData->roundedMttNs + pPvtData->launchLeadNs;
	if (launchTimeNs + pduPeriodNs < nowNs) {
		U64 lateNs = nowNs - launchTimeNs;
		// Ceil divide so rebased launch time is guaranteed to be in the future.
		U64 skipPeriods = (lateNs / pduPeriodNs) + 1;
		pPvtData->diagTxCatchupCount++;
		pPvtData->diagTxCatchupPeriods += skipPeriods;
		if (pPvtData->diagTxCatchupCount <= 16 ||
				(pPvtData->diagTxCatchupCount % 1000) == 0) {
			AVB_LOGF_WARNING(
				"CRF TX catch-up: late=%lluns skip_pdus=%llu pdu_period=%lluns next_launch_in=%lluns catches=%llu skipped_total=%llu",
				(unsigned long long)lateNs,
				(unsigned long long)skipPeriods,
				(unsigned long long)pduPeriodNs,
				(unsigned long long)((launchTimeNs + (skipPeriods * pduPeriodNs) >= nowNs) ?
					((launchTimeNs + (skipPeriods * pduPeriodNs)) - nowNs) : 0ULL),
				(unsigned long long)pPvtData->diagTxCatchupCount,
				(unsigned long long)pPvtData->diagTxCatchupPeriods);
		}
		baseCrfTime += skipPeriods * pduPeriodNs;
		launchTimeNs = baseCrfTime - pPvtData->roundedMttNs + pPvtData->launchLeadNs;
	}

	pPvtData->nextCrfTimeNs = baseCrfTime + pduPeriodNs;
	pPvtData->lastLaunchTimeNs = launchTimeNs;
	pPvtData->lastLaunchValid = TRUE;

	if (pPvtData->diagTxLeadLogCount < 16 || launchTimeNs < nowNs) {
		S64 launchDeltaNs = (launchTimeNs >= nowNs)
			? (S64)(launchTimeNs - nowNs)
			: -((S64)(nowNs - launchTimeNs));
		S64 crfLeadNs = (baseCrfTime >= nowNs)
			? (S64)(baseCrfTime - nowNs)
			: -((S64)(nowNs - baseCrfTime));
		AVB_LOGF_INFO(
			"CRF TX calc: now=%" PRIu64 " base=%" PRIu64 " crf_lead=%" PRId64 "ns launch=%" PRIu64 " launch_delta=%" PRId64 "ns rounded_mtt=%" PRIu64 "ns launch_lead=%" PRIu64 "ns next_valid=%d seq=%u",
			nowNs,
			baseCrfTime,
			crfLeadNs,
			launchTimeNs,
			launchDeltaNs,
			pPvtData->roundedMttNs,
			pPvtData->launchLeadNs,
			pPvtData->nextCrfTimeValid ? 1 : 0,
			(unsigned)txSeq);
	}

	U32 subtypeData = 0;
	subtypeData |= ((U32)(AVTP_CRF_SUBTYPE & 0x7F) << 24);
	subtypeData |= (1U << SHIFT_SV);
	if (tu)
		subtypeData |= (1U << SHIFT_TU);
	subtypeData |= ((U32)pPvtData->seqNum << SHIFT_SEQ_NUM);
	subtypeData |= (U32)pPvtData->type;

	pdu->subtype_data = htonl(subtypeData);

	U64 packetInfo = 0;
	packetInfo |= ((U64)(pPvtData->pull & 0x07) << SHIFT_PULL);
	packetInfo |= ((U64)(pPvtData->baseFreq & MASK_BASE_FREQ) << SHIFT_BASE_FREQ);
	packetInfo |= ((U64)(pPvtData->dataLen & MASK_DATA_LEN) << SHIFT_CRF_DATA_LEN);
	packetInfo |= ((U64)(pPvtData->timestampInterval & MASK_TIMESTAMP_INTERVAL));
	pdu->packet_info = htobe64(packetInfo);

	for (U16 idx = 0; idx < pPvtData->timestampsPerPdu; idx++) {
		pdu->crf_data[idx] = htobe64(baseCrfTime + (pPvtData->crfPeriodNs * idx));
	}
	openavbClockSourceRuntimeSetMediaClockForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
		crfRuntimeResolveLocationIndex(
			pPvtData,
			OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT),
		(baseCrfTime >= pPvtData->roundedMttNs)
			? (baseCrfTime - pPvtData->roundedMttNs)
			: nowNs,
		tu);
	openavbClockSourceRuntimeSetCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
		crfRuntimeResolveLocationIndex(
			pPvtData,
			OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT),
		baseCrfTime,
		tu);

	pPvtData->diagPacketsTx++;
	crfDiagTxLeadMaybeLog(
		pPvtData,
		baseCrfTime,
		nowNs,
		launchTimeNs);
	crfDiagCheckAndLog(
		pPvtData,
		"TX",
		txSeq,
		baseCrfTime,
		pduPeriodNs,
		&pPvtData->lastTxFirstCrfValid,
		&pPvtData->lastTxFirstCrfNs);
	crfDiagSummaryMaybeLog(pPvtData, "TX");

	pPvtData->seqNum++;

	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return TX_CB_RET_PACKET_READY;
}

bool openavbMapCrfLTCalcCB(media_q_t *pMediaQ, U64 *lt)
{
	if (!pMediaQ || !lt) {
		return FALSE;
	}

	pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
	if (!pPvtData || !pPvtData->lastLaunchValid) {
		return FALSE;
	}

	*lt = pPvtData->lastLaunchTimeNs;
	return TRUE;
}

void openavbMapCrfRxInitCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ && pMediaQ->pPvtMapInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		U16 runtimeLocation = crfRuntimeResolveLocationIndex(
			pPvtData,
			OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT);
		pPvtData->isTalker = FALSE;
		pPvtData->rxExpectedSeqValid = FALSE;
		pPvtData->lastRxFirstCrfValid = FALSE;
		pPvtData->diagPacketsRx = 0;
		openavbClockSourceRuntimeClearCrfTimeForLocation(
			OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
			runtimeLocation);
		openavbClockSourceRuntimeClearMediaClockForLocation(
			OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
			runtimeLocation);
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

bool openavbMapCrfRxCB(media_q_t *pMediaQ, U8 *pData, U32 dataLen)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);
	if (!pMediaQ || !pData || dataLen < sizeof(struct avtp_crf_pdu)) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
	if (!pPvtData) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	struct avtp_crf_pdu *pdu = (struct avtp_crf_pdu *)pData;
	U32 subtypeData = ntohl(pdu->subtype_data);
	U8 subtype = (U8)((subtypeData >> 24) & 0x7F);
	if (subtype != AVTP_CRF_SUBTYPE) {
		IF_LOG_INTERVAL(1000) AVB_LOGF_ERROR("Unexpected CRF subtype: 0x%02x", subtype);
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	U8 rxSeq = (U8)((subtypeData >> SHIFT_SEQ_NUM) & 0xFF);
	if (pPvtData->rxExpectedSeqValid && rxSeq != pPvtData->rxExpectedSeqNum) {
		U8 lost = (U8)(rxSeq - pPvtData->rxExpectedSeqNum);
		pPvtData->diagSeqGapCount++;
		AVB_LOGF_INFO("CRF sequence mismatch: expected=%u got=%u lost=%u",
			      pPvtData->rxExpectedSeqNum, rxSeq, lost);
	}
	pPvtData->rxExpectedSeqNum = (U8)(rxSeq + 1);
	pPvtData->rxExpectedSeqValid = TRUE;

	U64 packetInfo = be64toh(pdu->packet_info);
	U16 rxDataLen = (U16)((packetInfo >> SHIFT_CRF_DATA_LEN) & MASK_DATA_LEN);
	U32 payloadAvail = dataLen - (U32)offsetof(struct avtp_crf_pdu, crf_data);
	if (rxDataLen == 0 || rxDataLen > payloadAvail || (rxDataLen % sizeof(U64)) != 0) {
		IF_LOG_INTERVAL(1000) AVB_LOGF_ERROR(
			"Invalid CRF payload length: declared=%u available=%u", rxDataLen, payloadAvail);
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	U64 firstCrfNs = be64toh(pdu->crf_data[0]);
	U16 rxTimestampsPerPdu = (U16)(rxDataLen / sizeof(U64));
	U32 rxBaseFreq = (U32)((packetInfo >> SHIFT_BASE_FREQ) & MASK_BASE_FREQ);
	U16 rxTimestampInterval = (U16)(packetInfo & MASK_TIMESTAMP_INTERVAL);
	U64 expectedDeltaNs = crfExpectedPduDeltaNs(rxBaseFreq, rxTimestampInterval, rxTimestampsPerPdu, pPvtData);
	bool uncertain = ((subtypeData >> SHIFT_TU) & 0x1) ? TRUE : FALSE;
	openavbClockSourceRuntimeSetCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
		crfRuntimeResolveLocationIndex(
			pPvtData,
			OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT),
		firstCrfNs,
		uncertain);

	if (pPvtData->rxBypassQueue) {
		pPvtData->diagPacketsRx++;
		crfDiagCheckAndLog(
			pPvtData,
			"RX",
			rxSeq,
			firstCrfNs,
			expectedDeltaNs,
			&pPvtData->lastRxFirstCrfValid,
			&pPvtData->lastRxFirstCrfNs);
		crfDiagSummaryMaybeLog(pPvtData, "RX");
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TRUE;
	}

	media_q_item_t *pMediaQItem = openavbMediaQHeadLock(pMediaQ);
	if (!pMediaQItem) {
		IF_LOG_INTERVAL(1000) AVB_LOG_ERROR("CRF listener media queue full.");
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	U32 copyLen = rxDataLen;
	if (copyLen > pMediaQItem->itemSize) {
		IF_LOG_INTERVAL(1000) AVB_LOGF_ERROR("CRF payload too large for media queue item: %u > %u",
						     copyLen, pMediaQItem->itemSize);
		copyLen = pMediaQItem->itemSize;
	}
	memcpy(pMediaQItem->pPubData, pdu->crf_data, copyLen);
	pMediaQItem->dataLen = copyLen;

	// CRF carries absolute wall-clock timestamps. Use the first one for queue
	// timing so listener interfaces can optionally consume by presentation time.
	openavbAvtpTimeSetToTimestamp(pMediaQItem->pAvtpTime, (U32)firstCrfNs);
	openavbAvtpTimeSetTimestampValid(pMediaQItem->pAvtpTime, TRUE);
	openavbAvtpTimeSetTimestampUncertain(pMediaQItem->pAvtpTime, uncertain);

	pPvtData->diagPacketsRx++;
	crfDiagCheckAndLog(
		pPvtData,
		"RX",
		rxSeq,
		firstCrfNs,
		expectedDeltaNs,
		&pPvtData->lastRxFirstCrfValid,
		&pPvtData->lastRxFirstCrfNs);
	crfDiagSummaryMaybeLog(pPvtData, "RX");

	openavbMediaQHeadPush(pMediaQ);
	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return TRUE;
}

void openavbMapCrfEndCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ && pMediaQ->pPvtMapInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		U16 runtimeLocation = crfRuntimeResolveLocationIndex(
			pPvtData,
			pPvtData->isTalker
				? OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT
				: OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT);
		openavbClockSourceRuntimeClearCrfTimeForLocation(
			pPvtData->isTalker
				? OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT
				: OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
			runtimeLocation);
		openavbClockSourceRuntimeClearMediaClockForLocation(
			pPvtData->isTalker
				? OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT
				: OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
			runtimeLocation);
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

void openavbMapCrfGenEndCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// Initialization entry point into the mapping module. Will need to be included in the .ini file.
extern DLL_EXPORT bool openavbMapCrfInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);

	if (pMediaQ) {
		pMediaQ->pMediaQDataFormat = strdup(MapCRFMediaQDataFormat);
		pMediaQ->pPvtMapInfo = calloc(1, sizeof(pvt_data_t));

		if (!pMediaQ->pMediaQDataFormat || !pMediaQ->pPvtMapInfo) {
			AVB_LOG_ERROR("Unable to allocate memory for mapping module.");
			return FALSE;
		}

		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;

		pMapCB->map_cfg_cb = openavbMapCrfCfgCB;
		pMapCB->map_subtype_cb = openavbMapCrfSubtypeCB;
		pMapCB->map_avtp_version_cb = openavbMapCrfAvtpVersionCB;
		pMapCB->map_max_data_size_cb = openavbMapCrfMaxDataSizeCB;
		pMapCB->map_transmit_interval_cb = openavbMapCrfTransmitIntervalCB;
		pMapCB->map_gen_init_cb = openavbMapCrfGenInitCB;
		pMapCB->map_tx_init_cb = openavbMapCrfTxInitCB;
		pMapCB->map_tx_cb = openavbMapCrfTxCB;
#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
		pMapCB->map_lt_calc_cb = openavbMapCrfLTCalcCB;
#endif
		pMapCB->map_rx_init_cb = openavbMapCrfRxInitCB;
		pMapCB->map_rx_cb = openavbMapCrfRxCB;
		pMapCB->map_end_cb = openavbMapCrfEndCB;
		pMapCB->map_gen_end_cb = openavbMapCrfGenEndCB;

		pPvtData->baseFreq = 48000;
		pPvtData->timestampInterval = 96;
		pPvtData->timestampsPerPdu = 1;
		pPvtData->pull = AVTP_CRF_PULL_MULT_BY_1;
		pPvtData->type = AVTP_CRF_TYPE_AUDIO_SAMPLE;
		pPvtData->txInterval = 0;
		pPvtData->txIntervalOverride = FALSE;
		pPvtData->launchLeadUsec = 0;
		pPvtData->maxTransitUsec = inMaxTransitUsec;
		pPvtData->diagEnable = TRUE;
		pPvtData->diagLogEveryPackets = 0;
		pPvtData->diagJitterThreshNs = CRF_DIAG_DEFAULT_JITTER_THRESH_NS;
		pPvtData->runtimeLocationIndex = OPENAVB_AEM_DESCRIPTOR_INVALID;
		pPvtData->runtimeLocationConfigured = FALSE;
		pPvtData->rxBypassQueue = FALSE;

		crfUpdateDerived(pPvtData);
		openavbMediaQSetMaxLatency(pMediaQ, inMaxTransitUsec);
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return TRUE;
}
