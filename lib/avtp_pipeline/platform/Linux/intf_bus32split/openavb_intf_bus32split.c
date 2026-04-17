/*************************************************************************************************************
Copyright (c) 2026
All rights reserved.
*************************************************************************************************************/

/*
 * MODULE SUMMARY : Shared 32ch ALSA capture, split into 4x 8ch talker streams.
 *
 * This interface is talker-only and intended for a single 32-channel capture source
 * (for example: bus32_cap_hw) fan-out into four AAF/uncmp audio AVB streams.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>

#include "openavb_platform_pub.h"
#include "openavb_osal_pub.h"
#include "openavb_types_pub.h"
#include "openavb_audio_pub.h"
#include "openavb_trace_pub.h"
#include "openavb_mediaq_pub.h"
#include "openavb_map_uncmp_audio_pub.h"
#include "openavb_map_aaf_audio_pub.h"
#include "openavb_intf_pub.h"
#include "openavb_mcs.h"

#define AVB_LOG_COMPONENT "BUS32 Split Interface"
#include "openavb_log_pub.h"

#include <alsa/asoundlib.h>

#define BUS32_CAPTURE_CHANNELS            32
#define BUS32_STREAM_CHANNELS             8
#define BUS32_MAX_STREAMS                 4
#define BUS32_RING_BYTES_DEFAULT          (2 * 1024 * 1024)
#define BUS32_PCM_DEVICE_DEFAULT          "bus32_cap_hw"
#define BUS32_PERIOD_TIME_USEC_DEFAULT    2000
#define BUS32_FIXED_TS_RUNTIME_LEAD_USEC_DEFAULT 1000
#define BUS32_TX_STARTUP_GOVERN_PACKETS    8000U
#define BUS32_TX_QUEUE_AGE_HIGH_NS         4000000ULL
#define BUS32_TX_QUEUE_AGE_TARGET_NS       2000000ULL

#define BUS32_PCM_ACCESS_TYPE             SND_PCM_ACCESS_RW_INTERLEAVED

/* ---- Ring buffer ---- */
typedef struct {
	U8 *data;
	size_t size;
	size_t rd;
	size_t wr;
	size_t fill;
	U64 drops;
} bus32_ring_t;

#define BUS32_META_RING_DEPTH            1024

typedef struct {
	size_t bytes;
	U64 captureStartNs;
} bus32_meta_entry_t;

typedef struct {
	bus32_meta_entry_t entry[BUS32_META_RING_DEPTH];
	U32 rd;
	U32 wr;
	U32 fill;
} bus32_meta_ring_t;

typedef struct {
	bool captureTimeValid;
	U64 captureStartNs;
} bus32_item_meta_t;

static bool ringInit(bus32_ring_t *r, size_t size)
{
	if (!r) {
		return FALSE;
	}
	if (r->data) {
		return TRUE;
	}
	r->data = (U8 *)malloc(size);
	if (!r->data) {
		return FALSE;
	}
	r->size = size;
	r->rd = 0;
	r->wr = 0;
	r->fill = 0;
	r->drops = 0;
	return TRUE;
}

static void ringReset(bus32_ring_t *r)
{
	if (!r) {
		return;
	}
	r->rd = 0;
	r->wr = 0;
	r->fill = 0;
	r->drops = 0;
}

static void metaRingReset(bus32_meta_ring_t *r)
{
	if (!r) {
		return;
	}
	r->rd = 0;
	r->wr = 0;
	r->fill = 0;
}

static U64 bytesToDurationNs(size_t bytes, U32 bytesPerFrame, U32 sampleRate)
{
	U64 frames;
	if (bytesPerFrame == 0 || sampleRate == 0) {
		return 0;
	}
	frames = bytes / bytesPerFrame;
	return (frames * NANOSECONDS_PER_SECOND) / sampleRate;
}

static U64 framesToDurationNs(U64 frames, U32 sampleRate)
{
	if (sampleRate == 0) {
		return 0;
	}
	return (frames * NANOSECONDS_PER_SECOND) / sampleRate;
}

static U64 timespecToNs(const struct timespec *ts)
{
	if (!ts) {
		return 0;
	}
	return ((U64)ts->tv_sec * (U64)NANOSECONDS_PER_SECOND) + (U64)ts->tv_nsec;
}

static bool monotonicNsToWalltimeNs(U64 monoNs, U64 *pWallNs)
{
	U64 monoNowNs = 0;
	U64 wallNowNs = 0;

	if (!pWallNs) {
		return FALSE;
	}
	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_MONOTONIC, &monoNowNs)) {
		return FALSE;
	}
	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &wallNowNs)) {
		return FALSE;
	}

	if (monoNs >= monoNowNs) {
		*pWallNs = wallNowNs + (monoNs - monoNowNs);
	}
	else {
		U64 deltaNs = monoNowNs - monoNs;
		*pWallNs = (wallNowNs > deltaNs) ? (wallNowNs - deltaNs) : 0;
	}
	return TRUE;
}

/* ---- Stream private data ---- */
typedef struct {
	bool ignoreTimestamp;
	char *pDeviceName;

	avb_audio_rate_t audioRate;
	avb_audio_type_t audioType;
	avb_audio_bit_depth_t audioBitDepth;
	avb_audio_endian_t audioEndian;
	avb_audio_channels_t audioChannels;

	bool allowResampling;
	U32 periodTimeUsec;
	S32 clockSkewPPB;
	U32 streamIndex;
	bool streamIndexSet;

	U32 intervalCounter;
	bool fixedTimestampEnabled;
	mcs_t mcs;
	U16 streamUID;
	U32 fixedTsRuntimeLeadUsec;
	bool itemMetaAllocated;
	U32 txCaptureDiagEveryPackets;
	U64 txCaptureDiagPacketCounter;
	U64 txCaptureDiagLogCount;
	U64 txCaptureStaleCount;
	U64 txAsyncProducedCount;
	U64 txAsyncOverwriteCount;
	U64 txAsyncDropCount;
	bool txCatchupFlushPending;
	U64 txCatchupFlushCount;
	U32 txStartupGovernorPacketsRemaining;
	U64 txStartupTrimCount;
	U64 txStartupTrimBytes;

	bool registered;
} pvt_data_t;

/* ---- Shared capture manager ---- */
typedef struct {
	bool active;
	pvt_data_t *owner;
	media_q_t *pMediaQ;
	bus32_ring_t ring;
	bus32_meta_ring_t meta;
} bus32_slot_t;

typedef struct {
	bool initDone;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	bool threadRunning;
	bool stopThread;
	pthread_t thread;

	snd_pcm_t *pcmHandle;
	char *deviceName;
	U32 sampleRate;
	snd_pcm_format_t sampleFormat;
	U32 bytesPerSample;
	bool allowResampling;
	U32 periodTimeUsec;
	U32 framesPerRead;
	U32 negotiatedPeriodUsec;
	U32 negotiatedBufferUsec;
	U64 negotiatedPeriodFrames;
	U64 negotiatedBufferFrames;
	U64 swStartThresholdFrames;
	U64 swAvailMinFrames;
	S64 lastPcmDelayFrames;
	U64 lastPcmDelayNs;
	S64 lastPcmAvailFrames;
	U64 lastPcmSnapshotNs;

	/* Shared fixed-timestamp seed so all split streams start from a common epoch. */
	bool fixedTsSeedValid;
	mcs_t fixedTsSeed;
	U64 fixedTsNsPerAdvance;
	S32 fixedTsCorrectionAmount;
	U32 fixedTsCorrectionInterval;

	U32 activeStreamCount;
	bus32_slot_t slot[BUS32_MAX_STREAMS];
} bus32_mgr_t;

