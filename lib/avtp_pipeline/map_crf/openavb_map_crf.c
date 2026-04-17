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
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
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
#include "openavb_avb32_direct_abi.h"

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
#define CRF_DIRECT_DEFAULT_TARGET_LATENCY_FRAMES 384u
#define CRF_DIRECT_DEFAULT_CATCHUP_FRAMES 1920u

static bool crfHasAuthoritativeMediaTime(const openavb_avb32_direct_shm_t *pShm)
{
	if (!pShm) {
		return FALSE;
	}

	return pShm->version >= 2u
		&& (pShm->media_time_flags & OPENAVB_AVB32_DIRECT_MEDIA_TIME_VALID)
		&& (pShm->media_time_flags & OPENAVB_AVB32_DIRECT_MEDIA_TIME_TAI)
		&& pShm->sample_rate != 0u;
}

static U64 crfFrame0FromMediaTime(const openavb_avb32_direct_shm_t *pShm,
				  U32 sampleRate)
{
	U64 anchorToFrame0Ns;

	if (!pShm || sampleRate == 0u) {
		return 0;
	}

	anchorToFrame0Ns =
		((U64)pShm->media_time_anchor_frames * NSEC_PER_SEC) / sampleRate;
	return (pShm->media_time_anchor_tai_ns > anchorToFrame0Ns)
		? (pShm->media_time_anchor_tai_ns - anchorToFrame0Ns)
		: 0;
}

static U64 crfFramesToNs(U64 frames, U32 sampleRate);
static S64 crfDeltaNs(U64 newer, U64 older);

static void crfLogMediaTimeIdentity(const openavb_avb32_direct_shm_t *pShm,
				    U32 sampleRate,
				    U64 adjustedFrame0Ns,
				    U64 presentedFrames,
				    U64 nextCrfFrameIndex,
				    U64 baseCrfTimeNs,
				    U64 nowPtpNs)
{
	U64 anchorStepNs;
	U64 anchorReconstructedNs;
	U64 rawPresentedNs;
	U64 mediaPresentedNs;
	S64 anchorContractErrNs;
	S64 rawFrame0DeltaNs;
	S64 presentedDeltaNs;
	S64 baseLeadNs;

	if (!pShm || sampleRate == 0u || !crfHasAuthoritativeMediaTime(pShm)) {
		return;
	}

	anchorStepNs =
		((U64)pShm->media_time_anchor_frames * NSEC_PER_SEC) / sampleRate;
	anchorReconstructedNs = adjustedFrame0Ns + anchorStepNs;
	anchorContractErrNs =
		crfDeltaNs(anchorReconstructedNs, pShm->media_time_anchor_tai_ns);
	rawFrame0DeltaNs = crfDeltaNs(pShm->frame0_walltime_ns, adjustedFrame0Ns);
	rawPresentedNs =
		pShm->frame0_walltime_ns + crfFramesToNs(presentedFrames, sampleRate);
	mediaPresentedNs =
		adjustedFrame0Ns + crfFramesToNs(presentedFrames, sampleRate);
	presentedDeltaNs = crfDeltaNs(rawPresentedNs, mediaPresentedNs);
	baseLeadNs = crfDeltaNs(baseCrfTimeNs, nowPtpNs);

	AVB_LOGF_WARNING(
		"CRF media-time identity: epoch=%" PRIu64 " gen=%" PRIu64 " flags=0x%" PRIx64 " anchor_frames=%" PRIu64 " anchor_tai=%" PRIu64 " frame0_raw=%" PRIu64 " frame0_media=%" PRIu64 " frame0_delta=%" PRId64 "ns anchor_contract_err=%" PRId64 "ns presented=%" PRIu64 " presented_delta=%" PRId64 "ns next=%" PRIu64 " base=%" PRIu64 " base_lead=%" PRId64 "ns now=%" PRIu64,
		pShm->media_time_epoch,
		pShm->writer_generation,
		pShm->media_time_flags,
		pShm->media_time_anchor_frames,
		pShm->media_time_anchor_tai_ns,
		pShm->frame0_walltime_ns,
		adjustedFrame0Ns,
		rawFrame0DeltaNs,
		anchorContractErrNs,
		presentedFrames,
		presentedDeltaNs,
		nextCrfFrameIndex,
		baseCrfTimeNs,
		baseLeadNs,
		nowPtpNs);
}

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
	char *directDevicePath;
	U32 directAudioRate;
	U32 directTargetLatencyFrames;
	U32 directCatchupFrames;

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
	U64 nextCrfFrameIndex;
	bool nextCrfFrameValid;
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
	U64 diagTxCalcOutlierCount;
	U64 diagTxMarginLogCount;
	U64 lastTxFirstCrfNs;
	bool lastTxFirstCrfValid;
	U64 lastTxCalcBaseNs;
	U64 lastTxCalcLaunchNs;
	U8 lastTxCalcSeq;
	bool lastTxCalcValid;
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
	bool diagRuntimePublishValid;
	U64 diagRuntimePublishWallNs;
	U64 diagRuntimePublishMediaNs;
	U64 diagRuntimePublishCrfNs;
	U64 diagRuntimePublishOutlierCount;
	U16 runtimeLocationIndex;
	bool runtimeLocationConfigured;
	U32 recoveryGeneration;
	int directFd;
	size_t directMapBytes;
	openavb_avb32_direct_shm_t *pDirectShm;
	U64 directFramesPerTimestamp;
	U64 directFramesPerPdu;
	U64 directResyncCount;
	U64 lastDirectWriterGeneration;
	U64 lastDirectFrame0WalltimeNs;
	U64 lastDirectMediaTimeFlags;
	U64 lastDirectMediaTimeEpoch;
	U64 lastDirectMediaTimeAnchorFrames;
	U64 lastDirectMediaTimeAnchorTaiNs;
	U64 directMediaIdentityLogCount;
	U64 directClockDiagCount;
} pvt_data_t;

struct avtp_crf_pdu {
	U32 subtype_data;
	U64 stream_id;
	U64 packet_info;
	U64 crf_data[0];
} __attribute__ ((__packed__));

static size_t crfPageAlign(size_t bytes)
{
	long pageSize = sysconf(_SC_PAGESIZE);
	size_t pageSizeU = (pageSize > 0) ? (size_t)pageSize : 4096u;

	return ((bytes + pageSizeU - 1u) / pageSizeU) * pageSizeU;
}

static U64 crfFramesToNs(U64 frames, U32 sampleRate)
{
	if (sampleRate == 0) {
		return 0;
	}
	return (frames * NSEC_PER_SEC) / sampleRate;
}

static U64 crfAlignUp(U64 value, U64 step)
{
	if (step == 0) {
		return value;
	}
	return ((value + step - 1ULL) / step) * step;
}

