/*************************************************************************************************************
Copyright (c) 2026
All rights reserved.
*************************************************************************************************************/

/*
 * MODULE SUMMARY : Direct 32ch shared-memory playback interface.
 *
 * This is the clean architectural replacement path for bus32split:
 * - a future ALSA playback device/plugin writes 32ch/96k/S32LE audio into a shared-memory ring
 * - OpenAvnu talkers read 8-channel slices directly from that shared timeline
 * - item timestamps come from the writer-provided walltime anchor plus frame index
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <pthread.h>
#include <endian.h>

#include "openavb_platform_pub.h"
#include "openavb_osal_pub.h"
#include "openavb_types_pub.h"
#include "openavb_audio_pub.h"
#include "openavb_trace_pub.h"
#include "openavb_mediaq_pub.h"
#include "openavb_map_uncmp_audio_pub.h"
#include "openavb_map_aaf_audio_pub.h"
#include "openavb_aem_types_pub.h"
#include "openavb_clock_source_runtime_pub.h"
#include "openavb_intf_pub.h"

#include "openavb_avb32_direct_abi.h"

#define AVB_LOG_COMPONENT "AVB32 Direct Interface"
#include "openavb_log_pub.h"

#define AVB32_DIRECT_DEFAULT_SHM_NAME             "/openavb_avb32_direct"
#define AVB32_DIRECT_DEFAULT_DEVICE_PATH          "/dev/avb32out_export"
#define AVB32_DIRECT_STREAM_CHANNELS              8u
#define AVB32_DIRECT_DEFAULT_TARGET_LATENCY_FRAMES 960u
#define AVB32_DIRECT_DEFAULT_HIGH_WATER_FRAMES    1920u
#define AVB32_DIRECT_MAX_GROUPS                   4u
#define AVB32_DIRECT_SHARED_START_GUARD_NS        (10ULL * 1000ULL * 1000ULL)
#define AVB32_DIRECT_SOURCE_STEP_OUTLIER_NS       (250ULL * 1000ULL)

typedef struct {
	bool inUse;
	bool lockInitialized;
	pthread_mutex_t lock;
	char sourceKey[256];
	int shmFd;
	size_t shmBytes;
	openavb_avb32_direct_shm_t *pShm;
	U32 activeMask;
	U32 expectedMask;
	U32 pendingMask;
	U32 waitingLoggedMask;
	U32 readyLoggedMask;
	U32 startSeenMask;
	U32 commitWaitLoggedMask;
	U32 firstPushLoggedMask;
	U32 framesPerItem;
	U32 targetLatencyFrames;
	U32 catchupHighWaterFrames;
	U32 writerGeneration;
	U32 refCount;
	bool started;
	U64 currentFrameIndex;
	U64 cyclePresentedFrames;
	U64 cycleCommittedFrames;
	U64 cycleDesiredFrame;
	U64 cyclePresentationTimeNs;
	U64 resyncCount;
	U64 slewCount;
	U64 stallHoldCount;
	U64 snapshotResetCount;
	U64 sourceStepAnomalyCount;
	U64 sharedStartCycleNs;
	U64 sharedStartIntervalNs;
	U64 lastObservedFrame0Ns;
	U64 lastObservedPresentedFrames;
	U64 lastObservedCommittedFrames;
	U64 lastCycleFrameIndex;
	U64 lastCyclePresentationTimeNs;
	bool snapshotSeen;
	bool cycleTimingSeen;
	U32 sharedStartClaimMask;
} direct_group_t;

static pthread_mutex_t sGroupRegistryLock = PTHREAD_MUTEX_INITIALIZER;
static direct_group_t sDirectGroups[AVB32_DIRECT_MAX_GROUPS];

typedef struct {
	bool ignoreTimestamp;
	char *pShmName;
	char *pDevicePath;

	avb_audio_rate_t audioRate;
	avb_audio_type_t audioType;
	avb_audio_bit_depth_t audioBitDepth;
	avb_audio_endian_t audioEndian;
	avb_audio_channels_t audioChannels;

	U32 streamIndex;
	bool streamIndexSet;
	U32 groupMask;
	U32 targetLatencyFrames;
	U32 catchupHighWaterFrames;
	U32 sourcePresentationOffsetUsec;
	bool constantSignalEnable;
	S32 constantSignalLevel;
	direct_group_t *pGroup;

	int shmFd;
	size_t shmBytes;
	openavb_avb32_direct_shm_t *pShm;

	U64 nextFrameIndex;
	U64 underrunCount;
	U64 clockDiagCount;
	U64 queueTrimCount;
	U64 queueTrimItems;
} pvt_data_t;

static U64 framesToDurationNs(U64 frames, U32 sampleRate)
{
	if (sampleRate == 0) {
		return 0;
	}
	return (frames * NANOSECONDS_PER_SECOND) / sampleRate;
}

static U64 durationNsToFrames(U64 durationNs, U32 sampleRate)
{
	if (sampleRate == 0) {
		return 0;
	}
	return (durationNs * (U64)sampleRate) / NANOSECONDS_PER_SECOND;
}

static bool xHasAuthoritativeMediaTime(const openavb_avb32_direct_shm_t *pShm)
{
	if (!pShm) {
		return FALSE;
	}

	return pShm->version >= 2u
		&& (pShm->media_time_flags & OPENAVB_AVB32_DIRECT_MEDIA_TIME_VALID)
		&& (pShm->media_time_flags & OPENAVB_AVB32_DIRECT_MEDIA_TIME_TAI)
		&& pShm->sample_rate != 0u;
}

static U64 xFrame0FromMediaTime(const openavb_avb32_direct_shm_t *pShm,
				U32 sampleRate)
{
	U64 anchorToFrame0Ns;

	if (!pShm || sampleRate == 0u) {
		return 0;
	}

	anchorToFrame0Ns =
		((U64)pShm->media_time_anchor_frames * NANOSECONDS_PER_SECOND) /
		(U64)sampleRate;
	return (pShm->media_time_anchor_tai_ns > anchorToFrame0Ns)
		? (pShm->media_time_anchor_tai_ns - anchorToFrame0Ns)
		: 0;
}

static const char *xGetSourceKey(const pvt_data_t *pPvtData)
{
	if (!pPvtData) {
		return NULL;
	}
	if (pPvtData->pDevicePath && pPvtData->pDevicePath[0] != '\0') {
		return pPvtData->pDevicePath;
	}
	return pPvtData->pShmName;
}

static bool xRegisterGroup(pvt_data_t *pPvtData)
{
	direct_group_t *pGroup = NULL;
	direct_group_t *pFree = NULL;
	const char *pKey;
	size_t i;

	if (!pPvtData || !pPvtData->streamIndexSet) {
		return FALSE;
	}
	if (pPvtData->pGroup) {
		return TRUE;
	}

	pKey = xGetSourceKey(pPvtData);
	if (!pKey || pKey[0] == '\0') {
		return FALSE;
	}

	pthread_mutex_lock(&sGroupRegistryLock);
	for (i = 0; i < AVB32_DIRECT_MAX_GROUPS; i++) {
		if (sDirectGroups[i].inUse) {
			if (strncmp(sDirectGroups[i].sourceKey, pKey, sizeof(sDirectGroups[i].sourceKey)) == 0) {
				pGroup = &sDirectGroups[i];
				break;
			}
		}
		else if (!pFree) {
			pFree = &sDirectGroups[i];
		}
	}

	if (!pGroup && pFree) {
		pGroup = pFree;
		memset(pGroup, 0, sizeof(*pGroup));
		if (!pGroup->lockInitialized) {
			pthread_mutex_init(&pGroup->lock, NULL);
			pGroup->lockInitialized = TRUE;
		}
		pGroup->inUse = TRUE;
		pGroup->shmFd = -1;
		snprintf(pGroup->sourceKey, sizeof(pGroup->sourceKey), "%s", pKey);
	}

	if (pGroup) {
		pGroup->refCount++;
		pGroup->activeMask |= (1u << pPvtData->streamIndex);
		pPvtData->pGroup = pGroup;
		AVB_LOGF_WARNING("AVB32 direct register: stream=%u active_mask=0x%x ref_count=%u writer_gen=%u",
				pPvtData->streamIndex,
				pGroup->activeMask,
				pGroup->refCount,
				pGroup->writerGeneration);
	}
	pthread_mutex_unlock(&sGroupRegistryLock);

	return (pPvtData->pGroup != NULL);
}

static void xUnregisterGroup(pvt_data_t *pPvtData)
{
	direct_group_t *pGroup;
	U32 streamMask;

	if (!pPvtData || !pPvtData->pGroup || !pPvtData->streamIndexSet) {
		return;
	}

	pGroup = pPvtData->pGroup;
	streamMask = (1u << pPvtData->streamIndex);

	pthread_mutex_lock(&sGroupRegistryLock);
	pGroup->activeMask &= ~streamMask;
	pGroup->pendingMask &= ~streamMask;
	if (pGroup->refCount > 0) {
		pGroup->refCount--;
	}
	if (pGroup->refCount == 0) {
		if (pGroup->pShm && pGroup->shmBytes > 0) {
			munmap(pGroup->pShm, pGroup->shmBytes);
		}
		if (pGroup->shmFd >= 0) {
			close(pGroup->shmFd);
		}
		pGroup->inUse = FALSE;
		pGroup->pShm = NULL;
		pGroup->shmFd = -1;
		pGroup->shmBytes = 0;
		pGroup->started = FALSE;
		pGroup->activeMask = 0;
		pGroup->expectedMask = 0;
		pGroup->pendingMask = 0;
		pGroup->waitingLoggedMask = 0;
		pGroup->readyLoggedMask = 0;
		pGroup->startSeenMask = 0;
		pGroup->commitWaitLoggedMask = 0;
		pGroup->firstPushLoggedMask = 0;
		pGroup->framesPerItem = 0;
		pGroup->targetLatencyFrames = 0;
		pGroup->catchupHighWaterFrames = 0;
		pGroup->writerGeneration = 0;
		pGroup->currentFrameIndex = 0;
		pGroup->cyclePresentedFrames = 0;
		pGroup->cycleCommittedFrames = 0;
		pGroup->cycleDesiredFrame = 0;
		pGroup->cyclePresentationTimeNs = 0;
		pGroup->resyncCount = 0;
		pGroup->slewCount = 0;
		pGroup->stallHoldCount = 0;
		pGroup->sharedStartCycleNs = 0;
		pGroup->sharedStartIntervalNs = 0;
		pGroup->sharedStartClaimMask = 0;
		pGroup->sourceKey[0] = '\0';
	}
	pthread_mutex_unlock(&sGroupRegistryLock);

	pPvtData->pGroup = NULL;
}

static size_t xPageAlign(size_t bytes)
{
	long pageSize = sysconf(_SC_PAGESIZE);
	size_t pageSizeU = (pageSize > 0) ? (size_t)pageSize : 4096u;

	return ((bytes + pageSizeU - 1u) / pageSizeU) * pageSizeU;
}

static S64 xDeltaNs(U64 newer, U64 older)
{
	return (newer >= older)
		? (S64)(newer - older)
		: -((S64)(older - newer));
}

static U64 xAddSignedNs(U64 base, S64 delta)
{
	if (delta >= 0) {
		return base + (U64)delta;
	}
	else {
		U64 absDelta = (U64)(-delta);
		return (base > absDelta) ? (base - absDelta) : 0;
	}
}

static bool xGetSelectedClockFrames(const pvt_data_t *pPvtData,
				    U64 adjustedFrame0Ns,
				    U64 *pSelectedClockNs,
				    U64 *pSelectedFrames,
				    openavb_clock_source_runtime_t *pSelection,
				    U32 *pGeneration)
{
	openavb_clock_source_runtime_t selection = {0};
	U64 selectedClockNs = 0;
	bool uncertain = FALSE;
	U32 generation = 0;

	if (!pPvtData || !pSelectedFrames) {
		return FALSE;
	}
	if (!openavbClockSourceRuntimeGetSelection(&selection)) {
		return FALSE;
	}
	if ((selection.clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID) == 0) {
		return FALSE;
	}
	if (selection.clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT &&
	    selection.clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		return FALSE;
	}
	if (!openavbClockSourceRuntimeGetCrfTimeForLocation(
			selection.clock_source_location_type,
			selection.clock_source_location_index,
			&selectedClockNs,
			&uncertain,
			&generation)) {
		return FALSE;
	}

	*pSelectedFrames = (selectedClockNs > adjustedFrame0Ns)
		? durationNsToFrames(selectedClockNs - adjustedFrame0Ns, (U32)pPvtData->audioRate)
		: 0;
	if (pSelectedClockNs) {
		*pSelectedClockNs = selectedClockNs;
	}
	if (pSelection) {
		*pSelection = selection;
	}
	if (pGeneration) {
		*pGeneration = generation;
	}
	return TRUE;
}

static bool xGetAdjustedFrame0Ns(const pvt_data_t *pPvtData,
				 const openavb_avb32_direct_shm_t *pShm,
				 U64 *pAdjustedFrame0Ns,
				 U64 *pProjectedWriterNs,
				 U64 *pNowPtpNs,
				 S64 *pWriterToPtpNs)
{
	U64 nowPtpNs = 0;
	U64 nowMonoNs = 0;
	U64 projectedWriterNs = 0;
	S64 writerToPtpNs = 0;
	S64 phaseAdjustNs = 0;

	if (!pPvtData || !pShm || !pAdjustedFrame0Ns) {
		return FALSE;
	}
	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowPtpNs)) {
		return FALSE;
	}

	if (xHasAuthoritativeMediaTime(pShm)) {
		projectedWriterNs = pShm->media_time_anchor_tai_ns;
		writerToPtpNs = xDeltaNs(nowPtpNs, projectedWriterNs);
		*pAdjustedFrame0Ns = xFrame0FromMediaTime(
			pShm, (U32)pPvtData->audioRate);

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
	writerToPtpNs = xDeltaNs(nowPtpNs, projectedWriterNs);
	*pAdjustedFrame0Ns = xAddSignedNs(pShm->frame0_walltime_ns, phaseAdjustNs);
	/*
	 * Keep frame0 anchored to the exported presentation timeline.
	 *
	 * Rebasing frame0 against the live projected writer wallclock here makes
	 * the AAF item timestamps inherit writer update quantization. On this
	 * path the export already publishes a phase-adjusted frame0 origin, and
	 * folding writer_to_ptp into that base caused occasional ~1 ms timestamp
	 * inversions that ETF then reordered on wire.
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

static media_q_pub_map_uncmp_audio_info_t *xGetPubMapAudioInfo(media_q_t *pMediaQ);
static bool xOpenSharedMemory(media_q_t *pMediaQ);

static void xLogClockDiag(pvt_data_t *pPvtData,
			  const openavb_avb32_direct_shm_t *pShm,
			  U64 presentedFrames,
			  U64 committedFrames,
			  U64 nextFrameIndex,
			  U64 nextItemTimeNs,
			  const char *reason)
{
	U64 nowPtpNs = 0;
	U64 adjustedFrame0Ns = 0;
	U64 projectedWriterNs = 0;
	U64 presentedTimeNs;
	U64 committedTimeNs;
	S64 presentedSkewNs;
	S64 committedLeadNs;
	S64 nextLeadNs;
	S64 writerToPtpNs;
	S64 phaseAdjustNs;

	if (!pPvtData || !pShm) {
		return;
	}
	if (!xGetAdjustedFrame0Ns(pPvtData,
				  pShm,
				  &adjustedFrame0Ns,
				  &projectedWriterNs,
				  &nowPtpNs,
				  &writerToPtpNs)) {
		return;
	}

	presentedTimeNs = adjustedFrame0Ns +
		framesToDurationNs(presentedFrames, (U32)pPvtData->audioRate);
	committedTimeNs = adjustedFrame0Ns +
		framesToDurationNs(committedFrames, (U32)pPvtData->audioRate);
	presentedSkewNs = xDeltaNs(presentedTimeNs, nowPtpNs);
	committedLeadNs = xDeltaNs(committedTimeNs, nowPtpNs);
	nextLeadNs = xDeltaNs(nextItemTimeNs, nowPtpNs);
	phaseAdjustNs = (S64)pShm->reserved1;

	AVB_LOGF_INFO(
		"AVB32 direct clock diag: stream=%u reason=%s now_ptp=%" PRIu64 " frame0_raw=%" PRIu64 " frame0_adj=%" PRIu64 " writer_proj=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64 " next=%" PRIu64 " presented_skew=%" PRId64 "ns committed_lead=%" PRId64 "ns next_lead=%" PRId64 "ns writer_to_ptp=%" PRId64 "ns phase_adjust=%" PRId64 "ns target=%u",
		pPvtData->streamIndex,
		reason ? reason : "periodic",
		nowPtpNs,
		pShm->frame0_walltime_ns,
		adjustedFrame0Ns,
		projectedWriterNs,
		presentedFrames,
		committedFrames,
		nextFrameIndex,
		presentedSkewNs,
		committedLeadNs,
		nextLeadNs,
		writerToPtpNs,
		phaseAdjustNs,
		pPvtData->targetLatencyFrames);
}

static void xResetSourceCycleTracking(direct_group_t *pGroup)
{
	if (!pGroup) {
		return;
	}
	pGroup->cycleTimingSeen = FALSE;
	pGroup->lastCycleFrameIndex = 0;
	pGroup->lastCyclePresentationTimeNs = 0;
}

static void xMaybeLogSourceStepAnomaly(pvt_data_t *pPvtData,
				       direct_group_t *pGroup,
				       openavb_avb32_direct_shm_t *pShm,
				       U32 framesPerItem,
				       U64 presentedFrames,
				       U64 committedFrames)
{
	U64 frameStep = 0;
	U64 expectedStepNs = 0;
	S64 timeStepNs = 0;
	S64 deltaNs = 0;
	S64 absDeltaNs = 0;
	bool anomaly = FALSE;

	if (!pPvtData || !pGroup || !pShm) {
		return;
	}

	if (pGroup->cycleTimingSeen) {
		if (pGroup->currentFrameIndex >= pGroup->lastCycleFrameIndex) {
			frameStep = pGroup->currentFrameIndex - pGroup->lastCycleFrameIndex;
		}
		else {
			anomaly = TRUE;
		}

		timeStepNs = xDeltaNs(pGroup->cyclePresentationTimeNs, pGroup->lastCyclePresentationTimeNs);
		expectedStepNs = framesToDurationNs(frameStep, (U32)pPvtData->audioRate);
		deltaNs = timeStepNs - (S64)expectedStepNs;
		absDeltaNs = (deltaNs >= 0) ? deltaNs : -deltaNs;

		if (timeStepNs <= 0
			|| frameStep != (U64)framesPerItem
			|| absDeltaNs > (S64)AVB32_DIRECT_SOURCE_STEP_OUTLIER_NS) {
			anomaly = TRUE;
		}

		if (anomaly) {
			pGroup->sourceStepAnomalyCount++;
			AVB_LOGF_WARNING(
				"TX OUTLIER ROW flags=X.. stage=source stream=%u src=%" PRIu64 " frame=%" PRIu64 " prev_frame=%" PRIu64 " frame_step=%" PRIu64 " nominal_frame_step=%u step_ns=%" PRId64 " expected_ns=%" PRIu64 " delta_ns=%" PRId64 " presented=%" PRIu64 " committed=%" PRIu64 " gen=%u anomalies=%" PRIu64,
				pPvtData->streamIndex,
				pGroup->cyclePresentationTimeNs,
				pGroup->currentFrameIndex,
				pGroup->lastCycleFrameIndex,
				frameStep,
				framesPerItem,
				timeStepNs,
				expectedStepNs,
				deltaNs,
				presentedFrames,
				committedFrames,
				pGroup->writerGeneration,
				pGroup->sourceStepAnomalyCount);
			AVB_LOGF_WARNING(
				"AVB32 direct source step anomaly: stream=%u gen=%u frame=%" PRIu64 " prev_frame=%" PRIu64 " frame_step=%" PRIu64 " nominal_frame_step=%u ts=%" PRIu64 " prev_ts=%" PRIu64 " time_step=%" PRId64 "ns expected_step=%" PRIu64 "ns delta=%" PRId64 "ns presented=%" PRIu64 " committed=%" PRIu64 " target=%u anomalies=%" PRIu64 " resyncs=%" PRIu64 " snapshot_resets=%" PRIu64,
				pPvtData->streamIndex,
				pGroup->writerGeneration,
				pGroup->currentFrameIndex,
				pGroup->lastCycleFrameIndex,
				frameStep,
				framesPerItem,
				pGroup->cyclePresentationTimeNs,
				pGroup->lastCyclePresentationTimeNs,
				timeStepNs,
				expectedStepNs,
				deltaNs,
				presentedFrames,
				committedFrames,
				pGroup->targetLatencyFrames,
				pGroup->sourceStepAnomalyCount,
				pGroup->resyncCount,
				pGroup->snapshotResetCount);
		}
	}

	pGroup->lastCycleFrameIndex = pGroup->currentFrameIndex;
	pGroup->lastCyclePresentationTimeNs = pGroup->cyclePresentationTimeNs;
	pGroup->cycleTimingSeen = TRUE;
}

static void xLogSourceHoldRow(pvt_data_t *pPvtData,
			      direct_group_t *pGroup,
			      openavb_avb32_direct_shm_t *pShm,
			      U64 presentedFrames,
			      U64 committedFrames,
			      U64 desiredFrame,
			      U64 nextFrame,
			      S64 aheadFrames)
{
	U64 adjustedFrame0Ns = 0;
	U64 nextSourceNs = 0;

	if (!pPvtData || !pGroup || !pShm) {
		return;
	}
	if (!xGetAdjustedFrame0Ns(pPvtData, pShm, &adjustedFrame0Ns, NULL, NULL, NULL)) {
		adjustedFrame0Ns = pShm->frame0_walltime_ns;
	}
	nextSourceNs = adjustedFrame0Ns +
		framesToDurationNs(nextFrame, (U32)pPvtData->audioRate) +
		((U64)pPvtData->sourcePresentationOffsetUsec * 1000ULL);

	AVB_LOGF_WARNING(
		"TX OUTLIER ROW flags=X.. stage=source_hold stream=%u src=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " next=%" PRIu64 " ahead=%" PRId64 " target=%u frame0=%" PRIu64 " gen=%u holds=%" PRIu64,
		pPvtData->streamIndex,
		nextSourceNs,
		presentedFrames,
		committedFrames,
		desiredFrame,
		nextFrame,
		aheadFrames,
		pGroup->targetLatencyFrames,
		pShm->frame0_walltime_ns,
		pGroup->writerGeneration,
		pGroup->stallHoldCount);
}

static bool xPrepareCycleLocked(media_q_t *pMediaQ,
				 pvt_data_t *pPvtData,
				 media_q_pub_map_uncmp_audio_info_t *pInfo,
				 direct_group_t *pGroup,
				 openavb_avb32_direct_shm_t *pShm,
				 bool allowStartWithoutBarrier,
				 U64 *pFrameIndex,
				 U64 *pPresentationTimeNs,
				 U64 *pPresentedFrames,
				 U64 *pCommittedFrames)
{
	U32 framesPerItem;
	U64 committedFrames;
	U64 presentedFrames;
	U64 desiredFrame;
	U64 effectivePresentedFrames;
	U64 projectedPresentedFrames;
	U64 leadErrorFrames;
	U64 prevPresentedFrames;
	U64 prevCommittedFrames;
	U64 prevDesiredFrame;
	U64 selectedClockNs;
	U64 selectedFrames;
	S64 frameErr;
	U64 adjustedFrame0Ns;
	U64 nowPtpNs;
	U32 streamMask;
	U64 presentedLagFrames;
	bool snapshotUnchanged;
	bool newCycle;
	bool haveSelectedClock;
	openavb_clock_source_runtime_t selectedClockSelection;
	U32 selectedClockGeneration;

	if (!pMediaQ || !pPvtData || !pInfo || !pGroup || !pShm || !pPvtData->streamIndexSet) {
		return FALSE;
	}

	framesPerItem = pInfo->framesPerItem;
	if (framesPerItem == 0) {
		return FALSE;
	}

	streamMask = (1u << pPvtData->streamIndex);
	committedFrames = pShm->committed_frames;
	presentedFrames = pShm->presented_frames;
	effectivePresentedFrames = presentedFrames;
	projectedPresentedFrames = presentedFrames;
	adjustedFrame0Ns = 0;
	nowPtpNs = 0;
	presentedLagFrames = 0;
	selectedClockNs = 0;
	selectedFrames = 0;
	haveSelectedClock = FALSE;
	memset(&selectedClockSelection, 0, sizeof(selectedClockSelection));
	selectedClockGeneration = 0;

	if (xGetAdjustedFrame0Ns(pPvtData,
				 pShm,
				 &adjustedFrame0Ns,
				 NULL,
				 &nowPtpNs,
				 NULL) &&
	    nowPtpNs >= adjustedFrame0Ns) {
		projectedPresentedFrames = durationNsToFrames(
			nowPtpNs - adjustedFrame0Ns,
			(U32)pPvtData->audioRate);
		if (projectedPresentedFrames > committedFrames) {
			projectedPresentedFrames = committedFrames;
		}
		if (projectedPresentedFrames > effectivePresentedFrames) {
			presentedLagFrames = projectedPresentedFrames - effectivePresentedFrames;
			if (presentedLagFrames >= (U64)framesPerItem) {
				effectivePresentedFrames = projectedPresentedFrames;
				pGroup->slewCount++;
				if (pGroup->slewCount <= 16ULL || ((pGroup->slewCount % 1024ULL) == 0ULL)) {
					AVB_LOGF_WARNING(
						"AVB32 direct advance source cursor: stream=%u presented=%" PRIu64 " projected=%" PRIu64 " committed=%" PRIu64 " lag=%" PRIu64 " target=%u slews=%" PRIu64,
						pPvtData->streamIndex,
						presentedFrames,
						projectedPresentedFrames,
						committedFrames,
						presentedLagFrames,
						pGroup->targetLatencyFrames ? pGroup->targetLatencyFrames : pPvtData->targetLatencyFrames,
						pGroup->slewCount);
				}
			}
		}
		haveSelectedClock = xGetSelectedClockFrames(
			pPvtData,
			adjustedFrame0Ns,
			&selectedClockNs,
			&selectedFrames,
			&selectedClockSelection,
			&selectedClockGeneration);
		if (haveSelectedClock &&
		    (selectedFrames < effectivePresentedFrames || selectedFrames > committedFrames)) {
			haveSelectedClock = FALSE;
			selectedClockNs = 0;
			selectedFrames = 0;
			memset(&selectedClockSelection, 0, sizeof(selectedClockSelection));
			selectedClockGeneration = 0;
		}
	}

	if (pGroup->writerGeneration != pShm->writer_generation) {
		AVB_LOGF_WARNING("AVB32 direct writer generation change: stream=%u old=%u new=%u frame0=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64,
				pPvtData->streamIndex,
				pGroup->writerGeneration,
				pShm->writer_generation,
				pShm->frame0_walltime_ns,
				presentedFrames,
				committedFrames);
		pGroup->writerGeneration = pShm->writer_generation;
		pGroup->started = FALSE;
		pGroup->pendingMask = 0;
		pGroup->waitingLoggedMask = 0;
		pGroup->readyLoggedMask = 0;
		pGroup->startSeenMask = 0;
		pGroup->commitWaitLoggedMask = 0;
		pGroup->firstPushLoggedMask = 0;
		pGroup->currentFrameIndex = 0;
		pGroup->cyclePresentedFrames = 0;
		pGroup->cycleCommittedFrames = 0;
		pGroup->cycleDesiredFrame = 0;
		pGroup->cyclePresentationTimeNs = 0;
		pGroup->snapshotSeen = FALSE;
		pGroup->lastObservedFrame0Ns = 0;
		pGroup->lastObservedPresentedFrames = 0;
		pGroup->lastObservedCommittedFrames = 0;
		xResetSourceCycleTracking(pGroup);
	}

	if (pGroup->snapshotSeen) {
		bool frame0Regressed = pShm->frame0_walltime_ns < pGroup->lastObservedFrame0Ns;
		bool presentedRegressed = presentedFrames < pGroup->lastObservedPresentedFrames;
		bool committedRegressed = committedFrames < pGroup->lastObservedCommittedFrames;

		if (frame0Regressed || presentedRegressed || committedRegressed) {
			pGroup->snapshotResetCount++;
			AVB_LOGF_WARNING("AVB32 direct snapshot regression: stream=%u gen=%u frame0=%" PRIu64 "->%" PRIu64 " presented=%" PRIu64 "->%" PRIu64 " committed=%" PRIu64 "->%" PRIu64 " resets=%" PRIu64,
					pPvtData->streamIndex,
					pGroup->writerGeneration,
					pGroup->lastObservedFrame0Ns,
					pShm->frame0_walltime_ns,
					pGroup->lastObservedPresentedFrames,
					presentedFrames,
					pGroup->lastObservedCommittedFrames,
					committedFrames,
					pGroup->snapshotResetCount);
			xResetSourceCycleTracking(pGroup);
		}
	}
	pGroup->snapshotSeen = TRUE;
	pGroup->lastObservedFrame0Ns = pShm->frame0_walltime_ns;
	pGroup->lastObservedPresentedFrames = presentedFrames;
	pGroup->lastObservedCommittedFrames = committedFrames;

	if (pGroup->framesPerItem == 0) {
		pGroup->framesPerItem = framesPerItem;
	}
	if (pGroup->expectedMask == 0) {
		pGroup->expectedMask = pPvtData->groupMask;
	}
	if (pGroup->targetLatencyFrames == 0) {
		pGroup->targetLatencyFrames = pPvtData->targetLatencyFrames;
	}
	if (pGroup->catchupHighWaterFrames == 0) {
		pGroup->catchupHighWaterFrames = pPvtData->catchupHighWaterFrames;
	}

	if ((pGroup->activeMask & pGroup->expectedMask) != pGroup->expectedMask) {
		if (!(pGroup->waitingLoggedMask & streamMask)) {
			pGroup->waitingLoggedMask |= streamMask;
			AVB_LOGF_WARNING("AVB32 direct waiting for group: stream=%u active_mask=0x%x expected_mask=0x%x pending_mask=0x%x started=%u writer_gen=%u presented=%" PRIu64 " committed=%" PRIu64,
					pPvtData->streamIndex,
					pGroup->activeMask,
					pGroup->expectedMask,
					pGroup->pendingMask,
					pGroup->started ? 1u : 0u,
					pGroup->writerGeneration,
					presentedFrames,
					committedFrames);
		}
		return FALSE;
	}

	if (!(pGroup->readyLoggedMask & streamMask)) {
		pGroup->readyLoggedMask |= streamMask;
		AVB_LOGF_WARNING("AVB32 direct group ready: stream=%u active_mask=0x%x expected_mask=0x%x pending_mask=0x%x started=%u writer_gen=%u presented=%" PRIu64 " committed=%" PRIu64,
				pPvtData->streamIndex,
				pGroup->activeMask,
				pGroup->expectedMask,
				pGroup->pendingMask,
				pGroup->started ? 1u : 0u,
				pGroup->writerGeneration,
				presentedFrames,
				committedFrames);
	}

	if (!pGroup->started && !allowStartWithoutBarrier) {
		pGroup->startSeenMask |= streamMask;
		if (pGroup->startSeenMask != pGroup->expectedMask) {
			return FALSE;
		}
	}

	newCycle = (!pGroup->started || pGroup->pendingMask == 0);
	if (newCycle) {
		prevPresentedFrames = pGroup->cyclePresentedFrames;
		prevCommittedFrames = pGroup->cycleCommittedFrames;
		prevDesiredFrame = pGroup->cycleDesiredFrame;
		desiredFrame = (haveSelectedClock ? selectedFrames : effectivePresentedFrames) +
			(U64)pGroup->targetLatencyFrames;
		snapshotUnchanged = pGroup->started
			&& prevPresentedFrames == effectivePresentedFrames
			&& prevCommittedFrames == committedFrames
			&& prevDesiredFrame == desiredFrame;
		pGroup->cyclePresentedFrames = effectivePresentedFrames;
		pGroup->cycleCommittedFrames = committedFrames;
		pGroup->cycleDesiredFrame = desiredFrame;
		if (committedFrames < (desiredFrame + (U64)framesPerItem)) {
			if (!(pGroup->commitWaitLoggedMask & streamMask)) {
				pGroup->commitWaitLoggedMask |= streamMask;
				AVB_LOGF_WARNING("AVB32 direct waiting for data: stream=%u presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " need=%" PRIu64 " frames_per_item=%u target=%u started=%u pending_mask=0x%x",
						pPvtData->streamIndex,
						effectivePresentedFrames,
						committedFrames,
						desiredFrame,
						desiredFrame + (U64)framesPerItem,
						framesPerItem,
						pGroup->targetLatencyFrames,
						pGroup->started ? 1u : 0u,
						pGroup->pendingMask);
			}
			return FALSE;
		}
		pGroup->commitWaitLoggedMask &= ~streamMask;

		if (pGroup->started) {
			U64 nextFrame = pGroup->currentFrameIndex + (U64)framesPerItem;
			frameErr = (desiredFrame >= nextFrame)
				? (S64)(desiredFrame - nextFrame)
				: -((S64)(nextFrame - desiredFrame));
			if (frameErr > 0) {
				leadErrorFrames = (U64)frameErr;
				if (leadErrorFrames > (U64)pGroup->catchupHighWaterFrames) {
					nextFrame = desiredFrame;
					pGroup->resyncCount++;
					AVB_LOGF_WARNING("AVB32 direct resync: stream=%u presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " next=%" PRIu64 " frame_err=%" PRId64 " target=%u resyncs=%" PRIu64,
							pPvtData->streamIndex,
							effectivePresentedFrames,
							committedFrames,
							desiredFrame,
							nextFrame,
							frameErr,
							pGroup->targetLatencyFrames,
							pGroup->resyncCount);
					xLogClockDiag(pPvtData,
						      pShm,
						      effectivePresentedFrames,
						      committedFrames,
						      nextFrame,
						      pShm->frame0_walltime_ns + framesToDurationNs(nextFrame, (U32)pPvtData->audioRate),
						      "resync");
				}
			}
			else if (frameErr < 0) {
				S64 aheadFrames = -frameErr;
				if ((U64)aheadFrames > (U64)pGroup->catchupHighWaterFrames) {
					if (snapshotUnchanged) {
						pGroup->stallHoldCount++;
						if (pGroup->stallHoldCount <= 16ULL || ((pGroup->stallHoldCount % 1024ULL) == 0ULL)) {
							xLogSourceHoldRow(pPvtData,
								      pGroup,
								      pShm,
								      effectivePresentedFrames,
								      committedFrames,
								      desiredFrame,
								      nextFrame,
								      aheadFrames);
							AVB_LOGF_WARNING("AVB32 direct hold on frozen source: stream=%u presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " next=%" PRIu64 " ahead=%" PRId64 " target=%u holds=%" PRIu64,
									pPvtData->streamIndex,
									effectivePresentedFrames,
									committedFrames,
									desiredFrame,
									nextFrame,
									aheadFrames,
									pGroup->targetLatencyFrames,
									pGroup->stallHoldCount);
							xLogClockDiag(pPvtData,
								      pShm,
								      effectivePresentedFrames,
								      committedFrames,
								      nextFrame,
								      pShm->frame0_walltime_ns + framesToDurationNs(nextFrame, (U32)pPvtData->audioRate),
								      "frozen_hold");
						}
						return FALSE;
					}
					nextFrame = desiredFrame;
					pGroup->resyncCount++;
					AVB_LOGF_WARNING("AVB32 direct reverse resync: stream=%u presented=%" PRIu64 " committed=%" PRIu64 " desired=%" PRIu64 " next=%" PRIu64 " ahead=%" PRId64 " target=%u resyncs=%" PRIu64,
							pPvtData->streamIndex,
							effectivePresentedFrames,
							committedFrames,
							desiredFrame,
							nextFrame,
							aheadFrames,
							pGroup->targetLatencyFrames,
							pGroup->resyncCount);
					xLogClockDiag(pPvtData,
						      pShm,
						      effectivePresentedFrames,
						      committedFrames,
						      nextFrame,
						      pShm->frame0_walltime_ns + framesToDurationNs(nextFrame, (U32)pPvtData->audioRate),
						      "reverse_resync");
				}
			}
			pGroup->stallHoldCount = 0;
			pGroup->currentFrameIndex = nextFrame;
		}
		else {
			pGroup->currentFrameIndex = desiredFrame;
			pGroup->started = TRUE;
			pGroup->stallHoldCount = 0;
			AVB_LOGF_WARNING("AVB32 direct group start: stream=%u active_mask=0x%x expected_mask=0x%x desired=%" PRIu64 " frames_per_item=%u target=%u presented=%" PRIu64 " committed=%" PRIu64 " mode=%s clock=%s loc=%s/%u clk_gen=%u crf_gen=%u",
					pPvtData->streamIndex,
					pGroup->activeMask,
					pGroup->expectedMask,
					desiredFrame,
					framesPerItem,
					pGroup->targetLatencyFrames,
					effectivePresentedFrames,
					committedFrames,
					allowStartWithoutBarrier ? "prepared" : "barrier",
					haveSelectedClock ? "selected" : "source",
					haveSelectedClock
						? ((selectedClockSelection.clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT)
							? "input"
							: "output")
						: "none",
					haveSelectedClock ? selectedClockSelection.clock_source_location_index : 0U,
					haveSelectedClock ? selectedClockSelection.generation : 0U,
					haveSelectedClock ? selectedClockGeneration : 0U);
		}
		pGroup->startSeenMask = 0;
		pGroup->pendingMask = pGroup->expectedMask ? pGroup->expectedMask : streamMask;
	}

	if (!xGetAdjustedFrame0Ns(pPvtData, pShm, &adjustedFrame0Ns, NULL, NULL, NULL)) {
		adjustedFrame0Ns = pShm->frame0_walltime_ns;
	}
	pGroup->cyclePresentationTimeNs = adjustedFrame0Ns +
		framesToDurationNs(pGroup->currentFrameIndex, (U32)pPvtData->audioRate) +
		((U64)pPvtData->sourcePresentationOffsetUsec * 1000ULL);
	if (newCycle) {
		xMaybeLogSourceStepAnomaly(pPvtData,
					 pGroup,
					 pShm,
					 framesPerItem,
					 presentedFrames,
					 committedFrames);
	}

	if (pFrameIndex) {
		*pFrameIndex = pGroup->currentFrameIndex;
	}
	if (pPresentationTimeNs) {
		*pPresentationTimeNs = pGroup->cyclePresentationTimeNs;
	}
	if (pPresentedFrames) {
		*pPresentedFrames = effectivePresentedFrames;
	}
	if (pCommittedFrames) {
		*pCommittedFrames = committedFrames;
	}

	return TRUE;
}

static bool openavbIntfAvb32DirectGetTxStartCycleCB(media_q_t *pMediaQ, U64 intervalNS, U64 *pNextCycleNS)
{
	pvt_data_t *pPvtData;
	direct_group_t *pGroup;
	U32 streamMask;
	U64 nowWallNs = 0;
	U64 proposedCycleNs = 0;

	if (!pMediaQ || !pMediaQ->pPvtIntfInfo || !pNextCycleNS || intervalNS == 0) {
		return FALSE;
	}

	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	if (!pPvtData->streamIndexSet) {
		return FALSE;
	}
	if (!xRegisterGroup(pPvtData) || !pPvtData->pGroup) {
		return FALSE;
	}
	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowWallNs)) {
		return FALSE;
	}

	pGroup = pPvtData->pGroup;
	streamMask = (1u << pPvtData->streamIndex);

	pthread_mutex_lock(&pGroup->lock);
	if (pGroup->sharedStartIntervalNs == 0 || pGroup->sharedStartIntervalNs != intervalNS) {
		pGroup->sharedStartIntervalNs = intervalNS;
		pGroup->sharedStartCycleNs = 0;
		pGroup->sharedStartClaimMask = 0;
	}
	if (pGroup->sharedStartCycleNs == 0) {
		proposedCycleNs = nowWallNs + AVB32_DIRECT_SHARED_START_GUARD_NS;
		proposedCycleNs = ((proposedCycleNs + intervalNS - 1ULL) / intervalNS) * intervalNS;
		pGroup->sharedStartCycleNs = proposedCycleNs;
	}
	pGroup->sharedStartClaimMask |= streamMask;
	*pNextCycleNS = pGroup->sharedStartCycleNs;
	AVB_LOGF_WARNING("AVB32 direct shared start cycle: stream=%u next_cycle=%" PRIu64 " interval=%" PRIu64 " claims=0x%x expected=0x%x active=0x%x",
			pPvtData->streamIndex,
			pGroup->sharedStartCycleNs,
			intervalNS,
			pGroup->sharedStartClaimMask,
			pGroup->expectedMask ? pGroup->expectedMask : pPvtData->groupMask,
			pGroup->activeMask);
	pthread_mutex_unlock(&pGroup->lock);

	return TRUE;
}

static bool openavbIntfAvb32DirectPrepareTxCycleCB(media_q_t *pMediaQ, U64 intervalNS, U64 *pPresentationTimeNS)
{
	pvt_data_t *pPvtData;
	direct_group_t *pGroup;
	media_q_pub_map_uncmp_audio_info_t *pInfo;
	openavb_avb32_direct_shm_t *pShm;
	U64 frameIndex = 0;
	U64 presentedFrames = 0;
	U64 committedFrames = 0;
	U64 presentationTimeNs = 0;
	U64 nowWallNs = 0;
	U64 staleThresholdNs = (intervalNS != 0) ? (intervalNS * 4ULL) : 0ULL;
	if (staleThresholdNs != 0 && staleThresholdNs < 2000000ULL) {
		staleThresholdNs = 2000000ULL;
	}

	if (!pMediaQ || !pMediaQ->pPvtIntfInfo || !pPresentationTimeNS) {
		return FALSE;
	}

	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pInfo = xGetPubMapAudioInfo(pMediaQ);
	if (!pInfo || !pPvtData->streamIndexSet) {
		return FALSE;
	}
	if (!pPvtData->pShm && !xOpenSharedMemory(pMediaQ)) {
		return FALSE;
	}
	pShm = pPvtData->pShm;
	if (!pShm || pShm->frame0_walltime_ns == 0) {
		return FALSE;
	}
	if (!xRegisterGroup(pPvtData) || !pPvtData->pGroup) {
		return FALSE;
	}

	pGroup = pPvtData->pGroup;
	pthread_mutex_lock(&pGroup->lock);
	if (staleThresholdNs != 0 &&
			pGroup->pendingMask != 0 &&
			pGroup->cyclePresentationTimeNs != 0 &&
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowWallNs) &&
			(pGroup->cyclePresentationTimeNs + staleThresholdNs) < nowWallNs) {
		AVB_LOGF_WARNING("AVB32 direct discard stale prepared cycle: stream=%u presentation=%" PRIu64 " now=%" PRIu64 " threshold=%" PRIu64 " pending_mask=0x%x frame=%" PRIu64,
				pPvtData->streamIndex,
				pGroup->cyclePresentationTimeNs,
				nowWallNs,
				staleThresholdNs,
				pGroup->pendingMask,
				pGroup->currentFrameIndex);
		pGroup->pendingMask = 0;
		pGroup->cyclePresentationTimeNs = 0;
		pGroup->sharedStartCycleNs = 0;
	}
	if (!xPrepareCycleLocked(pMediaQ,
				 pPvtData,
				 pInfo,
				 pGroup,
				 pShm,
				 TRUE,
				 &frameIndex,
				 &presentationTimeNs,
				 &presentedFrames,
				 &committedFrames)) {
		pthread_mutex_unlock(&pGroup->lock);
		return FALSE;
	}
	pthread_mutex_unlock(&pGroup->lock);

	*pPresentationTimeNS = presentationTimeNs;
	return TRUE;
}

static media_q_pub_map_uncmp_audio_info_t *xGetPubMapAudioInfo(media_q_t *pMediaQ)
{
	if (!pMediaQ || !pMediaQ->pPubMapInfo || !pMediaQ->pMediaQDataFormat) {
		return NULL;
	}
	if (strcmp(pMediaQ->pMediaQDataFormat, MapUncmpAudioMediaQDataFormat) == 0
		|| strcmp(pMediaQ->pMediaQDataFormat, MapAVTPAudioMediaQDataFormat) == 0) {
		return (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	}
	return NULL;
}

static void xSyncAudioInfo(media_q_t *pMediaQ, const pvt_data_t *pPvtData)
{
	media_q_pub_map_uncmp_audio_info_t *pInfo = xGetPubMapAudioInfo(pMediaQ);
	if (!pInfo || !pPvtData) {
		return;
	}
	pInfo->audioRate = pPvtData->audioRate;
	pInfo->audioType = pPvtData->audioType;
	pInfo->audioBitDepth = pPvtData->audioBitDepth;
	pInfo->audioEndian = pPvtData->audioEndian;
	pInfo->audioChannels = pPvtData->audioChannels;
}

static void xCloseSharedMemory(pvt_data_t *pPvtData)
{
	if (!pPvtData) {
		return;
	}
	pPvtData->pShm = NULL;
	pPvtData->shmBytes = 0;
	pPvtData->shmFd = -1;
	pPvtData->nextFrameIndex = 0;
	xUnregisterGroup(pPvtData);
}

static bool xOpenSharedMemory(media_q_t *pMediaQ)
{
	struct stat st;
	openavb_avb32_direct_shm_t *pShm = NULL;
	pvt_data_t *pPvtData = NULL;
	int fd = -1;
	size_t minBytes = 0;
	size_t mapBytes = 0;
	bool useDevicePath = FALSE;

	if (!pMediaQ || !pMediaQ->pPvtIntfInfo) {
		return FALSE;
	}

	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	if (pPvtData->pShm) {
		return TRUE;
	}
	if (!xRegisterGroup(pPvtData) || !pPvtData->pGroup) {
		return FALSE;
	}

	pthread_mutex_lock(&pPvtData->pGroup->lock);
	if (pPvtData->pGroup->pShm) {
		pPvtData->shmFd = -1;
		pPvtData->shmBytes = pPvtData->pGroup->shmBytes;
		pPvtData->pShm = pPvtData->pGroup->pShm;
		pthread_mutex_unlock(&pPvtData->pGroup->lock);
		AVB_LOGF_WARNING("AVB32 direct attached shared: uid=%u source=%s ring_frames=%u rate=%u refs=%u",
				pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
				xGetSourceKey(pPvtData),
				pPvtData->pShm->ring_frames,
				pPvtData->pShm->sample_rate,
				pPvtData->pGroup->refCount);
		return TRUE;
	}

	useDevicePath = (pPvtData->pDevicePath && pPvtData->pDevicePath[0] != '\0');
	if (useDevicePath) {
		fd = open(pPvtData->pDevicePath, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			pthread_mutex_unlock(&pPvtData->pGroup->lock);
			xUnregisterGroup(pPvtData);
			AVB_LOGF_WARNING("AVB32 direct: open device failed: stream=%u path=%s errno=%d (%s)",
					pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
					pPvtData->pDevicePath,
					errno,
					strerror(errno));
			return FALSE;
		}
		mapBytes = xPageAlign(openavbAvb32DirectShmBytes(OPENAVB_AVB32_DIRECT_KERNEL_RING_FRAMES));
	}
	else {
		fd = shm_open(pPvtData->pShmName, O_RDONLY, 0);
		if (fd < 0) {
			pthread_mutex_unlock(&pPvtData->pGroup->lock);
			xUnregisterGroup(pPvtData);
			AVB_LOGF_WARNING("AVB32 direct: shm_open failed: stream=%u shm=%s errno=%d (%s)",
					pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
					pPvtData->pShmName ? pPvtData->pShmName : "(null)",
					errno,
					strerror(errno));
			return FALSE;
		}
		if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(openavb_avb32_direct_shm_t)) {
			pthread_mutex_unlock(&pPvtData->pGroup->lock);
			xUnregisterGroup(pPvtData);
			AVB_LOGF_WARNING("AVB32 direct: fstat/size failed: stream=%u shm=%s errno=%d (%s) size=%lld",
					pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
					pPvtData->pShmName ? pPvtData->pShmName : "(null)",
					errno,
					strerror(errno),
					(long long)st.st_size);
			close(fd);
			return FALSE;
		}
		mapBytes = (size_t)st.st_size;
	}

	pShm = mmap(NULL, mapBytes, PROT_READ, MAP_SHARED, fd, 0);
	if (pShm == MAP_FAILED) {
		pthread_mutex_unlock(&pPvtData->pGroup->lock);
		xUnregisterGroup(pPvtData);
		AVB_LOGF_WARNING("AVB32 direct: mmap failed: stream=%u source=%s errno=%d (%s) bytes=%zu",
				pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
				useDevicePath ? pPvtData->pDevicePath : (pPvtData->pShmName ? pPvtData->pShmName : "(null)"),
				errno,
				strerror(errno),
				mapBytes);
		close(fd);
		return FALSE;
	}

	minBytes = (pShm->header_bytes != 0 ? pShm->header_bytes : sizeof(*pShm))
		+ ((size_t)pShm->ring_frames * openavbAvb32DirectFrameBytes());
	if (pShm->magic != OPENAVB_AVB32_DIRECT_ABI_MAGIC
			|| (pShm->version != 1u
				&& pShm->version != OPENAVB_AVB32_DIRECT_ABI_VERSION)
			|| pShm->channel_count != OPENAVB_AVB32_DIRECT_CHANNELS
			|| pShm->bytes_per_sample != OPENAVB_AVB32_DIRECT_BYTES_PER_SAMPLE
			|| pShm->sample_rate != (U32)pPvtData->audioRate
			|| pShm->ring_frames == 0
			|| pPvtData->audioChannels != AVB32_DIRECT_STREAM_CHANNELS
			|| mapBytes < minBytes) {
		pthread_mutex_unlock(&pPvtData->pGroup->lock);
		xUnregisterGroup(pPvtData);
		munmap(pShm, mapBytes);
		close(fd);
		AVB_LOGF_ERROR("AVB32 direct: shared memory validation failed: stream=%u source=%s magic=0x%08x version=%u channels=%u bytes_per_sample=%u sample_rate=%u expected_rate=%u audio_channels=%u expected_channels=%u size=%zu min_bytes=%zu",
				pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
				useDevicePath ? pPvtData->pDevicePath : (pPvtData->pShmName ? pPvtData->pShmName : "(null)"),
				pShm->magic,
				pShm->version,
				pShm->channel_count,
				pShm->bytes_per_sample,
				pShm->sample_rate,
				(U32)pPvtData->audioRate,
				(U32)pPvtData->audioChannels,
				(U32)AVB32_DIRECT_STREAM_CHANNELS,
				mapBytes,
				minBytes);
		return FALSE;
	}

	pPvtData->pGroup->shmFd = fd;
	pPvtData->pGroup->shmBytes = mapBytes;
	pPvtData->pGroup->pShm = pShm;
	pPvtData->shmFd = -1;
	pPvtData->shmBytes = mapBytes;
	pPvtData->pShm = pShm;
	pPvtData->nextFrameIndex = 0;
	pthread_mutex_unlock(&pPvtData->pGroup->lock);

	AVB_LOGF_WARNING("AVB32 direct attached: uid=%u source=%s ring_frames=%u rate=%u",
			pPvtData->streamIndexSet ? pPvtData->streamIndex : 0,
			useDevicePath ? pPvtData->pDevicePath : pPvtData->pShmName,
			pShm->ring_frames,
			pShm->sample_rate);
	return TRUE;
}

static bool xCopyStreamSlice(const pvt_data_t *pPvtData,
			     const openavb_avb32_direct_shm_t *pShm,
			     U64 startFrame,
			     U32 framesPerItem,
			     U8 *pDst,
			     size_t dstBytes)
{
	U32 frameBytes = (U32)openavbAvb32DirectFrameBytes();
	U32 streamBytesPerFrame = AVB32_DIRECT_STREAM_CHANNELS * OPENAVB_AVB32_DIRECT_BYTES_PER_SAMPLE;
	U32 streamByteOffset = pPvtData->streamIndex * streamBytesPerFrame;
	U32 f;

	if (pPvtData && pPvtData->constantSignalEnable) {
		U32 const sampleCount = framesPerItem * AVB32_DIRECT_STREAM_CHANNELS;
		U32 encoded = (U32)pPvtData->constantSignalLevel;
		U32 i;

		if (!pDst || dstBytes < ((size_t)sampleCount * sizeof(U32))) {
			return FALSE;
		}

		if (pPvtData->audioEndian == AVB_AUDIO_ENDIAN_BIG) {
			encoded = htobe32(encoded);
		}
		else {
			encoded = htole32(encoded);
		}

		for (i = 0; i < sampleCount; i++) {
			memcpy(pDst + ((size_t)i * sizeof(U32)), &encoded, sizeof(U32));
		}
		return TRUE;
	}

	if (!pShm || pShm->ring_frames == 0) {
		AVB_LOGF_ERROR("AVB32 direct: invalid ring_frames during copy: stream=%u ring_frames=%u start=%" PRIu64 " frames_per_item=%u",
				pPvtData ? pPvtData->streamIndex : 0,
				pShm ? pShm->ring_frames : 0,
				startFrame,
				framesPerItem);
		return FALSE;
	}

	if (!pDst || dstBytes < ((size_t)framesPerItem * streamBytesPerFrame)) {
		return FALSE;
	}

	for (f = 0; f < framesPerItem; f++) {
		U64 absFrame = startFrame + (U64)f;
		U32 ringFrame = (U32)(absFrame % pShm->ring_frames);
		const U8 *pSrc = pShm->audio_data + ((size_t)ringFrame * frameBytes) + streamByteOffset;
		memcpy(pDst + ((size_t)f * streamBytesPerFrame), pSrc, streamBytesPerFrame);
	}
	return TRUE;
}

static U32 xTrimPendingQueue(media_q_t *pMediaQ)
{
	U32 dropped = 0;

	if (!pMediaQ) {
		return 0;
	}

	while (TRUE) {
		media_q_item_t *pTail = openavbMediaQTailLock(pMediaQ, TRUE);
		if (!pTail) {
			break;
		}
		if (!openavbMediaQTailPull(pMediaQ)) {
			openavbMediaQTailUnlock(pMediaQ);
			break;
		}
		dropped++;
	}

	return dropped;
}

void openavbIntfAvb32DirectCfgCB(media_q_t *pMediaQ, const char *name, const char *value)
{
	char *pEnd;
	long tmp;
	pvt_data_t *pPvtData;

	if (!pMediaQ || !name || !value) {
		return;
	}

	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	if (!pPvtData) {
		return;
	}

	if (strcmp(name, "intf_nv_ignore_timestamp") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0') {
			pPvtData->ignoreTimestamp = (tmp != 0);
		}
	}
	else if (strcmp(name, "intf_nv_shm_name") == 0) {
		free(pPvtData->pShmName);
		pPvtData->pShmName = strdup(value);
	}
	else if (strcmp(name, "intf_nv_device_path") == 0) {
		free(pPvtData->pDevicePath);
		pPvtData->pDevicePath = strdup(value);
	}
	else if (strcmp(name, "intf_nv_stream_index") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= 0 && tmp < 4) {
			pPvtData->streamIndex = (U32)tmp;
			pPvtData->streamIndexSet = TRUE;
		}
	}
	else if (strcmp(name, "intf_nv_group_mask") == 0) {
		tmp = strtol(value, &pEnd, 0);
		if (*pEnd == '\0' && tmp > 0 && tmp <= 0x0f) {
			pPvtData->groupMask = (U32)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_audio_rate") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= AVB_AUDIO_RATE_8KHZ && tmp <= AVB_AUDIO_RATE_192KHZ) {
			pPvtData->audioRate = (avb_audio_rate_t)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_audio_bit_depth") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= AVB_AUDIO_BIT_DEPTH_1BIT && tmp <= AVB_AUDIO_BIT_DEPTH_64BIT) {
			pPvtData->audioBitDepth = (avb_audio_bit_depth_t)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_audio_type") == 0) {
		if (strncasecmp(value, "float", 5) == 0) {
			pPvtData->audioType = AVB_AUDIO_TYPE_FLOAT;
		}
		else if (strncasecmp(value, "sign", 4) == 0 || strncasecmp(value, "int", 3) == 0) {
			pPvtData->audioType = AVB_AUDIO_TYPE_INT;
		}
		else if (strncasecmp(value, "unsign", 6) == 0 || strncasecmp(value, "uint", 4) == 0) {
			pPvtData->audioType = AVB_AUDIO_TYPE_UINT;
		}
	}
	else if (strcmp(name, "intf_nv_audio_endian") == 0) {
		if (strncasecmp(value, "big", 3) == 0) {
			pPvtData->audioEndian = AVB_AUDIO_ENDIAN_BIG;
		}
		else if (strncasecmp(value, "little", 6) == 0) {
			pPvtData->audioEndian = AVB_AUDIO_ENDIAN_LITTLE;
		}
	}
	else if (strcmp(name, "intf_nv_audio_channels") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= AVB_AUDIO_CHANNELS_1) {
			pPvtData->audioChannels = (avb_audio_channels_t)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_target_latency_frames") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp > 0) {
			pPvtData->targetLatencyFrames = (U32)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_catchup_high_water_frames") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp > 0) {
			pPvtData->catchupHighWaterFrames = (U32)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_source_presentation_offset_usec") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= 0) {
			pPvtData->sourcePresentationOffsetUsec = (U32)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_constant_signal_enable") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0') {
			pPvtData->constantSignalEnable = (tmp != 0);
		}
	}
	else if (strcmp(name, "intf_nv_constant_signal_level") == 0) {
		tmp = strtol(value, &pEnd, 0);
		if (*pEnd == '\0') {
			pPvtData->constantSignalLevel = (S32)tmp;
		}
	}

	xSyncAudioInfo(pMediaQ, pPvtData);
}

void openavbIntfAvb32DirectGenInitCB(media_q_t *pMediaQ)
{
	if (pMediaQ) {
		xSyncAudioInfo(pMediaQ, (const pvt_data_t *)pMediaQ->pPvtIntfInfo);
	}
}

void openavbIntfAvb32DirectTxInitCB(media_q_t *pMediaQ)
{
	pvt_data_t *pPvtData;

	if (!pMediaQ || !pMediaQ->pPvtIntfInfo) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	xSyncAudioInfo(pMediaQ, pPvtData);
	openavbMediaQThreadSafeOn(pMediaQ);
	xOpenSharedMemory(pMediaQ);
}

bool openavbIntfAvb32DirectTxCB(media_q_t *pMediaQ)
{
	media_q_item_t *pItem;
	media_q_pub_map_uncmp_audio_info_t *pInfo;
	pvt_data_t *pPvtData;
	openavb_avb32_direct_shm_t *pShm;
	U32 framesPerItem;
	U64 committedFrames;
	U64 presentedFrames;
	U64 frameIndex;
	U64 tsNs;
	size_t itemBytes;
	U32 streamMask;
	direct_group_t *pGroup;

	if (!pMediaQ || !pMediaQ->pPvtIntfInfo) {
		return FALSE;
	}

	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pInfo = xGetPubMapAudioInfo(pMediaQ);
	if (!pInfo || !pPvtData->streamIndexSet) {
		return FALSE;
	}
	if (!pPvtData->pShm && !xOpenSharedMemory(pMediaQ)) {
		return FALSE;
	}
	pShm = pPvtData->pShm;
	if (!pShm || pShm->frame0_walltime_ns == 0) {
		return FALSE;
	}

	framesPerItem = pInfo->framesPerItem;
	if (framesPerItem == 0) {
		return FALSE;
	}

	if (!xRegisterGroup(pPvtData) || !pPvtData->pGroup) {
		return FALSE;
	}

	pGroup = pPvtData->pGroup;
	streamMask = (1u << pPvtData->streamIndex);
	frameIndex = 0;

	pthread_mutex_lock(&pGroup->lock);
	if (!xPrepareCycleLocked(pMediaQ,
				 pPvtData,
				 pInfo,
				 pGroup,
				 pShm,
				 FALSE,
				 &frameIndex,
				 &tsNs,
				 &presentedFrames,
				 &committedFrames)) {
		pPvtData->clockDiagCount++;
		pthread_mutex_unlock(&pGroup->lock);
		return FALSE;
	}

	if (!(pGroup->pendingMask & streamMask)) {
		pthread_mutex_unlock(&pGroup->lock);
		return FALSE;
	}

	if (committedFrames < (frameIndex + (U64)framesPerItem)) {
		pthread_mutex_unlock(&pGroup->lock);
		pPvtData->underrunCount++;
		return FALSE;
	}

	{
		U32 dropped = xTrimPendingQueue(pMediaQ);
		if (dropped > 0) {
			pPvtData->queueTrimCount++;
			pPvtData->queueTrimItems += dropped;
			if (pPvtData->queueTrimCount <= 16 || ((pPvtData->queueTrimCount % 1024ULL) == 0ULL)) {
				AVB_LOGF_WARNING(
					"AVB32 direct queue trim: stream=%u dropped=%u trims=%" PRIu64 " items=%" PRIu64 " frame=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64 " pending_mask=0x%x",
					pPvtData->streamIndex,
					dropped,
					pPvtData->queueTrimCount,
					pPvtData->queueTrimItems,
					frameIndex,
					presentedFrames,
					committedFrames,
					pGroup->pendingMask);
			}
		}
	}

	pItem = openavbMediaQHeadLock(pMediaQ);
	if (!pItem) {
		pthread_mutex_unlock(&pGroup->lock);
		return FALSE;
	}

	itemBytes = (size_t)framesPerItem * AVB32_DIRECT_STREAM_CHANNELS * OPENAVB_AVB32_DIRECT_BYTES_PER_SAMPLE;
	if (!pItem->pPubData || pItem->itemSize < itemBytes) {
		pthread_mutex_unlock(&pGroup->lock);
		openavbMediaQHeadUnlock(pMediaQ);
		return FALSE;
	}

	if (!xCopyStreamSlice(pPvtData, pShm, frameIndex, framesPerItem, pItem->pPubData, pItem->itemSize)) {
		pthread_mutex_unlock(&pGroup->lock);
		openavbMediaQHeadUnlock(pMediaQ);
		return FALSE;
	}

	if (pPvtData->clockDiagCount < 8 || ((pPvtData->clockDiagCount % 8000ULL) == 0)) {
		xLogClockDiag(pPvtData,
			      pShm,
			      presentedFrames,
			      committedFrames,
			      frameIndex,
			      tsNs,
			      "periodic");
	}
	pPvtData->clockDiagCount++;
	pPvtData->nextFrameIndex = frameIndex;
	openavbAvtpTimeSetToTimestampNS(pItem->pAvtpTime, tsNs);
	pItem->dataLen = (U32)itemBytes;
	if (!(pGroup->firstPushLoggedMask & streamMask)) {
		pGroup->firstPushLoggedMask |= streamMask;
		AVB_LOGF_WARNING("AVB32 direct first push: stream=%u frame=%" PRIu64 " ts=%" PRIu64 " presented=%" PRIu64 " committed=%" PRIu64 " pending_mask=0x%x target=%u",
				pPvtData->streamIndex,
				frameIndex,
				tsNs,
				pGroup->cyclePresentedFrames ? pGroup->cyclePresentedFrames : presentedFrames,
				pGroup->cycleCommittedFrames ? pGroup->cycleCommittedFrames : committedFrames,
				pGroup->pendingMask,
				pGroup->targetLatencyFrames);
	}
	pGroup->pendingMask &= ~streamMask;
	pthread_mutex_unlock(&pGroup->lock);
	openavbMediaQHeadPush(pMediaQ);
	return TRUE;
}

void openavbIntfAvb32DirectRxInitCB(media_q_t *pMediaQ)
{
	(void)pMediaQ;
	AVB_LOG_ERROR("AVB32 direct interface is talker-only");
}

bool openavbIntfAvb32DirectRxCB(media_q_t *pMediaQ)
{
	(void)pMediaQ;
	return FALSE;
}

void openavbIntfAvb32DirectEndCB(media_q_t *pMediaQ)
{
	if (pMediaQ && pMediaQ->pPvtIntfInfo) {
		xCloseSharedMemory((pvt_data_t *)pMediaQ->pPvtIntfInfo);
	}
}

void openavbIntfAvb32DirectGenEndCB(media_q_t *pMediaQ)
{
	pvt_data_t *pPvtData;
	if (!pMediaQ || !pMediaQ->pPvtIntfInfo) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	xCloseSharedMemory(pPvtData);
	free(pPvtData->pShmName);
	pPvtData->pShmName = NULL;
	free(pPvtData->pDevicePath);
	pPvtData->pDevicePath = NULL;
}

extern DLL_EXPORT bool openavbIntfAvb32DirectInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB)
{
	pvt_data_t *pPvtData;
	media_q_pub_map_uncmp_audio_info_t *pInfo;

	if (!pMediaQ || !pIntfCB) {
		return FALSE;
	}

	pMediaQ->pPvtIntfInfo = calloc(1, sizeof(pvt_data_t));
	if (!pMediaQ->pPvtIntfInfo) {
		return FALSE;
	}

	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	if (!pInfo) {
		return FALSE;
	}

	pIntfCB->intf_cfg_cb = openavbIntfAvb32DirectCfgCB;
	pIntfCB->intf_gen_init_cb = openavbIntfAvb32DirectGenInitCB;
	pIntfCB->intf_tx_init_cb = openavbIntfAvb32DirectTxInitCB;
	pIntfCB->intf_tx_cb = openavbIntfAvb32DirectTxCB;
	pIntfCB->intf_rx_init_cb = openavbIntfAvb32DirectRxInitCB;
	pIntfCB->intf_rx_cb = openavbIntfAvb32DirectRxCB;
	pIntfCB->intf_end_cb = openavbIntfAvb32DirectEndCB;
	pIntfCB->intf_gen_end_cb = openavbIntfAvb32DirectGenEndCB;
	pIntfCB->intf_get_tx_start_cycle_cb = openavbIntfAvb32DirectGetTxStartCycleCB;
	pIntfCB->intf_prepare_tx_cycle_cb = openavbIntfAvb32DirectPrepareTxCycleCB;

	pPvtData->pShmName = strdup(AVB32_DIRECT_DEFAULT_SHM_NAME);
	pPvtData->audioRate = AVB_AUDIO_RATE_96KHZ;
	pPvtData->audioType = AVB_AUDIO_TYPE_INT;
	pPvtData->audioBitDepth = AVB_AUDIO_BIT_DEPTH_32BIT;
	pPvtData->audioEndian = AVB_AUDIO_ENDIAN_LITTLE;
	pPvtData->audioChannels = AVB32_DIRECT_STREAM_CHANNELS;
	pPvtData->groupMask = 0x0f;
	pPvtData->targetLatencyFrames = AVB32_DIRECT_DEFAULT_TARGET_LATENCY_FRAMES;
	pPvtData->catchupHighWaterFrames = AVB32_DIRECT_DEFAULT_HIGH_WATER_FRAMES;
	pPvtData->sourcePresentationOffsetUsec = 0;
	pPvtData->constantSignalEnable = FALSE;
	pPvtData->constantSignalLevel = 0;
	pPvtData->shmFd = -1;

	pInfo->audioRate = pPvtData->audioRate;
	pInfo->audioType = pPvtData->audioType;
	pInfo->audioBitDepth = pPvtData->audioBitDepth;
	pInfo->audioEndian = pPvtData->audioEndian;
	pInfo->audioChannels = pPvtData->audioChannels;

	return TRUE;
}