static bus32_mgr_t g_mgr = {0};

static bool deriveCaptureStartNsFromAlsa(U32 framesRead, U32 sampleRate, U64 *pCaptureStartNs)
{
	snd_pcm_uframes_t availFrames = 0;
	snd_htimestamp_t htstamp = {0};
	U64 htMonoNs = 0;
	U64 captureStartMonoNs = 0;
	U64 totalFramesNs = 0;
	int rslt;

	if (!g_mgr.pcmHandle || !pCaptureStartNs || framesRead == 0 || sampleRate == 0) {
		return FALSE;
	}

	rslt = snd_pcm_htimestamp(g_mgr.pcmHandle, &availFrames, &htstamp);
	if (rslt < 0) {
		return FALSE;
	}

	htMonoNs = timespecToNs((const struct timespec *)&htstamp);
	totalFramesNs = framesToDurationNs((U64)availFrames + (U64)framesRead, sampleRate);
	captureStartMonoNs = (htMonoNs > totalFramesNs) ? (htMonoNs - totalFramesNs) : 0;

	g_mgr.lastPcmAvailFrames = (S64)availFrames;
	g_mgr.lastPcmSnapshotNs = htMonoNs;

	return monotonicNsToWalltimeNs(captureStartMonoNs, pCaptureStartNs);
}

static bool mgrInitIfNeeded(void)
{
	int i;
	if (g_mgr.initDone) {
		return TRUE;
	}
	if (pthread_mutex_init(&g_mgr.lock, NULL) != 0) {
		AVB_LOG_ERROR("Failed to initialize BUS32 split mutex");
		return FALSE;
	}
	if (pthread_cond_init(&g_mgr.cond, NULL) != 0) {
		AVB_LOG_ERROR("Failed to initialize BUS32 split cond");
		pthread_mutex_destroy(&g_mgr.lock);
		return FALSE;
	}
	for (i = 0; i < BUS32_MAX_STREAMS; i++) {
		g_mgr.slot[i].active = FALSE;
		g_mgr.slot[i].owner = NULL;
		ringInit(&g_mgr.slot[i].ring, BUS32_RING_BYTES_DEFAULT);
		metaRingReset(&g_mgr.slot[i].meta);
	}
	g_mgr.initDone = TRUE;
	return TRUE;
}

static void logCapturePcmNegotiatedLocked(void)
{
	snd_pcm_hw_params_t *hwCurrent = NULL;
	snd_pcm_sw_params_t *swCurrent = NULL;
	snd_pcm_uframes_t bufferFrames = 0;
	snd_pcm_uframes_t periodFrames = 0;
	snd_pcm_uframes_t startThreshold = 0;
	snd_pcm_uframes_t availMin = 0;
	unsigned int periodUsec = 0;
	unsigned int bufferUsec = 0;
	int dir = 0;

	if (!g_mgr.pcmHandle) {
		return;
	}

	if (snd_pcm_get_params(g_mgr.pcmHandle, &bufferFrames, &periodFrames) == 0) {
		g_mgr.negotiatedBufferFrames = (U64)bufferFrames;
		g_mgr.negotiatedPeriodFrames = (U64)periodFrames;
	}

	if (snd_pcm_hw_params_malloc(&hwCurrent) == 0) {
		if (snd_pcm_hw_params_current(g_mgr.pcmHandle, hwCurrent) == 0) {
			dir = 0;
			if (snd_pcm_hw_params_get_period_time(hwCurrent, &periodUsec, &dir) == 0) {
				g_mgr.negotiatedPeriodUsec = periodUsec;
			}
			dir = 0;
			if (snd_pcm_hw_params_get_buffer_time(hwCurrent, &bufferUsec, &dir) == 0) {
				g_mgr.negotiatedBufferUsec = bufferUsec;
			}
		}
		snd_pcm_hw_params_free(hwCurrent);
	}

	if (snd_pcm_sw_params_malloc(&swCurrent) == 0) {
		if (snd_pcm_sw_params_current(g_mgr.pcmHandle, swCurrent) == 0) {
			if (snd_pcm_sw_params_get_start_threshold(swCurrent, &startThreshold) == 0) {
				g_mgr.swStartThresholdFrames = (U64)startThreshold;
			}
			if (snd_pcm_sw_params_get_avail_min(swCurrent, &availMin) == 0) {
				g_mgr.swAvailMinFrames = (U64)availMin;
			}
		}
		snd_pcm_sw_params_free(swCurrent);
	}

	AVB_LOGF_INFO(
		"BUS32 split capture params: dev=%s rate=%u period=%lluf/%uus buffer=%lluf/%uus start_threshold=%lluf avail_min=%lluf framesPerRead=%u",
		g_mgr.deviceName ? g_mgr.deviceName : "(null)",
		g_mgr.sampleRate,
		(unsigned long long)g_mgr.negotiatedPeriodFrames,
		g_mgr.negotiatedPeriodUsec,
		(unsigned long long)g_mgr.negotiatedBufferFrames,
		g_mgr.negotiatedBufferUsec,
		(unsigned long long)g_mgr.swStartThresholdFrames,
		(unsigned long long)g_mgr.swAvailMinFrames,
		g_mgr.framesPerRead);
}