static S64 crfDeltaNs(U64 newer, U64 older)
{
	return (newer >= older)
		? (S64)(newer - older)
		: -((S64)(older - newer));
}

static U64 crfAddSignedNs(U64 base, S64 delta)
{
	if (delta >= 0) {
		return base + (U64)delta;
	}
	else {
		U64 absDelta = (U64)(-delta);
		return (base > absDelta) ? (base - absDelta) : 0;
	}
}

static bool crfDirectGetAdjustedFrame0Ns(pvt_data_t *pPvtData,
					 U64 *pAdjustedFrame0Ns,
					 U64 *pProjectedWriterNs,
					 U64 *pNowPtpNs,
					 S64 *pWriterToPtpNs);

static void crfLogSourceOutlierRow(pvt_data_t *pPvtData,
				      const char *event,
				      U64 presentedFrames,
				      U64 committedFrames,
				      U64 desiredFrame,
				      U64 nextFrameIndex,
				      U64 baseFrameIndex,
				      U64 baseCrfTimeNs)
{
	openavb_avb32_direct_shm_t *pShm;
	U64 adjustedFrame0Ns = 0;
	U64 projectedWriterNs = 0;
	S64 writerToPtpNs = 0;

	if (!pPvtData || !pPvtData->pDirectShm) {
		return;
	}

	pShm = pPvtData->pDirectShm;
	if (!crfDirectGetAdjustedFrame0Ns(pPvtData,
					  &adjustedFrame0Ns,
					  &projectedWriterNs,
					  NULL,
					  &writerToPtpNs)) {
		adjustedFrame0Ns = pShm->frame0_walltime_ns;
		projectedWriterNs = pShm->writer_walltime_ns;
		writerToPtpNs = 0;
	}

	AVB_LOGF_WARNING(
		"TX OUTLIER ROW flags=X.. stage=crf_source event=%s loc=%u seq=%u presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " next=%" PRIu64 " base_frame=%" PRIu64 " base_time=%" PRIu64 " frame0_raw=%" PRIu64 " frame0_adj=%" PRIu64 " writer_proj=%" PRIu64 " writer_gen=%" PRIu64 " writer_to_ptp_ns=%" PRId64 " target=%u catchup=%u resyncs=%" PRIu64,
		event ? event : "unknown",
		pPvtData->runtimeLocationIndex,
		(unsigned)pPvtData->seqNum,
		presentedFrames,
		committedFrames,
		desiredFrame,
		nextFrameIndex,
		baseFrameIndex,
		baseCrfTimeNs,
		pShm->frame0_walltime_ns,
		adjustedFrame0Ns,
		projectedWriterNs,
		pShm->writer_generation,
		writerToPtpNs,
		pPvtData->directTargetLatencyFrames,
		pPvtData->directCatchupFrames,
		pPvtData->directResyncCount);
}

static void crfLogTimestampOutlierRow(pvt_data_t *pPvtData,
					 const char *direction,
					 const char *event,
					 U8 seq,
					 U64 firstCrfNs,
					 U64 prevFirstCrfNs,
					 U64 deltaNs,
					 U64 expectedDeltaNs,
					 U64 errNs,
					 U64 thresholdNs)
{
	if (!pPvtData) {
		return;
	}

	AVB_LOGF_INFO(
		"TX OUTLIER ROW flags=.Y. stage=crf_mapper dir=%s event=%s loc=%u seq=%u first=%" PRIu64 " prev=%" PRIu64 " delta_ns=%" PRIu64 " expected_ns=%" PRIu64 " err_ns=%" PRIu64 " thresh_ns=%" PRIu64 " next_frame=%" PRIu64 " next_time=%" PRIu64 " recovery_gen=%u jumps=%" PRIu64 " jitters=%" PRIu64 " nonmono=%" PRIu64,
		direction ? direction : "?",
		event ? event : "unknown",
		pPvtData->runtimeLocationIndex,
		(unsigned)seq,
		firstCrfNs,
		prevFirstCrfNs,
		deltaNs,
		expectedDeltaNs,
		errNs,
		thresholdNs,
		pPvtData->nextCrfFrameIndex,
		pPvtData->nextCrfTimeNs,
		pPvtData->recoveryGeneration,
		pPvtData->diagTsJumpCount,
		pPvtData->diagTsJitterCount,
		pPvtData->diagTsNonMonotonicCount);
}

static void crfLogCatchupOutlierRow(pvt_data_t *pPvtData,
				       bool useDirectExport,
				       U64 nowNs,
				       U64 baseCrfTimeNs,
				       U64 launchTimeNs,
				       U64 lateNs,
				       U64 skipPeriods,
				       U64 pduPeriodNs,
				       U64 baseFrameIndex)
{
	if (!pPvtData) {
		return;
	}

	AVB_LOGF_WARNING(
		"TX OUTLIER ROW flags=.Y. stage=crf_mapper event=catchup source=%s loc=%u seq=%u now=%" PRIu64 " base=%" PRIu64 " launch=%" PRIu64 " late_ns=%" PRIu64 " skip_pdus=%" PRIu64 " pdu_period_ns=%" PRIu64 " frame=%" PRIu64 " catches=%" PRIu64 " skipped_total=%" PRIu64,
		useDirectExport ? "direct_export" : "walltime",
		pPvtData->runtimeLocationIndex,
		(unsigned)pPvtData->seqNum,
		nowNs,
		baseCrfTimeNs,
		launchTimeNs,
		lateNs,
		skipPeriods,
		pduPeriodNs,
		baseFrameIndex,
		pPvtData->diagTxCatchupCount,
		pPvtData->diagTxCatchupPeriods);
}

static void crfLogBuildOutlierRow(pvt_data_t *pPvtData,
				     bool useDirectExport,
				     U8 seq,
				     U64 nowNs,
				     U64 baseFrameIndex,
				     U64 baseCrfTimeNs,
				     U64 launchTimeNs,
				     U64 timeNs,
				     U64 baseNs,
				     U64 fillNs,
				     U64 runtimeNs,
				     U64 totalNs,
				     U64 warnNs)
{
	if (!pPvtData) {
		return;
	}

	AVB_LOGF_WARNING(
		"TX OUTLIER ROW flags=.Y. stage=crf_build source=%s loc=%u seq=%u now=%" PRIu64 " frame=%" PRIu64 " base=%" PRIu64 " launch=%" PRIu64 " time_ns=%" PRIu64 " base_ns=%" PRIu64 " fill_ns=%" PRIu64 " runtime_ns=%" PRIu64 " total_ns=%" PRIu64 " warn_ns=%" PRIu64 " slow_time=%u slow_base=%u slow_fill=%u slow_runtime=%u slow_total=%u",
		useDirectExport ? "direct_export" : "walltime",
		pPvtData->runtimeLocationIndex,
		(unsigned)seq,
		nowNs,
		baseFrameIndex,
		baseCrfTimeNs,
		launchTimeNs,
		timeNs,
		baseNs,
		fillNs,
		runtimeNs,
		totalNs,
		warnNs,
		(timeNs >= warnNs) ? 1u : 0u,
		(baseNs >= warnNs) ? 1u : 0u,
		(fillNs >= warnNs) ? 1u : 0u,
		(runtimeNs >= warnNs) ? 1u : 0u,
		(totalNs >= warnNs) ? 1u : 0u);
}