static snd_pcm_format_t avbToAlsaFormat(avb_audio_type_t type,
							avb_audio_bit_depth_t bitDepth,
							avb_audio_endian_t endian,
							const char *pMediaQDataFormat)
{
	bool tight = FALSE;
	if (bitDepth == AVB_AUDIO_BIT_DEPTH_24BIT) {
		if (pMediaQDataFormat != NULL
			&& (strcmp(pMediaQDataFormat, MapAVTPAudioMediaQDataFormat) == 0
			|| strcmp(pMediaQDataFormat, MapUncmpAudioMediaQDataFormat) == 0)) {
			tight = TRUE;
		}
	}

	if (type == AVB_AUDIO_TYPE_FLOAT) {
		if (bitDepth == AVB_AUDIO_BIT_DEPTH_32BIT) {
			return (endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_FLOAT_BE : SND_PCM_FORMAT_FLOAT_LE;
		}
	}
	else if (type == AVB_AUDIO_TYPE_UINT) {
		switch (bitDepth) {
			case AVB_AUDIO_BIT_DEPTH_8BIT: return SND_PCM_FORMAT_U8;
			case AVB_AUDIO_BIT_DEPTH_16BIT: return (endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_U16_BE : SND_PCM_FORMAT_U16_LE;
			case AVB_AUDIO_BIT_DEPTH_24BIT: return tight ? ((endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_U24_3BE : SND_PCM_FORMAT_U24_3LE)
											: ((endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_U24_BE : SND_PCM_FORMAT_U24_LE);
			case AVB_AUDIO_BIT_DEPTH_32BIT: return (endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_U32_BE : SND_PCM_FORMAT_U32_LE;
			default: break;
		}
	}
	else {
		/* AVB_AUDIO_TYPE_INT or unspecified defaults to signed integer. */
		switch (bitDepth) {
			case AVB_AUDIO_BIT_DEPTH_8BIT:
				return (type == AVB_AUDIO_TYPE_INT) ? SND_PCM_FORMAT_S8 : SND_PCM_FORMAT_U8;
			case AVB_AUDIO_BIT_DEPTH_16BIT: return (endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_S16_BE : SND_PCM_FORMAT_S16_LE;
			case AVB_AUDIO_BIT_DEPTH_24BIT: return tight ? ((endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_S24_3BE : SND_PCM_FORMAT_S24_3LE)
											: ((endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_S24_BE : SND_PCM_FORMAT_S24_LE);
			case AVB_AUDIO_BIT_DEPTH_32BIT: return (endian == AVB_AUDIO_ENDIAN_BIG) ? SND_PCM_FORMAT_S32_BE : SND_PCM_FORMAT_S32_LE;
			default: break;
		}
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

static U32 alsaFormatBytesPerSample(snd_pcm_format_t fmt)
{
	U32 bits = snd_pcm_format_physical_width(fmt);
	if (bits == 0) {
		return 0;
	}
	return bits / 8;
}

static bool calcFixedTsParams(const pvt_data_t *pPvtData,
						 const media_q_pub_map_uncmp_audio_info_t *pInfo,
						 S32 skewEst,
						 U64 *pNsPerAdvance,
						 S32 *pCorrectionAmount,
						 U32 *pCorrectionInterval)
{
	S64 base;
	S64 per;
	U32 rate;
	S64 rem;
	if (!pPvtData || !pInfo || !pNsPerAdvance || !pCorrectionAmount || !pCorrectionInterval) {
		return FALSE;
	}
	if (pPvtData->audioRate == 0) {
		return FALSE;
	}

	base = (S64)MICROSECONDS_PER_SECOND * (S64)pInfo->framesPerItem * 10;
	per = base + (S64)(skewEst / 10);
	rate = pPvtData->audioRate / 100;
	if (rate == 0 || per <= 0) {
		return FALSE;
	}

	*pNsPerAdvance = (U64)(per / (S64)rate);
	rem = per % (S64)rate;
	if (rem != 0) {
		rem *= 10;
		rem /= (S64)rate;
	}
	*pCorrectionAmount = (S32)rem;
	*pCorrectionInterval = 10;
	return TRUE;
}

static bool fixedTsSeedMatchesLocked(U64 nsPerAdvance, S32 correctionAmount, U32 correctionInterval)
{
	if (!g_mgr.fixedTsSeedValid) {
		return FALSE;
	}
	return (g_mgr.fixedTsNsPerAdvance == nsPerAdvance
		&& g_mgr.fixedTsCorrectionAmount == correctionAmount
		&& g_mgr.fixedTsCorrectionInterval == correctionInterval);
}

static bool ensureSharedFixedTsSeedLocked(const pvt_data_t *pPvtData,
							 const media_q_pub_map_uncmp_audio_info_t *pInfo)
{
	U64 nsPerAdvance = 0;
	S32 correctionAmount = 0;
	U32 correctionInterval = 0;

	if (!calcFixedTsParams(pPvtData, pInfo, pPvtData->clockSkewPPB,
					   &nsPerAdvance, &correctionAmount, &correctionInterval)) {
		return FALSE;
	}

	if (!fixedTsSeedMatchesLocked(nsPerAdvance, correctionAmount, correctionInterval)) {
		openavbMcsInit(&g_mgr.fixedTsSeed, nsPerAdvance, correctionAmount, correctionInterval);
		/* Seed once so every stream copies an identical, already-established epoch. */
		openavbMcsAdvance(&g_mgr.fixedTsSeed);
		if (!g_mgr.fixedTsSeed.firstTimeSet) {
			AVB_LOG_ERROR("BUS32 split: failed to seed shared fixed timestamp epoch");
			g_mgr.fixedTsSeedValid = FALSE;
			return FALSE;
		}
		g_mgr.fixedTsNsPerAdvance = nsPerAdvance;
		g_mgr.fixedTsCorrectionAmount = correctionAmount;
		g_mgr.fixedTsCorrectionInterval = correctionInterval;
		g_mgr.fixedTsSeedValid = TRUE;
		AVB_LOGF_INFO("BUS32 split shared fixed TS seed: edge=%" PRIu64 " ns=%" PRIu64 " corr=%d/%u",
				  g_mgr.fixedTsSeed.edgeTime,
				  g_mgr.fixedTsNsPerAdvance,
				  g_mgr.fixedTsCorrectionAmount,
				  g_mgr.fixedTsCorrectionInterval);
	}

	return TRUE;
}

static void advanceMcsToWallTime(mcs_t *pMcs, U64 targetTimeNs)
{
	if (!pMcs || !pMcs->firstTimeSet || pMcs->nsPerAdvance == 0) {
		return;
	}
	while (pMcs->edgeTime <= targetTimeNs) {
		pMcs->edgeTime += pMcs->nsPerAdvance;
		pMcs->tickCount++;
		if (pMcs->correctionInterval > 0 &&
			(pMcs->tickCount % pMcs->correctionInterval) == 0) {
			pMcs->edgeTime += pMcs->correctionAmount;
		}
	}
}

static bool cloneSharedFixedTsToStreamLocked(pvt_data_t *pPvtData)
{
	U64 nowNs = 0;
	U64 targetNs = 0;
	if (!g_mgr.fixedTsSeedValid) {
		return FALSE;
	}
	CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
	targetNs = nowNs + g_mgr.fixedTsSeed.nsPerAdvance;
	/* Keep shared phase, but never hand a stream an already-stale edge time. */
	advanceMcsToWallTime(&g_mgr.fixedTsSeed, targetNs);
	pPvtData->mcs = g_mgr.fixedTsSeed;
	return TRUE;
}

static void setTxItemTimestamp(pvt_data_t *pPvtData, media_q_item_t *pItem)
{
	bus32_item_meta_t *pItemMeta = NULL;

	if (!pPvtData || !pItem) {
		return;
	}

	pItemMeta = (bus32_item_meta_t *)pItem->pPvtIntfData;
	if (pItemMeta && pItemMeta->captureTimeValid) {
		openavbAvtpTimeSetToTimestampNS(pItem->pAvtpTime, pItemMeta->captureStartNs);
		return;
	}

	if (!pPvtData->fixedTimestampEnabled) {
		openavbAvtpTimeSetToWallTime(pItem->pAvtpTime);
		return;
	}

	if (pPvtData->mcs.firstTimeSet && pPvtData->mcs.nsPerAdvance > 0) {
		U64 nowNs = 0;
		U64 targetNs = 0;
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
		/*
		 * Keep the shared edge just ahead of wall time plus any explicitly
		 * configured runtime lead. The map layer already adds packet/transit
		 * offsets, so forcing an extra full item quantum here biases every
		 * split stream early together.
		 */
		targetNs = nowNs + ((U64)pPvtData->fixedTsRuntimeLeadUsec * NANOSECONDS_PER_USEC);
		advanceMcsToWallTime(&pPvtData->mcs, targetNs);
	}
	else {
		openavbMcsAdvance(&pPvtData->mcs);
	}

	openavbAvtpTimeSetToTimestampNS(pItem->pAvtpTime, pPvtData->mcs.edgeTime);
}

static void slotPushDirectToMediaQLocked(bus32_slot_t *pSlot, U32 idx, const U8 *pSrc, size_t len, U64 captureStartNs)
{
	media_q_t *pMediaQ = NULL;
	pvt_data_t *pPvtData = NULL;
	media_q_item_t *pItem = NULL;
	bool droppedOldest = FALSE;
	U64 nowNs = 0;
	S64 captureAgeNs = 0;
	U32 logEveryPackets = 0;

	if (!pSlot || !pSlot->active || !pSlot->owner || !pSlot->pMediaQ || !pSrc || len == 0) {
		return;
	}

	pMediaQ = pSlot->pMediaQ;
	pPvtData = pSlot->owner;

	pItem = openavbMediaQHeadLock(pMediaQ);
	if (!pItem) {
		media_q_item_t *pTail = openavbMediaQTailLock(pMediaQ, TRUE);
		if (pTail) {
			openavbMediaQTailPull(pMediaQ);
			droppedOldest = TRUE;
			pPvtData->txAsyncOverwriteCount++;
			pItem = openavbMediaQHeadLock(pMediaQ);
		}
	}

	if (!pItem) {
		pPvtData->txAsyncDropCount++;
		if (pPvtData->txAsyncDropCount <= 16 ||
				(pPvtData->txAsyncDropCount % 2000ULL) == 0ULL) {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
			captureAgeNs = (nowNs >= captureStartNs)
				? (S64)(nowNs - captureStartNs)
				: -((S64)(captureStartNs - nowNs));
			AVB_LOGF_WARNING(
				"BUS32 direct queue drop: stream=%u idx=%u item_bytes=%zu capture=%llu now=%llu age=%lldns produced=%llu overwrites=%llu drops=%llu",
				pPvtData->streamUID,
				idx,
				len,
				(unsigned long long)captureStartNs,
				(unsigned long long)nowNs,
				(long long)captureAgeNs,
				(unsigned long long)pPvtData->txAsyncProducedCount,
				(unsigned long long)pPvtData->txAsyncOverwriteCount,
				(unsigned long long)pPvtData->txAsyncDropCount);
		}
		return;
	}

	if (!pItem->pPubData || pItem->itemSize < len) {
		openavbMediaQHeadUnlock(pMediaQ);
		pPvtData->txAsyncDropCount++;
		AVB_LOGF_ERROR("BUS32 direct queue item too small: stream=%u idx=%u need=%zu item=%u",
				pPvtData->streamUID, idx, len, pItem->itemSize);
		return;
	}

	pItem->readIdx = 0;
	pItem->dataLen = 0;
	memcpy(pItem->pPubData, pSrc, len);
	pItem->dataLen = (U32)len;

	if (pItem->pPvtIntfData) {
		bus32_item_meta_t *pItemMeta = (bus32_item_meta_t *)pItem->pPvtIntfData;
		pItemMeta->captureTimeValid = TRUE;
		pItemMeta->captureStartNs = captureStartNs;
	}

	setTxItemTimestamp(pPvtData, pItem);
	openavbMediaQHeadPush(pMediaQ);

	pPvtData->txAsyncProducedCount++;
	logEveryPackets = pPvtData->txCaptureDiagEveryPackets ? pPvtData->txCaptureDiagEveryPackets : 8000U;

	if (droppedOldest ||
			(pPvtData->txAsyncProducedCount % logEveryPackets) == 0ULL) {
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
		captureAgeNs = (nowNs >= captureStartNs)
			? (S64)(nowNs - captureStartNs)
			: -((S64)(captureStartNs - nowNs));
		AVB_LOGF_INFO(
			"BUS32 direct queue %s: stream=%u idx=%u item_bytes=%zu capture=%llu now=%llu age=%lldns produced=%llu overwrites=%llu drops=%llu",
			droppedOldest ? "overwrite" : "diag",
			pPvtData->streamUID,
			idx,
			len,
			(unsigned long long)captureStartNs,
			(unsigned long long)nowNs,
			(long long)captureAgeNs,
			(unsigned long long)pPvtData->txAsyncProducedCount,
			(unsigned long long)pPvtData->txAsyncOverwriteCount,
			(unsigned long long)pPvtData->txAsyncDropCount);
	}
}

static bool openCapturePcmLocked(const pvt_data_t *pPvtData,
					 const media_q_pub_map_uncmp_audio_info_t *pInfo,
					 const char *pMediaQDataFormat)
{
	snd_pcm_hw_params_t *hwParams = NULL;
	snd_pcm_sw_params_t *swParams = NULL;
	int rslt;
	U32 rate;
	snd_pcm_format_t fmt;

	if (g_mgr.pcmHandle) {
		return TRUE;
	}
	fmt = avbToAlsaFormat(pPvtData->audioType, pPvtData->audioBitDepth, pPvtData->audioEndian, pMediaQDataFormat);
	if (fmt == SND_PCM_FORMAT_UNKNOWN) {
		AVB_LOG_ERROR("Unsupported ALSA format for BUS32 split");
		return FALSE;
	}

	rslt = snd_pcm_open(&g_mgr.pcmHandle, pPvtData->pDeviceName, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_open(%s) failed: %s", pPvtData->pDeviceName, snd_strerror(rslt));
		g_mgr.pcmHandle = NULL;
		return FALSE;
	}

	rslt = snd_pcm_hw_params_malloc(&hwParams);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_hw_params_malloc failed: %s", snd_strerror(rslt));
		snd_pcm_close(g_mgr.pcmHandle);
		g_mgr.pcmHandle = NULL;
		return FALSE;
	}

	rslt = snd_pcm_hw_params_any(g_mgr.pcmHandle, hwParams);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_hw_params_any failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_hw_params_set_rate_resample(g_mgr.pcmHandle, hwParams, pPvtData->allowResampling ? 1 : 0);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: set_rate_resample failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_hw_params_set_access(g_mgr.pcmHandle, hwParams, BUS32_PCM_ACCESS_TYPE);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: set_access failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_hw_params_set_format(g_mgr.pcmHandle, hwParams, fmt);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: set_format failed: %s", snd_strerror(rslt));
		goto err;
	}

	rate = pPvtData->audioRate;
	rslt = snd_pcm_hw_params_set_rate_near(g_mgr.pcmHandle, hwParams, &rate, 0);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: set_rate_near failed: %s", snd_strerror(rslt));
		goto err;
	}
	if (rate != pPvtData->audioRate) {
		AVB_LOGF_WARNING("BUS32 split: requested rate=%u, actual=%u", pPvtData->audioRate, rate);
	}

	rslt = snd_pcm_hw_params_set_channels(g_mgr.pcmHandle, hwParams, BUS32_CAPTURE_CHANNELS);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: set_channels(%d) failed: %s", BUS32_CAPTURE_CHANNELS, snd_strerror(rslt));
		goto err;
	}

	if (pPvtData->periodTimeUsec > 0) {
		U32 period = pPvtData->periodTimeUsec;
		rslt = snd_pcm_hw_params_set_period_time_near(g_mgr.pcmHandle, hwParams, &period, 0);
		if (rslt < 0) {
			AVB_LOGF_WARNING("BUS32 split: set_period_time_near(%u) failed: %s", pPvtData->periodTimeUsec, snd_strerror(rslt));
		}
	}

	rslt = snd_pcm_hw_params(g_mgr.pcmHandle, hwParams);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_hw_params failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_sw_params_malloc(&swParams);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_sw_params_malloc failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_sw_params_current(g_mgr.pcmHandle, swParams);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_sw_params_current failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_sw_params_set_tstamp_mode(g_mgr.pcmHandle, swParams, SND_PCM_TSTAMP_ENABLE);
	if (rslt < 0) {
		AVB_LOGF_WARNING("BUS32 split: set_tstamp_mode(enable) failed: %s", snd_strerror(rslt));
	}
	rslt = snd_pcm_sw_params_set_tstamp_type(g_mgr.pcmHandle, swParams, SND_PCM_TSTAMP_TYPE_MONOTONIC);
	if (rslt < 0) {
		AVB_LOGF_WARNING("BUS32 split: set_tstamp_type(monotonic) failed: %s", snd_strerror(rslt));
	}

	rslt = snd_pcm_sw_params(g_mgr.pcmHandle, swParams);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_sw_params failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_prepare(g_mgr.pcmHandle);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_prepare failed: %s", snd_strerror(rslt));
		goto err;
	}

	rslt = snd_pcm_start(g_mgr.pcmHandle);
	if (rslt < 0) {
		AVB_LOGF_ERROR("BUS32 split: snd_pcm_start failed: %s", snd_strerror(rslt));
		goto err;
	}

	g_mgr.sampleRate = rate;
	g_mgr.sampleFormat = fmt;
	g_mgr.bytesPerSample = alsaFormatBytesPerSample(fmt);
	g_mgr.allowResampling = pPvtData->allowResampling;
	g_mgr.periodTimeUsec = pPvtData->periodTimeUsec;
	g_mgr.negotiatedPeriodUsec = 0;
	g_mgr.negotiatedBufferUsec = 0;
	g_mgr.negotiatedPeriodFrames = 0;
	g_mgr.negotiatedBufferFrames = 0;
	g_mgr.swStartThresholdFrames = 0;
	g_mgr.swAvailMinFrames = 0;
	g_mgr.lastPcmDelayFrames = 0;
	g_mgr.lastPcmDelayNs = 0;
	g_mgr.lastPcmAvailFrames = 0;
	g_mgr.lastPcmSnapshotNs = 0;
	g_mgr.framesPerRead = pInfo->framesPerItem;
	if (g_mgr.framesPerRead == 0) {
		g_mgr.framesPerRead = 12;
	}
	if (g_mgr.deviceName) {
		free(g_mgr.deviceName);
	}
	g_mgr.deviceName = strdup(pPvtData->pDeviceName);

	snd_pcm_hw_params_free(hwParams);
	snd_pcm_sw_params_free(swParams);
	AVB_LOGF_INFO("BUS32 split capture ready: dev=%s rate=%u fmt=%d bytesPerSample=%u framesPerRead=%u",
			g_mgr.deviceName ? g_mgr.deviceName : "(null)",
			g_mgr.sampleRate,
			g_mgr.sampleFormat,
			g_mgr.bytesPerSample,
			g_mgr.framesPerRead);
	logCapturePcmNegotiatedLocked();
	return TRUE;

err:
	if (hwParams) {
		snd_pcm_hw_params_free(hwParams);
	}
	if (swParams) {
		snd_pcm_sw_params_free(swParams);
	}
	if (g_mgr.pcmHandle) {
		snd_pcm_close(g_mgr.pcmHandle);
		g_mgr.pcmHandle = NULL;
	}
	return FALSE;
}

static void closeCapturePcmLocked(void)
{
	if (g_mgr.pcmHandle) {
		snd_pcm_close(g_mgr.pcmHandle);
		g_mgr.pcmHandle = NULL;
	}
	if (g_mgr.deviceName) {
		free(g_mgr.deviceName);
		g_mgr.deviceName = NULL;
	}
	g_mgr.sampleRate = 0;
	g_mgr.sampleFormat = SND_PCM_FORMAT_UNKNOWN;
	g_mgr.bytesPerSample = 0;
	g_mgr.framesPerRead = 0;
	g_mgr.fixedTsSeedValid = FALSE;
	g_mgr.fixedTsNsPerAdvance = 0;
	g_mgr.fixedTsCorrectionAmount = 0;
	g_mgr.fixedTsCorrectionInterval = 0;
}

static void *captureThreadFn(void *arg)
{
	U8 *captureBuf = NULL;
	U8 *splitBuf[BUS32_MAX_STREAMS] = {0};
	size_t captureBytes;
	size_t splitBytes;
	U32 bytesPerSample;
	U32 bytesPerFrame;
	U32 framesPerRead;
	U32 sampleRate;
	int rslt;
	U32 activeMask;
	U32 frame;
	U32 s;
	U8 *srcFrame;
	U64 captureStartNs;

	(void)arg;

	pthread_mutex_lock(&g_mgr.lock);
	bytesPerSample = g_mgr.bytesPerSample;
	bytesPerFrame = BUS32_STREAM_CHANNELS * bytesPerSample;
	framesPerRead = g_mgr.framesPerRead;
	sampleRate = g_mgr.sampleRate;
	pthread_mutex_unlock(&g_mgr.lock);

	captureBytes = (size_t)framesPerRead * BUS32_CAPTURE_CHANNELS * bytesPerSample;
	splitBytes = (size_t)framesPerRead * BUS32_STREAM_CHANNELS * bytesPerSample;
	captureBuf = (U8 *)malloc(captureBytes);
	if (!captureBuf) {
		AVB_LOG_ERROR("BUS32 split: failed to allocate capture buffer");
		return NULL;
	}
	for (s = 0; s < BUS32_MAX_STREAMS; s++) {
		splitBuf[s] = (U8 *)malloc(splitBytes);
		if (!splitBuf[s]) {
			AVB_LOG_ERROR("BUS32 split: failed to allocate split buffer");
			for (s = 0; s < BUS32_MAX_STREAMS; s++) {
				if (splitBuf[s]) free(splitBuf[s]);
			}
			free(captureBuf);
			return NULL;
		}
	}

	for (;;) {
		pthread_mutex_lock(&g_mgr.lock);
		if (g_mgr.stopThread) {
			pthread_mutex_unlock(&g_mgr.lock);
			break;
		}
		activeMask = 0;
		for (s = 0; s < BUS32_MAX_STREAMS; s++) {
			if (g_mgr.slot[s].active) {
				activeMask |= (1U << s);
			}
		}
		pthread_mutex_unlock(&g_mgr.lock);

		if (activeMask == 0) {
			usleep(1000);
			continue;
		}

		rslt = snd_pcm_readi(g_mgr.pcmHandle, captureBuf, framesPerRead);
		if (rslt < 0) {
			if (rslt == -EAGAIN) {
				usleep(500);
				continue;
			}
			if (rslt == -EPIPE) {
				AVB_LOGF_WARNING("BUS32 split: capture overrun: %s", snd_strerror(rslt));
				rslt = snd_pcm_recover(g_mgr.pcmHandle, rslt, 0);
				if (rslt < 0) {
					AVB_LOGF_ERROR("BUS32 split: snd_pcm_recover failed: %s", snd_strerror(rslt));
					usleep(1000);
				}
				continue;
			}
			AVB_LOGF_ERROR("BUS32 split: snd_pcm_readi failed: %s", snd_strerror(rslt));
			usleep(1000);
			continue;
		}
		if (rslt == 0) {
			usleep(500);
			continue;
		}

		if (!deriveCaptureStartNsFromAlsa((U32)rslt, sampleRate, &captureStartNs)) {
			U64 captureEndNs = 0;
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &captureEndNs);
			captureStartNs = captureEndNs - bytesToDurationNs((size_t)rslt * bytesPerFrame, bytesPerFrame, sampleRate);
			g_mgr.lastPcmSnapshotNs = captureEndNs;
		}

		if (g_mgr.pcmHandle) {
			snd_pcm_sframes_t delayFrames = 0;
			snd_pcm_sframes_t availFrames = 0;

			if (snd_pcm_delay(g_mgr.pcmHandle, &delayFrames) == 0) {
				g_mgr.lastPcmDelayFrames = (S64)delayFrames;
				g_mgr.lastPcmDelayNs = (delayFrames > 0)
					? framesToDurationNs((U64)delayFrames, sampleRate)
					: 0;
			}
			availFrames = snd_pcm_avail_update(g_mgr.pcmHandle);
			if (availFrames >= 0) {
				g_mgr.lastPcmAvailFrames = (S64)availFrames;
			}
		}

		/* Split interleaved 32ch -> 4 x interleaved 8ch */
		for (frame = 0; frame < (U32)rslt; frame++) {
			srcFrame = captureBuf + ((size_t)frame * BUS32_CAPTURE_CHANNELS * bytesPerSample);
			for (s = 0; s < BUS32_MAX_STREAMS; s++) {
				if (activeMask & (1U << s)) {
					memcpy(splitBuf[s] + ((size_t)frame * BUS32_STREAM_CHANNELS * bytesPerSample),
						   srcFrame + ((size_t)s * BUS32_STREAM_CHANNELS * bytesPerSample),
						   (size_t)BUS32_STREAM_CHANNELS * bytesPerSample);
				}
			}
		}

		pthread_mutex_lock(&g_mgr.lock);
		for (s = 0; s < BUS32_MAX_STREAMS; s++) {
			if (g_mgr.slot[s].active) {
				slotPushDirectToMediaQLocked(
					&g_mgr.slot[s],
					s,
					splitBuf[s],
					(size_t)rslt * BUS32_STREAM_CHANNELS * bytesPerSample,
					captureStartNs);
			}
		}
		pthread_mutex_unlock(&g_mgr.lock);
	}

	for (s = 0; s < BUS32_MAX_STREAMS; s++) {
		if (splitBuf[s]) {
			free(splitBuf[s]);
		}
	}
	free(captureBuf);
	return NULL;
}

static bool validateSharedConfigLocked(const pvt_data_t *pPvtData,
						   const media_q_pub_map_uncmp_audio_info_t *pInfo,
						   const char *pMediaQDataFormat)
{
	snd_pcm_format_t fmt;
	if (!g_mgr.pcmHandle) {
		return TRUE;
	}
	if (g_mgr.sampleRate != pPvtData->audioRate) {
		AVB_LOGF_ERROR("BUS32 split: incompatible sample rate. Existing=%u New=%u",
				   g_mgr.sampleRate, pPvtData->audioRate);
		return FALSE;
	}
	fmt = avbToAlsaFormat(pPvtData->audioType, pPvtData->audioBitDepth, pPvtData->audioEndian, pMediaQDataFormat);
	if (fmt != g_mgr.sampleFormat) {
		AVB_LOGF_ERROR("BUS32 split: incompatible sample format. Existing=%d New=%d", g_mgr.sampleFormat, fmt);
		return FALSE;
	}
	if (g_mgr.framesPerRead != pInfo->framesPerItem) {
		AVB_LOGF_ERROR("BUS32 split: framesPerItem mismatch. Existing=%u New=%u",
				   g_mgr.framesPerRead, pInfo->framesPerItem);
		return FALSE;
	}
	if (g_mgr.deviceName && strcmp(g_mgr.deviceName, pPvtData->pDeviceName) != 0) {
		AVB_LOGF_ERROR("BUS32 split: device mismatch. Existing=%s New=%s",
				   g_mgr.deviceName, pPvtData->pDeviceName);
		return FALSE;
	}
	return TRUE;
}

static bool registerStream(media_q_t *pMediaQ, pvt_data_t *pPvtData)
{
	bool ok = FALSE;
	media_q_pub_map_uncmp_audio_info_t *pInfo;
	U32 idx;

	if (!mgrInitIfNeeded()) {
		return FALSE;
	}
	pInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	if (!pInfo) {
		AVB_LOG_ERROR("BUS32 split: public map audio info missing");
		return FALSE;
	}

	if (!pPvtData->streamIndexSet) {
		AVB_LOG_ERROR("BUS32 split: intf_nv_stream_index must be set (0..3)");
		return FALSE;
	}
	if (pPvtData->streamIndex >= BUS32_MAX_STREAMS) {
		AVB_LOGF_ERROR("BUS32 split: invalid stream index %u", pPvtData->streamIndex);
		return FALSE;
	}
	idx = pPvtData->streamIndex;

	if (pInfo->audioChannels != BUS32_STREAM_CHANNELS) {
		AVB_LOGF_ERROR("BUS32 split: stream index %u must be 8-channel, configured=%u", idx, pInfo->audioChannels);
		return FALSE;
	}

	pthread_mutex_lock(&g_mgr.lock);

	if (g_mgr.slot[idx].active && g_mgr.slot[idx].owner != pPvtData) {
		AVB_LOGF_ERROR("BUS32 split: stream index %u already registered", idx);
		goto done;
	}

	if (!ringInit(&g_mgr.slot[idx].ring, BUS32_RING_BYTES_DEFAULT)) {
		AVB_LOG_ERROR("BUS32 split: ring init failed");
		goto done;
	}
	ringReset(&g_mgr.slot[idx].ring);
	metaRingReset(&g_mgr.slot[idx].meta);

	if (!validateSharedConfigLocked(pPvtData, pInfo, pMediaQ->pMediaQDataFormat)) {
		goto done;
	}

	if (!g_mgr.pcmHandle) {
		if (!openCapturePcmLocked(pPvtData, pInfo, pMediaQ->pMediaQDataFormat)) {
			goto done;
		}
	}

	if (!g_mgr.threadRunning) {
		g_mgr.stopThread = FALSE;
		if (pthread_create(&g_mgr.thread, NULL, captureThreadFn, NULL) != 0) {
			AVB_LOG_ERROR("BUS32 split: failed to create capture thread");
			closeCapturePcmLocked();
			goto done;
		}
		g_mgr.threadRunning = TRUE;
	}

	if (!g_mgr.slot[idx].active) {
		g_mgr.slot[idx].active = TRUE;
		g_mgr.slot[idx].owner = pPvtData;
		g_mgr.slot[idx].pMediaQ = pMediaQ;
		g_mgr.activeStreamCount++;
	}
	pPvtData->registered = TRUE;

	AVB_LOGF_INFO("BUS32 split: registered stream uid=%u idx=%u active=%u", pPvtData->streamUID, idx, g_mgr.activeStreamCount);
	ok = TRUE;

done:
	pthread_mutex_unlock(&g_mgr.lock);
	return ok;
}

static void unregisterStream(pvt_data_t *pPvtData)
{
	U32 idx;
	bool joinThread = FALSE;
	pthread_t tid;

	if (!g_mgr.initDone || !pPvtData || !pPvtData->registered || !pPvtData->streamIndexSet) {
		return;
	}
	idx = pPvtData->streamIndex;
	if (idx >= BUS32_MAX_STREAMS) {
		return;
	}

	pthread_mutex_lock(&g_mgr.lock);
	if (g_mgr.slot[idx].active && g_mgr.slot[idx].owner == pPvtData) {
		g_mgr.slot[idx].active = FALSE;
		g_mgr.slot[idx].owner = NULL;
		g_mgr.slot[idx].pMediaQ = NULL;
		ringReset(&g_mgr.slot[idx].ring);
		metaRingReset(&g_mgr.slot[idx].meta);
		if (g_mgr.activeStreamCount > 0) {
			g_mgr.activeStreamCount--;
		}
	}
	pPvtData->registered = FALSE;

	if (g_mgr.activeStreamCount == 0 && g_mgr.threadRunning) {
		g_mgr.stopThread = TRUE;
		pthread_cond_broadcast(&g_mgr.cond);
		joinThread = TRUE;
		tid = g_mgr.thread;
	}
	pthread_mutex_unlock(&g_mgr.lock);

	if (joinThread) {
		pthread_join(tid, NULL);
		pthread_mutex_lock(&g_mgr.lock);
		g_mgr.threadRunning = FALSE;
		g_mgr.stopThread = FALSE;
		closeCapturePcmLocked();
		pthread_mutex_unlock(&g_mgr.lock);
	}

	AVB_LOGF_INFO("BUS32 split: unregistered stream uid=%u idx=%u", pPvtData->streamUID, idx);
}

/* ---- interface callbacks ---- */
void openavbIntfBus32SplitCfgCB(media_q_t *pMediaQ, const char *name, const char *value)
{
	char *pEnd;
	long tmp;
	U32 val;
	media_q_pub_map_uncmp_audio_info_t *pInfo;
	pvt_data_t *pPvtData;

	if (!pMediaQ || !name || !value) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	if (!pPvtData) {
		AVB_LOG_ERROR("BUS32 split: private data missing");
		return;
	}
	pInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	if (!pInfo) {
		AVB_LOG_ERROR("BUS32 split: public map info missing");
		return;
	}

	if (strcmp(name, "intf_nv_device_name") == 0) {
		if (pPvtData->pDeviceName) {
			free(pPvtData->pDeviceName);
		}
		pPvtData->pDeviceName = strdup(value);
	}
	else if (strcmp(name, "intf_nv_stream_index") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= 0 && tmp < BUS32_MAX_STREAMS) {
			pPvtData->streamIndex = (U32)tmp;
			pPvtData->streamIndexSet = TRUE;
		}
	}
	else if (strcmp(name, "intf_nv_audio_rate") == 0) {
		val = strtol(value, &pEnd, 10);
		if (val >= AVB_AUDIO_RATE_8KHZ && val <= AVB_AUDIO_RATE_192KHZ) {
			pPvtData->audioRate = (avb_audio_rate_t)val;
			pInfo->audioRate = pPvtData->audioRate;
		}
	}
	else if (strcmp(name, "intf_nv_audio_type") == 0) {
		if (strncasecmp(value, "float", 5) == 0) {
			pPvtData->audioType = AVB_AUDIO_TYPE_FLOAT;
			pInfo->audioType = pPvtData->audioType;
		}
		else if (strncasecmp(value, "sign", 4) == 0 || strncasecmp(value, "int", 3) == 0) {
			pPvtData->audioType = AVB_AUDIO_TYPE_INT;
			pInfo->audioType = pPvtData->audioType;
		}
		else if (strncasecmp(value, "unsign", 6) == 0 || strncasecmp(value, "uint", 4) == 0) {
			pPvtData->audioType = AVB_AUDIO_TYPE_UINT;
			pInfo->audioType = pPvtData->audioType;
		}
		else {
			val = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && val <= AVB_AUDIO_TYPE_FLOAT) {
				pPvtData->audioType = (avb_audio_type_t)val;
				pInfo->audioType = pPvtData->audioType;
			}
		}
	}
	else if (strcmp(name, "intf_nv_audio_bit_depth") == 0) {
		val = strtol(value, &pEnd, 10);
		if (val >= AVB_AUDIO_BIT_DEPTH_1BIT && val <= AVB_AUDIO_BIT_DEPTH_64BIT) {
			pPvtData->audioBitDepth = (avb_audio_bit_depth_t)val;
			pInfo->audioBitDepth = pPvtData->audioBitDepth;
		}
	}
	else if (strcmp(name, "intf_nv_audio_endian") == 0) {
		if (strncasecmp(value, "big", 3) == 0) {
			pPvtData->audioEndian = AVB_AUDIO_ENDIAN_BIG;
			pInfo->audioEndian = pPvtData->audioEndian;
		}
		else if (strncasecmp(value, "little", 6) == 0) {
			pPvtData->audioEndian = AVB_AUDIO_ENDIAN_LITTLE;
			pInfo->audioEndian = pPvtData->audioEndian;
		}
		else {
			val = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && val <= AVB_AUDIO_ENDIAN_BIG) {
				pPvtData->audioEndian = (avb_audio_endian_t)val;
				pInfo->audioEndian = pPvtData->audioEndian;
			}
		}
	}
	else if (strcmp(name, "intf_nv_audio_channels") == 0) {
		val = strtol(value, &pEnd, 10);
		if (val >= AVB_AUDIO_CHANNELS_1 && val <= AVB_AUDIO_CHANNELS_8) {
			pPvtData->audioChannels = (avb_audio_channels_t)val;
			pInfo->audioChannels = pPvtData->audioChannels;
		}
	}
	else if (strcmp(name, "intf_nv_allow_resampling") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0') {
			pPvtData->allowResampling = (tmp != 0);
		}
	}
	else if (strcmp(name, "intf_nv_period_time") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp > 0) {
			pPvtData->periodTimeUsec = (U32)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_clock_skew_ppb") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0') {
			pPvtData->clockSkewPPB = (S32)tmp;
		}
	}
	else if (strcmp(name, "intf_nv_fixed_ts_runtime_lead_usec") == 0) {
		tmp = strtol(value, &pEnd, 10);
		if (*pEnd == '\0' && tmp >= 0) {
			pPvtData->fixedTsRuntimeLeadUsec = (U32)tmp;
		}
	}
}

void openavbIntfBus32SplitGenInitCB(media_q_t *pMediaQ)
{
	(void)pMediaQ;
}

void openavbIntfBus32SplitTxInitCB(media_q_t *pMediaQ)
{
	pvt_data_t *pPvtData;
	media_q_pub_map_uncmp_audio_info_t *pInfo;
	if (!pMediaQ) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	if (!pPvtData) {
		AVB_LOG_ERROR("BUS32 split: private data missing");
		return;
	}
	if (!pInfo) {
		AVB_LOG_ERROR("BUS32 split: public map info missing");
		return;
	}
	if (!pPvtData->itemMetaAllocated) {
		if (!openavbMediaQAllocItemIntfData(pMediaQ, sizeof(bus32_item_meta_t))) {
			AVB_LOG_ERROR("BUS32 split: failed to allocate per-item interface metadata");
			return;
		}
		pPvtData->itemMetaAllocated = TRUE;
	}
	openavbMediaQThreadSafeOn(pMediaQ);
	pPvtData->intervalCounter = 0;
	pPvtData->txAsyncProducedCount = 0;
	pPvtData->txAsyncOverwriteCount = 0;
	pPvtData->txAsyncDropCount = 0;
	pPvtData->txCatchupFlushPending = TRUE;
	pPvtData->txCatchupFlushCount = 0;
	pPvtData->txStartupGovernorPacketsRemaining = 0;
	pPvtData->txStartupTrimCount = 0;
	pPvtData->txStartupTrimBytes = 0;
	if (pPvtData->fixedTimestampEnabled) {
		pthread_mutex_lock(&g_mgr.lock);
		if (!ensureSharedFixedTsSeedLocked(pPvtData, pInfo)
			|| !cloneSharedFixedTsToStreamLocked(pPvtData)) {
			AVB_LOG_ERROR("BUS32 split: failed to initialize shared fixed timestamp for stream");
		}
		pthread_mutex_unlock(&g_mgr.lock);
	}
	if (!registerStream(pMediaQ, pPvtData)) {
		AVB_LOG_ERROR("BUS32 split: stream registration failed");
	}
	else {
		AVB_LOGF_INFO("BUS32 split direct producer enabled: uid=%u idx=%u framesPerItem=%u itemSize=%u",
				pPvtData->streamUID,
				pPvtData->streamIndex,
				pInfo->framesPerItem,
				pInfo->itemSize);
	}
}