static void crfLogTxCalcOutlierRow(pvt_data_t *pPvtData,
				      bool useDirectExport,
				      const char *event,
				      U8 seq,
				      U8 prevSeq,
				      U64 nowNs,
				      U64 baseFrameIndex,
				      U64 baseCrfTimeNs,
				      U64 prevBaseCrfTimeNs,
				      U64 launchTimeNs,
				      U64 prevLaunchTimeNs,
				      S64 crfLeadNs,
				      S64 launchDeltaNs,
				      S64 baseStepNs,
				      S64 launchStepNs,
				      U64 expectedStepNs,
				      S64 baseErrNs,
				      S64 launchErrNs,
				      U64 pduPeriodNs,
				      U64 thresholdNs)
{
	if (!pPvtData) {
		return;
	}

	pPvtData->diagTxCalcOutlierCount++;
	if (pPvtData->diagTxCalcOutlierCount <= 16 ||
			((pPvtData->diagTxCalcOutlierCount % 256) == 0)) {
		AVB_LOGF_WARNING(
			"TX OUTLIER ROW flags=.Y. stage=crf_txcalc event=%s source=%s loc=%u seq=%u prev_seq=%u now=%" PRIu64 " frame=%" PRIu64 " base=%" PRIu64 " prev_base=%" PRIu64 " launch=%" PRIu64 " prev_launch=%" PRIu64 " crf_lead_ns=%" PRId64 " launch_delta_ns=%" PRId64 " base_step_ns=%" PRId64 " launch_step_ns=%" PRId64 " expected_step_ns=%" PRIu64 " base_err_ns=%" PRId64 " launch_err_ns=%" PRId64 " pdu_period_ns=%" PRIu64 " thresh_ns=%" PRIu64 " rounded_mtt_ns=%" PRIu64 " launch_lead_ns=%" PRIu64 " count=%" PRIu64,
			event ? event : "unknown",
			useDirectExport ? "direct_export" : "walltime",
			pPvtData->runtimeLocationIndex,
			(unsigned)seq,
			(unsigned)prevSeq,
			nowNs,
			baseFrameIndex,
			baseCrfTimeNs,
			prevBaseCrfTimeNs,
			launchTimeNs,
			prevLaunchTimeNs,
			crfLeadNs,
			launchDeltaNs,
			baseStepNs,
			launchStepNs,
			expectedStepNs,
			baseErrNs,
			launchErrNs,
			pduPeriodNs,
			thresholdNs,
			pPvtData->roundedMttNs,
			pPvtData->launchLeadNs,
			pPvtData->diagTxCalcOutlierCount);
	}
}

static bool crfDirectGetAdjustedFrame0Ns(pvt_data_t *pPvtData,
					 U64 *pAdjustedFrame0Ns,
					 U64 *pProjectedWriterNs,
					 U64 *pNowPtpNs,
					 S64 *pWriterToPtpNs)
{
	openavb_avb32_direct_shm_t *pShm;
	U64 nowPtpNs = 0;
	U64 nowMonoNs = 0;
	U64 projectedWriterNs = 0;
	S64 writerToPtpNs = 0;
	S64 phaseAdjustNs = 0;

	if (!pPvtData || !pPvtData->pDirectShm || !pAdjustedFrame0Ns) {
		return FALSE;
	}
	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowPtpNs)) {
		return FALSE;
	}

	pShm = pPvtData->pDirectShm;
	if (crfHasAuthoritativeMediaTime(pShm)) {
		projectedWriterNs = pShm->media_time_anchor_tai_ns;
		writerToPtpNs = crfDeltaNs(nowPtpNs, projectedWriterNs);
		*pAdjustedFrame0Ns = crfFrame0FromMediaTime(
			pShm, pPvtData->directAudioRate);

		if (pProjectedWriterNs) {
			*pProjectedWriterNs = projectedWriterNs;
		}
		if (pNowPtpNs) {
			*pNowPtpNs = nowPtpNs;
		}
		if (pWriterToPtpNs) {
			*pWriterToPtpNs = writerToPtpNs;
		}
		return TRUE;
	}

	projectedWriterNs = pShm->writer_walltime_ns ? pShm->writer_walltime_ns
						      : pShm->frame0_walltime_ns;
	if (pShm->writer_walltime_ns != 0 &&
	    pShm->writer_monotonic_ns != 0 &&
	    CLOCK_GETTIME64(OPENAVB_CLOCK_MONOTONIC, &nowMonoNs) &&
	    nowMonoNs >= pShm->writer_monotonic_ns) {
		projectedWriterNs += (nowMonoNs - pShm->writer_monotonic_ns);
	}

	phaseAdjustNs = (S64)pShm->reserved1;
	writerToPtpNs = crfDeltaNs(nowPtpNs, projectedWriterNs);
	*pAdjustedFrame0Ns = crfAddSignedNs(pShm->frame0_walltime_ns, phaseAdjustNs);
	/*
	 * Keep frame0 anchored to the exported presentation timeline.
	 *
	 * Rebasing frame0 against the live projected writer wallclock here makes
	 * the CRF base inherit writer update quantization. The export already
	 * publishes a phase-adjusted frame0 origin, and folding writer_to_ptp
	 * into that base can create timestamp step outliers even when the source
	 * playhead itself is monotonic.
	 */

	if (pProjectedWriterNs) {
		*pProjectedWriterNs = projectedWriterNs;
	}
	if (pNowPtpNs) {
		*pNowPtpNs = nowPtpNs;
	}
	if (pWriterToPtpNs) {
		*pWriterToPtpNs = writerToPtpNs;
	}
	return TRUE;
}