bool openavbIntfBus32SplitTxCB(media_q_t *pMediaQ)
{
	(void)pMediaQ;
	return TRUE;
}

void openavbIntfBus32SplitRxInitCB(media_q_t *pMediaQ)
{
	(void)pMediaQ;
	AVB_LOG_ERROR("BUS32 split interface is talker-only");
}

bool openavbIntfBus32SplitRxCB(media_q_t *pMediaQ)
{
	(void)pMediaQ;
	return FALSE;
}

void openavbIntfBus32SplitEndCB(media_q_t *pMediaQ)
{
	pvt_data_t *pPvtData;
	if (!pMediaQ) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	if (!pPvtData) {
		return;
	}
	unregisterStream(pPvtData);
}

void openavbIntfBus32SplitGenEndCB(media_q_t *pMediaQ)
{
	pvt_data_t *pPvtData;
	if (!pMediaQ) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	if (!pPvtData) {
		return;
	}
	if (pPvtData->pDeviceName) {
		free(pPvtData->pDeviceName);
		pPvtData->pDeviceName = NULL;
	}
}

void openavbIntfBus32SplitSetStreamUid(media_q_t *pMediaQ, U16 stream_uid)
{
	pvt_data_t *pPvtData;
	if (!pMediaQ || !pMediaQ->pPvtIntfInfo) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pPvtData->streamUID = stream_uid;
}