static void crfLogDirectClockDiag(pvt_data_t *pPvtData,
				  U64 nowPtpNs,
				  U64 presentedFrames,
				  U64 committedFrames,
				  U64 desiredFrame,
				  U64 baseFrameIndex,
				  U64 baseCrfTimeNs,
				  const char *reason)
{
	openavb_avb32_direct_shm_t *pShm;
	U64 adjustedFrame0Ns = 0;
	U64 projectedWriterNs = 0;
	U64 presentedTimeNs;
	U64 committedTimeNs;
	S64 presentedSkewNs;
	S64 baseLeadNs;
	S64 committedLeadNs;
	S64 writerToPtpNs;
	S64 phaseAdjustNs;

	if (!pPvtData || !pPvtData->pDirectShm) {
		return;
	}

	pShm = pPvtData->pDirectShm;
	if (!crfDirectGetAdjustedFrame0Ns(pPvtData,
					  &adjustedFrame0Ns,
					  &projectedWriterNs,
					  NULL,
					  &writerToPtpNs)) {
		adjustedFrame0Ns = pShm->frame0_walltime_ns;
		projectedWriterNs = pShm->writer_walltime_ns;
		writerToPtpNs = crfDeltaNs(nowPtpNs, projectedWriterNs);
	}
	presentedTimeNs = adjustedFrame0Ns +
		crfFramesToNs(presentedFrames, pPvtData->directAudioRate);
	committedTimeNs = adjustedFrame0Ns +
		crfFramesToNs(committedFrames, pPvtData->directAudioRate);
	presentedSkewNs = crfDeltaNs(presentedTimeNs, nowPtpNs);
	baseLeadNs = crfDeltaNs(baseCrfTimeNs, nowPtpNs);
	committedLeadNs = crfDeltaNs(committedTimeNs, nowPtpNs);
	phaseAdjustNs = (S64)pShm->reserved1;

	AVB_LOGF_INFO(
		"CRF direct clock diag: reason=%s now_ptp=%" PRIu64 " frame0_raw=%" PRIu64 " frame0_adj=%" PRIu64 " writer_proj=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " base_frame=%" PRIu64 " presented_skew=%" PRId64 "ns committed_lead=%" PRId64 "ns base_lead=%" PRId64 "ns writer_to_ptp=%" PRId64 "ns phase_adjust=%" PRId64 "ns target=%u",
		reason ? reason : "periodic",
		nowPtpNs,
		pShm->frame0_walltime_ns,
		adjustedFrame0Ns,
		projectedWriterNs,
		presentedFrames,
		committedFrames,
		desiredFrame,
		baseFrameIndex,
		presentedSkewNs,
		committedLeadNs,
		baseLeadNs,
		writerToPtpNs,
		phaseAdjustNs,
		pPvtData->directTargetLatencyFrames);
}

static void crfDirectClose(pvt_data_t *pPvtData)
{
	if (!pPvtData) {
		return;
	}

	if (pPvtData->pDirectShm && pPvtData->directMapBytes > 0) {
		munmap(pPvtData->pDirectShm, pPvtData->directMapBytes);
	}
	pPvtData->pDirectShm = NULL;
	pPvtData->directMapBytes = 0;

	if (pPvtData->directFd >= 0) {
		close(pPvtData->directFd);
	}
	pPvtData->directFd = -1;
	pPvtData->directFramesPerTimestamp = 0;
	pPvtData->directFramesPerPdu = 0;
	pPvtData->nextCrfFrameValid = FALSE;
	pPvtData->lastDirectWriterGeneration = 0;
	pPvtData->lastDirectFrame0WalltimeNs = 0;
	pPvtData->lastDirectMediaTimeFlags = 0;
	pPvtData->lastDirectMediaTimeEpoch = 0;
	pPvtData->lastDirectMediaTimeAnchorFrames = 0;
	pPvtData->lastDirectMediaTimeAnchorTaiNs = 0;
	pPvtData->directMediaIdentityLogCount = 0;
}

static bool crfDirectOpen(pvt_data_t *pPvtData)
{
	size_t mapBytes;
	int fd;
	openavb_avb32_direct_shm_t *pShm;
	U64 framesPerTimestampNum;

	if (!pPvtData || !pPvtData->directDevicePath || pPvtData->directDevicePath[0] == '\0') {
		return FALSE;
	}
	if (pPvtData->pDirectShm) {
		return TRUE;
	}

	fd = open(pPvtData->directDevicePath, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		AVB_LOGF_WARNING("CRF direct export open failed: path=%s errno=%d (%s)",
			pPvtData->directDevicePath, errno, strerror(errno));
		return FALSE;
	}

	mapBytes = crfPageAlign(openavbAvb32DirectShmBytes(OPENAVB_AVB32_DIRECT_KERNEL_RING_FRAMES));
	pShm = mmap(NULL, mapBytes, PROT_READ, MAP_SHARED, fd, 0);
	if (pShm == MAP_FAILED) {
		AVB_LOGF_WARNING("CRF direct export mmap failed: path=%s errno=%d (%s) bytes=%zu",
			pPvtData->directDevicePath, errno, strerror(errno), mapBytes);
		close(fd);
		return FALSE;
	}

	if (pShm->magic != OPENAVB_AVB32_DIRECT_ABI_MAGIC
		|| (pShm->version != 1u
			&& pShm->version != OPENAVB_AVB32_DIRECT_ABI_VERSION)
		|| pShm->channel_count != OPENAVB_AVB32_DIRECT_CHANNELS
		|| pShm->bytes_per_sample != OPENAVB_AVB32_DIRECT_BYTES_PER_SAMPLE) {
		AVB_LOGF_ERROR("CRF direct export validation failed: path=%s magic=0x%08x version=%u channels=%u bytes_per_sample=%u",
			pPvtData->directDevicePath,
			pShm->magic,
			pShm->version,
			pShm->channel_count,
			pShm->bytes_per_sample);
		munmap(pShm, mapBytes);
		close(fd);
		return FALSE;
	}

	if (pPvtData->directAudioRate == 0) {
		pPvtData->directAudioRate = pShm->sample_rate;
	}
	else if (pShm->sample_rate != pPvtData->directAudioRate) {
		AVB_LOGF_ERROR("CRF direct export sample-rate mismatch: path=%s shm_rate=%u cfg_rate=%u",
			pPvtData->directDevicePath,
			pShm->sample_rate,
			pPvtData->directAudioRate);
		munmap(pShm, mapBytes);
		close(fd);
		return FALSE;
	}

	framesPerTimestampNum = (U64)pPvtData->directAudioRate * (U64)pPvtData->timestampInterval;
	if (pPvtData->baseFreq == 0 || framesPerTimestampNum == 0) {
		munmap(pShm, mapBytes);
		close(fd);
		return FALSE;
	}
	pPvtData->directFramesPerTimestamp =
		(U64)llround((double)framesPerTimestampNum / (double)pPvtData->baseFreq);
	pPvtData->directFramesPerPdu =
		pPvtData->directFramesPerTimestamp * (U64)pPvtData->timestampsPerPdu;
	if (pPvtData->directFramesPerTimestamp == 0 || pPvtData->directFramesPerPdu == 0) {
		munmap(pShm, mapBytes);
		close(fd);
		return FALSE;
	}

	if ((framesPerTimestampNum % pPvtData->baseFreq) != 0) {
		AVB_LOGF_WARNING("CRF direct export uses rounded frame step: rate=%u interval=%u base=%u frames_per_ts=%" PRIu64,
			pPvtData->directAudioRate,
			pPvtData->timestampInterval,
			pPvtData->baseFreq,
			pPvtData->directFramesPerTimestamp);
	}

	pPvtData->directFd = fd;
	pPvtData->directMapBytes = mapBytes;
	pPvtData->pDirectShm = pShm;
	pPvtData->nextCrfFrameValid = FALSE;
	pPvtData->lastDirectWriterGeneration = pShm->writer_generation;
	pPvtData->lastDirectFrame0WalltimeNs = pShm->frame0_walltime_ns;
	pPvtData->lastDirectMediaTimeFlags = pShm->media_time_flags;
	pPvtData->lastDirectMediaTimeEpoch = pShm->media_time_epoch;
	pPvtData->lastDirectMediaTimeAnchorFrames = pShm->media_time_anchor_frames;
	pPvtData->lastDirectMediaTimeAnchorTaiNs = pShm->media_time_anchor_tai_ns;
	pPvtData->directMediaIdentityLogCount = 0;

	AVB_LOGF_INFO("CRF direct export attached: path=%s rate=%u ring_frames=%u target=%u catchup=%u frames_per_ts=%" PRIu64 " frames_per_pdu=%" PRIu64,
		pPvtData->directDevicePath,
		pPvtData->directAudioRate,
		pShm->ring_frames,
		pPvtData->directTargetLatencyFrames,
		pPvtData->directCatchupFrames,
		pPvtData->directFramesPerTimestamp,
		pPvtData->directFramesPerPdu);
	return TRUE;
}

static bool crfDirectGetBaseTime(pvt_data_t *pPvtData,
				 U64 *pBaseCrfTimeNs,
				 U64 *pBaseFrameIndex,
				 U64 *pCurrentMediaClockNs)
{
	openavb_avb32_direct_shm_t *pShm;
	U64 presentedFrames;
	U64 effectivePresentedFrames;
	U64 projectedPresentedFrames;
	U64 committedFrames;
	U64 desiredFrame;
	U64 leadErrorFrames;
	U64 adjustedFrame0Ns;
	U64 nowPtpNs;
	bool epochChanged;

	if (!pPvtData || !pBaseCrfTimeNs || !pBaseFrameIndex) {
		return FALSE;
	}
	if (!crfDirectOpen(pPvtData)) {
		return FALSE;
	}

	pShm = pPvtData->pDirectShm;
	if (!pShm || pShm->frame0_walltime_ns == 0 || pPvtData->directFramesPerPdu == 0) {
		return FALSE;
	}

	epochChanged = (pShm->writer_generation != pPvtData->lastDirectWriterGeneration)
		|| (pShm->frame0_walltime_ns != pPvtData->lastDirectFrame0WalltimeNs)
		|| (pShm->media_time_flags != pPvtData->lastDirectMediaTimeFlags)
		|| (pShm->media_time_epoch != pPvtData->lastDirectMediaTimeEpoch)
		|| (pShm->media_time_anchor_frames != pPvtData->lastDirectMediaTimeAnchorFrames)
		|| (pShm->media_time_anchor_tai_ns != pPvtData->lastDirectMediaTimeAnchorTaiNs);
	if (epochChanged) {
		pPvtData->nextCrfFrameValid = FALSE;
		pPvtData->lastDirectWriterGeneration = pShm->writer_generation;
		pPvtData->lastDirectFrame0WalltimeNs = pShm->frame0_walltime_ns;
		pPvtData->lastDirectMediaTimeFlags = pShm->media_time_flags;
		pPvtData->lastDirectMediaTimeEpoch = pShm->media_time_epoch;
		pPvtData->lastDirectMediaTimeAnchorFrames = pShm->media_time_anchor_frames;
		pPvtData->lastDirectMediaTimeAnchorTaiNs = pShm->media_time_anchor_tai_ns;
		pPvtData->directMediaIdentityLogCount = 0;
		if (crfHasAuthoritativeMediaTime(pShm)) {
			AVB_LOGF_WARNING(
				"CRF direct media epoch change: epoch=%" PRIu64 " gen=%" PRIu64 " anchor_frames=%" PRIu64 " anchor_tai=%" PRIu64 " flags=0x%" PRIx64 " frame0=%" PRIu64,
				pShm->media_time_epoch,
				pShm->writer_generation,
				pShm->media_time_anchor_frames,
				pShm->media_time_anchor_tai_ns,
				pShm->media_time_flags,
				pShm->frame0_walltime_ns);
		}
	}

	presentedFrames = pShm->presented_frames;
	effectivePresentedFrames = presentedFrames;
	projectedPresentedFrames = presentedFrames;
	committedFrames = pShm->committed_frames;
	adjustedFrame0Ns = 0;
	nowPtpNs = 0;
	if (crfDirectGetAdjustedFrame0Ns(pPvtData, &adjustedFrame0Ns, NULL, &nowPtpNs, NULL) &&
	    nowPtpNs >= adjustedFrame0Ns) {
		projectedPresentedFrames =
			((nowPtpNs - adjustedFrame0Ns) * (U64)pPvtData->directAudioRate) / NSEC_PER_SEC;
		if (projectedPresentedFrames > committedFrames) {
			projectedPresentedFrames = committedFrames;
		}
		if (projectedPresentedFrames > effectivePresentedFrames) {
			effectivePresentedFrames = projectedPresentedFrames;
		}
	}
	desiredFrame = crfAlignUp(
		effectivePresentedFrames + (U64)pPvtData->directTargetLatencyFrames,
		pPvtData->directFramesPerTimestamp);
	if (epochChanged) {
		crfLogSourceOutlierRow(pPvtData,
				      "epoch_change",
				      effectivePresentedFrames,
				      committedFrames,
				      desiredFrame,
				      pPvtData->nextCrfFrameValid ? pPvtData->nextCrfFrameIndex : 0,
				      0,
				      0);
	}

	if (!pPvtData->nextCrfFrameValid) {
		if (committedFrames < (desiredFrame + pPvtData->directFramesPerPdu)) {
			return FALSE;
		}
		pPvtData->nextCrfFrameIndex = desiredFrame;
		pPvtData->nextCrfFrameValid = TRUE;
	}

	if (desiredFrame > pPvtData->nextCrfFrameIndex) {
		leadErrorFrames = desiredFrame - pPvtData->nextCrfFrameIndex;
		if (leadErrorFrames > (U64)pPvtData->directCatchupFrames) {
			pPvtData->nextCrfFrameIndex = desiredFrame;
			pPvtData->directResyncCount++;
			AVB_LOGF_WARNING("CRF direct export resync: desired=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64 " target=%u resyncs=%" PRIu64,
				desiredFrame,
				effectivePresentedFrames,
				committedFrames,
				pPvtData->directTargetLatencyFrames,
				pPvtData->directResyncCount);
			crfLogSourceOutlierRow(pPvtData,
					      "resync",
					      effectivePresentedFrames,
					      committedFrames,
					      desiredFrame,
					      pPvtData->nextCrfFrameIndex,
					      pPvtData->nextCrfFrameIndex,
					      pShm->frame0_walltime_ns +
					      	crfFramesToNs(pPvtData->nextCrfFrameIndex, pPvtData->directAudioRate));
			if (CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, pBaseCrfTimeNs)) {
				crfLogDirectClockDiag(pPvtData,
						      *pBaseCrfTimeNs,
						      effectivePresentedFrames,
						      committedFrames,
						      desiredFrame,
						      pPvtData->nextCrfFrameIndex,
						      pShm->frame0_walltime_ns + crfFramesToNs(pPvtData->nextCrfFrameIndex, pPvtData->directAudioRate),
						      "resync");
			}
		}
	}

	if (committedFrames < (pPvtData->nextCrfFrameIndex + pPvtData->directFramesPerPdu)) {
		return FALSE;
	}

	*pBaseFrameIndex = pPvtData->nextCrfFrameIndex;
	if (!crfDirectGetAdjustedFrame0Ns(pPvtData, &adjustedFrame0Ns, NULL, NULL, NULL)) {
		adjustedFrame0Ns = pShm->frame0_walltime_ns;
	}
	if (pCurrentMediaClockNs) {
		*pCurrentMediaClockNs = adjustedFrame0Ns +
			crfFramesToNs(effectivePresentedFrames, pPvtData->directAudioRate);
	}
	*pBaseCrfTimeNs = adjustedFrame0Ns +
		crfFramesToNs(*pBaseFrameIndex, pPvtData->directAudioRate);
	if (crfHasAuthoritativeMediaTime(pShm) &&
	    (epochChanged || pPvtData->directMediaIdentityLogCount < 8)) {
		U64 identityNowPtpNs = 0;
		if (CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &identityNowPtpNs)) {
			crfLogMediaTimeIdentity(pShm,
						pPvtData->directAudioRate,
						adjustedFrame0Ns,
						effectivePresentedFrames,
						*pBaseFrameIndex,
						*pBaseCrfTimeNs,
						identityNowPtpNs);
			pPvtData->directMediaIdentityLogCount++;
		}
	}

	if (pPvtData->directClockDiagCount < 8 || ((pPvtData->directClockDiagCount % 500ULL) == 0)) {
		U64 nowPtpNs = 0;
		if (CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowPtpNs)) {
			crfLogDirectClockDiag(pPvtData,
					      nowPtpNs,
					      effectivePresentedFrames,
					      committedFrames,
					      desiredFrame,
					      *pBaseFrameIndex,
					      *pBaseCrfTimeNs,
					      "periodic");
		}
	}
	pPvtData->directClockDiagCount++;
	return TRUE;
}

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
	pPvtData->nextCrfFrameValid = FALSE;
	pPvtData->lastTxCalcValid = FALSE;
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
		else if (strcmp(name, "map_nv_crf_direct_device_path") == 0 || strcmp(name, "crf_direct_device_path") == 0) {
			free(pPvtData->directDevicePath);
			pPvtData->directDevicePath = strdup(value);
			crfDirectClose(pPvtData);
		}
		else if (strcmp(name, "map_nv_crf_direct_audio_rate") == 0 || strcmp(name, "crf_direct_audio_rate") == 0) {
			pPvtData->directAudioRate = (U32)strtoul(value, &pEnd, 10);
			crfDirectClose(pPvtData);
		}
		else if (strcmp(name, "map_nv_crf_direct_target_latency_frames") == 0 || strcmp(name, "crf_direct_target_latency_frames") == 0) {
			pPvtData->directTargetLatencyFrames = (U32)strtoul(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_crf_direct_catchup_frames") == 0 || strcmp(name, "crf_direct_catchup_frames") == 0) {
			pPvtData->directCatchupFrames = (U32)strtoul(value, &pEnd, 10);
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
		pPvtData->diagTxCalcOutlierCount = 0;
		pPvtData->lastTxCalcValid = FALSE;
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
			crfLogTimestampOutlierRow(pPvtData,
						 direction,
						 "non_monotonic",
						 seq,
						 firstCrfNs,
						 prevFirstNs,
						 0,
						 expectedDeltaNs,
						 0,
						 0);
		}
		else if (jump) {
			pPvtData->diagTsJumpCount++;
			AVB_LOGF_WARNING("CRF DIAG %s timestamp jump/rebase: seq=%u delta=%lluns expected=%lluns err=%lluns",
				direction,
				(unsigned)seq,
				(unsigned long long)deltaNs,
				(unsigned long long)expectedDeltaNs,
				(unsigned long long)absErrNs);
			crfLogTimestampOutlierRow(pPvtData,
						 direction,
						 "jump",
						 seq,
						 firstCrfNs,
						 prevFirstNs,
						 deltaNs,
						 expectedDeltaNs,
						 absErrNs,
						 jitterThresholdNs);
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
			crfLogTimestampOutlierRow(pPvtData,
						 direction,
						 "jitter",
						 seq,
						 firstCrfNs,
						 prevFirstNs,
						 deltaNs,
						 expectedDeltaNs,
						 absErrNs,
						 jitterThresholdNs);
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

static bool crfShouldLogSparse(U64 count)
{
	return (count < 32ULL) || ((count % 1024ULL) == 0ULL);
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
	U64 cbStartNs = 0;
	U64 afterTimeNs = 0;
	U64 afterBaseNs = 0;
	U64 afterFillNs = 0;
	U64 afterRuntimeNs = 0;
	U64 timeDurationNs = 0;
	U64 baseDurationNs = 0;
	U64 fillDurationNs = 0;
	U64 runtimeDurationNs = 0;
	U64 totalDurationNs = 0;
	const U64 buildWarnNs = 500000ULL;

	bool tu = FALSE;
	struct timespec now = {0};
	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &cbStartNs);
	if (!crfGetTime(&now, &tu)) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}
	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &afterTimeNs);
	U64 nowNs = crfTimespecToNs(&now);
	U32 recoveryGeneration = osalClockGetWalltimeRecoveryGeneration();
	U64 pduPeriodNs = pPvtData->crfPeriodNs * pPvtData->timestampsPerPdu;
	U64 baseFrameIndex = 0;
	U64 currentMediaClockNs = 0;
	bool useDirectExport = (pPvtData->directDevicePath && pPvtData->directDevicePath[0] != '\0');
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
		pPvtData->lastTxCalcValid = FALSE;
		pPvtData->diagTxLeadWindowValid = FALSE;
		pPvtData->diagTxLeadErrMinNs = 0;
		pPvtData->diagTxLeadErrMaxNs = 0;
		pPvtData->diagTxLeadErrSumNs = 0;
		pPvtData->diagTxLeadErrAbsMaxNs = 0;
		pPvtData->diagTxLaunchSkewAbsMaxNs = 0;
		pPvtData->diagTxLeadWindowPackets = 0;
		pPvtData->nextCrfFrameValid = FALSE;
		pPvtData->diagTxCalcOutlierCount = 0;
	}

	// Maintain a free-running CRF timeline. If we're more than one PDU period
	// late versus the scheduled launch, skip ahead by whole periods and continue.
	U64 baseCrfTime = nowNs + pPvtData->roundedMttNs;
	if (useDirectExport) {
		if (!crfDirectGetBaseTime(pPvtData, &baseCrfTime, &baseFrameIndex, &currentMediaClockNs)) {
			AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
			return TX_CB_RET_PACKET_NOT_READY;
		}
		// The direct export path returns the media-clock time of the selected
		// frame. CRF payload timestamps must carry the future clock reference
		// time after the configured transit budget, so align the direct path
		// with the non-direct walltime path by adding roundedMtt here.
		baseCrfTime += pPvtData->roundedMttNs;
		pPvtData->nextCrfTimeValid = TRUE;
	}
	else {
		if (pPvtData->nextCrfTimeValid) {
			baseCrfTime = pPvtData->nextCrfTimeNs;
		}
		else {
			pPvtData->nextCrfTimeValid = TRUE;
		}
	}
	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &afterBaseNs);

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
		crfLogCatchupOutlierRow(pPvtData,
				       useDirectExport,
				       nowNs,
				       baseCrfTime,
				       launchTimeNs,
				       lateNs,
				       skipPeriods,
				       pduPeriodNs,
				       baseFrameIndex);
		baseCrfTime += skipPeriods * pduPeriodNs;
		if (useDirectExport) {
			baseFrameIndex += skipPeriods * pPvtData->directFramesPerPdu;
		}
		launchTimeNs = baseCrfTime - pPvtData->roundedMttNs + pPvtData->launchLeadNs;
	}

	if (useDirectExport) {
		pPvtData->nextCrfFrameIndex = baseFrameIndex + pPvtData->directFramesPerPdu;
		pPvtData->nextCrfFrameValid = TRUE;
		pPvtData->nextCrfTimeNs = baseCrfTime + pduPeriodNs;
	}
	else {
		pPvtData->nextCrfTimeNs = baseCrfTime + pduPeriodNs;
	}
	pPvtData->lastLaunchTimeNs = launchTimeNs;
	pPvtData->lastLaunchValid = TRUE;

	{
		S64 crfLeadNs = (baseCrfTime >= nowNs)
			? (S64)(baseCrfTime - nowNs)
			: -((S64)(nowNs - baseCrfTime));
		S64 launchDeltaNs = (launchTimeNs >= nowNs)
			? (S64)(launchTimeNs - nowNs)
			: -((S64)(nowNs - launchTimeNs));
		U64 txCalcThreshNs = pPvtData->diagJitterThreshNs ? pPvtData->diagJitterThreshNs : CRF_DIAG_DEFAULT_JITTER_THRESH_NS;

		if (pPvtData->lastTxCalcValid) {
			U8 prevSeq = pPvtData->lastTxCalcSeq;
			U8 seqDelta = (U8)(txSeq - prevSeq);
			U64 expectedStepNs = pduPeriodNs * (seqDelta ? (U64)seqDelta : 1ULL);
			S64 baseStepNs = crfDeltaNs(baseCrfTime, pPvtData->lastTxCalcBaseNs);
			S64 launchStepNs = crfDeltaNs(launchTimeNs, pPvtData->lastTxCalcLaunchNs);
			S64 baseErrNs = baseStepNs - (S64)expectedStepNs;
			S64 launchErrNs = launchStepNs - (S64)expectedStepNs;
			const char *txCalcEvent = NULL;

			if (seqDelta == 0) {
				txCalcEvent = "seq_repeat";
			}
			else if (baseStepNs <= 0 || launchStepNs <= 0) {
				txCalcEvent = "non_monotonic";
			}
			else if (llabs(baseErrNs) > (long long)txCalcThreshNs ||
					llabs(launchErrNs) > (long long)txCalcThreshNs) {
				txCalcEvent = "step_jitter";
			}

			if (txCalcEvent) {
				crfLogTxCalcOutlierRow(pPvtData,
						      useDirectExport,
						      txCalcEvent,
						      txSeq,
						      prevSeq,
						      nowNs,
						      baseFrameIndex,
						      baseCrfTime,
						      pPvtData->lastTxCalcBaseNs,
						      launchTimeNs,
						      pPvtData->lastTxCalcLaunchNs,
						      crfLeadNs,
						      launchDeltaNs,
						      baseStepNs,
						      launchStepNs,
						      expectedStepNs,
						      baseErrNs,
						      launchErrNs,
						      pduPeriodNs,
						      txCalcThreshNs);
			}
		}

		pPvtData->lastTxCalcBaseNs = baseCrfTime;
		pPvtData->lastTxCalcLaunchNs = launchTimeNs;
		pPvtData->lastTxCalcSeq = txSeq;
		pPvtData->lastTxCalcValid = TRUE;
	}

	if (pPvtData->diagTxLeadLogCount < 16 || launchTimeNs < nowNs) {
		S64 launchDeltaNs = (launchTimeNs >= nowNs)
			? (S64)(launchTimeNs - nowNs)
			: -((S64)(nowNs - launchTimeNs));
		S64 crfLeadNs = (baseCrfTime >= nowNs)
			? (S64)(baseCrfTime - nowNs)
			: -((S64)(nowNs - baseCrfTime));
		AVB_LOGF_INFO(
			"CRF TX calc: source=%s frame=%" PRIu64 " now=%" PRIu64 " base=%" PRIu64 " crf_lead=%" PRId64 "ns launch=%" PRIu64 " launch_delta=%" PRId64 "ns rounded_mtt=%" PRIu64 "ns launch_lead=%" PRIu64 "ns next_valid=%d seq=%u",
			useDirectExport ? "direct_export" : "walltime",
			baseFrameIndex,
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

	{
		S64 launchDeltaNs = (launchTimeNs >= nowNs)
			? (S64)(launchTimeNs - nowNs)
			: -((S64)(nowNs - launchTimeNs));
		S64 crfLeadNs = (baseCrfTime >= nowNs)
			? (S64)(baseCrfTime - nowNs)
			: -((S64)(nowNs - baseCrfTime));
		U64 warnLeadNs = pPvtData->launchLeadNs / 2ULL;
		const char *event = NULL;

		if (warnLeadNs < 250000ULL) {
			warnLeadNs = 250000ULL;
		}

		if (launchDeltaNs < 0) {
			event = "late_launch";
		}
		else if ((U64)launchDeltaNs < warnLeadNs) {
			event = "low_lead";
		}

		if (event && crfShouldLogSparse(pPvtData->diagTxMarginLogCount)) {
			pPvtData->diagTxMarginLogCount++;
			AVB_LOGF_WARNING(
				"TX OUTLIER ROW flags=.Y. stage=crf_margin event=%s source=%s loc=%u seq=%u now=%" PRIu64 " frame=%" PRIu64 " base=%" PRIu64 " launch=%" PRIu64 " crf_lead_ns=%" PRId64 " launch_lead_ns=%" PRId64 " warn_lead_ns=%" PRIu64 " rounded_mtt_ns=%" PRIu64 " cfg_launch_lead_ns=%" PRIu64 " pdu_period_ns=%" PRIu64 " next_valid=%u catches=%" PRIu64,
				event,
				useDirectExport ? "direct_export" : "walltime",
				(unsigned)pPvtData->runtimeLocationIndex,
				(unsigned)txSeq,
				nowNs,
				baseFrameIndex,
				baseCrfTime,
				launchTimeNs,
				crfLeadNs,
				launchDeltaNs,
				warnLeadNs,
				pPvtData->roundedMttNs,
				pPvtData->launchLeadNs,
				pduPeriodNs,
				pPvtData->nextCrfTimeValid ? 1u : 0u,
				pPvtData->diagTxCatchupCount);
		}
		else if (event) {
			pPvtData->diagTxMarginLogCount++;
		}
	}

	U16 runtimeLocationIndex = crfRuntimeResolveLocationIndex(
		pPvtData,
		OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT);
	U64 publishedMediaClockNs = (useDirectExport && currentMediaClockNs != 0)
		? currentMediaClockNs
		: ((baseCrfTime >= pPvtData->roundedMttNs)
			? (baseCrfTime - pPvtData->roundedMttNs)
			: nowNs);
	if (runtimeLocationIndex == 8) {
		U64 publishNowNs = nowNs;
		if (pPvtData->diagRuntimePublishValid && publishNowNs >= pPvtData->diagRuntimePublishWallNs) {
			U64 wallStepNs = publishNowNs - pPvtData->diagRuntimePublishWallNs;
			S64 mediaStepNs = crfDeltaNs(publishedMediaClockNs, pPvtData->diagRuntimePublishMediaNs);
			S64 crfStepNs = crfDeltaNs(baseCrfTime, pPvtData->diagRuntimePublishCrfNs);
			S64 mediaErrNs = mediaStepNs - (S64)wallStepNs;
			S64 crfErrNs = crfStepNs - (S64)wallStepNs;
			U64 runtimeThreshNs = 50000ULL;
			if (((U64)llabs(mediaErrNs) > runtimeThreshNs ||
					(U64)llabs(crfErrNs) > runtimeThreshNs) &&
					crfShouldLogSparse(pPvtData->diagRuntimePublishOutlierCount)) {
				pPvtData->diagRuntimePublishOutlierCount++;
				AVB_LOGF_WARNING(
					"TX OUTLIER ROW flags=.Y. stage=clocksrc_pub event=output_publish loc=%u seq=%u wall=%" PRIu64 " wall_step=%" PRIu64 " media=%" PRIu64 " media_step=%" PRId64 " media_err=%" PRId64 " crf=%" PRIu64 " crf_step=%" PRId64 " crf_err=%" PRId64 " mtt=%" PRIu64 " count=%" PRIu64,
					(unsigned)runtimeLocationIndex,
					(unsigned)txSeq,
					publishNowNs,
					wallStepNs,
					publishedMediaClockNs,
					mediaStepNs,
					mediaErrNs,
					baseCrfTime,
					crfStepNs,
					crfErrNs,
					pPvtData->roundedMttNs,
					pPvtData->diagRuntimePublishOutlierCount);
			}
			else if ((U64)llabs(mediaErrNs) > runtimeThreshNs ||
					(U64)llabs(crfErrNs) > runtimeThreshNs) {
				pPvtData->diagRuntimePublishOutlierCount++;
			}
		}
		pPvtData->diagRuntimePublishValid = TRUE;
		pPvtData->diagRuntimePublishWallNs = publishNowNs;
		pPvtData->diagRuntimePublishMediaNs = publishedMediaClockNs;
		pPvtData->diagRuntimePublishCrfNs = baseCrfTime;
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
	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &afterFillNs);
	openavbClockSourceRuntimeSetMediaClockForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
		runtimeLocationIndex,
		publishedMediaClockNs,
		tu);
	openavbClockSourceRuntimeSetCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
		runtimeLocationIndex,
		baseCrfTime,
		tu);
	(void)CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &afterRuntimeNs);

	if (afterTimeNs >= cbStartNs) {
		timeDurationNs = afterTimeNs - cbStartNs;
	}
	if (afterBaseNs >= afterTimeNs) {
		baseDurationNs = afterBaseNs - afterTimeNs;
	}
	if (afterFillNs >= afterBaseNs) {
		fillDurationNs = afterFillNs - afterBaseNs;
	}
	if (afterRuntimeNs >= afterFillNs) {
		runtimeDurationNs = afterRuntimeNs - afterFillNs;
	}
	if (afterRuntimeNs >= cbStartNs) {
		totalDurationNs = afterRuntimeNs - cbStartNs;
	}
	if (timeDurationNs >= buildWarnNs ||
	    baseDurationNs >= buildWarnNs ||
	    fillDurationNs >= buildWarnNs ||
	    runtimeDurationNs >= buildWarnNs ||
	    totalDurationNs >= buildWarnNs) {
		crfLogBuildOutlierRow(pPvtData,
				     useDirectExport,
				     txSeq,
				     nowNs,
				     baseFrameIndex,
				     baseCrfTime,
				     launchTimeNs,
				     timeDurationNs,
				     baseDurationNs,
				     fillDurationNs,
				     runtimeDurationNs,
				     totalDurationNs,
				     buildWarnNs);
	}

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
		crfDirectClose(pPvtData);
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

void openavbMapCrfGenEndCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	if (pMediaQ && pMediaQ->pPvtMapInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		crfDirectClose(pPvtData);
		free(pPvtData->directDevicePath);
		pPvtData->directDevicePath = NULL;
	}
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
		pPvtData->directDevicePath = NULL;
		pPvtData->directAudioRate = 0;
		pPvtData->directTargetLatencyFrames = CRF_DIRECT_DEFAULT_TARGET_LATENCY_FRAMES;
		pPvtData->directCatchupFrames = CRF_DIRECT_DEFAULT_CATCHUP_FRAMES;
		pPvtData->directFd = -1;

		crfUpdateDerived(pPvtData);
		openavbMediaQSetMaxLatency(pMediaQ, inMaxTransitUsec);
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return TRUE;
}