void openavbIntfBus32SplitEnableFixedTimestamp(media_q_t *pMediaQ, bool enabled, U32 transmitInterval, U32 batchFactor)
{
	pvt_data_t *pPvtData;
	media_q_pub_map_uncmp_audio_info_t *pInfo;
	(void)transmitInterval;
	(void)batchFactor;
	if (!pMediaQ || !pMediaQ->pPvtIntfInfo || !pMediaQ->pPubMapInfo) {
		return;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	pPvtData->fixedTimestampEnabled = enabled;
	if (enabled) {
		pthread_mutex_lock(&g_mgr.lock);
		if (ensureSharedFixedTsSeedLocked(pPvtData, pInfo)
			&& cloneSharedFixedTsToStreamLocked(pPvtData)) {
			AVB_LOGF_INFO("BUS32 split fixed timestamp enabled: uid=%u commonEdge=%" PRIu64 " ns=%" PRIu64 " corr=%d/%u skew=%d",
					  pPvtData->streamUID,
					  g_mgr.fixedTsSeed.edgeTime,
					  pPvtData->mcs.nsPerAdvance,
					  pPvtData->mcs.correctionAmount,
					  pPvtData->mcs.correctionInterval,
					  pPvtData->clockSkewPPB);
		}
		else {
			AVB_LOG_ERROR("BUS32 split: failed to initialize fixed timestamp parameters");
		}
		pthread_mutex_unlock(&g_mgr.lock);
	}
}

extern DLL_EXPORT bool openavbIntfBus32SplitInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB)
{
	pvt_data_t *pPvtData;
	media_q_pub_map_uncmp_audio_info_t *pInfo;

	if (!pMediaQ || !pIntfCB) {
		return FALSE;
	}
	if (!mgrInitIfNeeded()) {
		return FALSE;
	}

	pMediaQ->pPvtIntfInfo = calloc(1, sizeof(pvt_data_t));
	if (!pMediaQ->pPvtIntfInfo) {
		AVB_LOG_ERROR("BUS32 split: allocation failed");
		return FALSE;
	}
	pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;
	pInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	if (!pInfo) {
		AVB_LOG_ERROR("BUS32 split: map public info missing");
		return FALSE;
	}

	pIntfCB->intf_cfg_cb = openavbIntfBus32SplitCfgCB;
	pIntfCB->intf_gen_init_cb = openavbIntfBus32SplitGenInitCB;
	pIntfCB->intf_tx_init_cb = openavbIntfBus32SplitTxInitCB;
	pIntfCB->intf_tx_cb = openavbIntfBus32SplitTxCB;
	pIntfCB->intf_rx_init_cb = openavbIntfBus32SplitRxInitCB;
	pIntfCB->intf_rx_cb = openavbIntfBus32SplitRxCB;
	pIntfCB->intf_end_cb = openavbIntfBus32SplitEndCB;
	pIntfCB->intf_gen_end_cb = openavbIntfBus32SplitGenEndCB;
	pIntfCB->intf_set_stream_uid_cb = openavbIntfBus32SplitSetStreamUid;
	pIntfCB->intf_enable_fixed_timestamp = openavbIntfBus32SplitEnableFixedTimestamp;

	pPvtData->ignoreTimestamp = FALSE;
	pPvtData->pDeviceName = strdup(BUS32_PCM_DEVICE_DEFAULT);
	pPvtData->audioRate = AVB_AUDIO_RATE_48KHZ;
	pPvtData->audioType = AVB_AUDIO_TYPE_INT;
	pPvtData->audioBitDepth = AVB_AUDIO_BIT_DEPTH_32BIT;
	pPvtData->audioEndian = AVB_AUDIO_ENDIAN_LITTLE;
	pPvtData->audioChannels = BUS32_STREAM_CHANNELS;
	pPvtData->allowResampling = FALSE;
	pPvtData->periodTimeUsec = BUS32_PERIOD_TIME_USEC_DEFAULT;
	pPvtData->streamIndex = 0;
	pPvtData->streamIndexSet = FALSE;
	pPvtData->intervalCounter = 0;
	pPvtData->clockSkewPPB = 0;
	pPvtData->fixedTimestampEnabled = FALSE;
	pPvtData->fixedTsRuntimeLeadUsec = BUS32_FIXED_TS_RUNTIME_LEAD_USEC_DEFAULT;
	pPvtData->itemMetaAllocated = FALSE;
	pPvtData->txCaptureDiagEveryPackets = 8000;
	pPvtData->txCaptureDiagPacketCounter = 0;
	pPvtData->txCaptureDiagLogCount = 0;
	pPvtData->txCaptureStaleCount = 0;
	pPvtData->txAsyncProducedCount = 0;
	pPvtData->txAsyncOverwriteCount = 0;
	pPvtData->txAsyncDropCount = 0;
	pPvtData->txCatchupFlushPending = FALSE;
	pPvtData->txCatchupFlushCount = 0;
	pPvtData->txStartupGovernorPacketsRemaining = 0;
	pPvtData->txStartupTrimCount = 0;
	pPvtData->txStartupTrimBytes = 0;
	pPvtData->registered = FALSE;
	pPvtData->streamUID = 0;

	/* Provide defaults to mapping module. */
	pInfo->audioRate = pPvtData->audioRate;
	pInfo->audioType = pPvtData->audioType;
	pInfo->audioBitDepth = pPvtData->audioBitDepth;
	pInfo->audioEndian = pPvtData->audioEndian;
	pInfo->audioChannels = pPvtData->audioChannels;

	return TRUE;
}
