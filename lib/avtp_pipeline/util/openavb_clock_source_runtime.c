/*************************************************************************************************************
Copyright (c) 2026
All rights reserved.
*************************************************************************************************************/

/*
 * MODULE SUMMARY : Runtime cache for selected AVDECC clock source, latest CRF
 * timestamp, and shared media-clock phase.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "openavb_clock_source_runtime_pub.h"
#include "openavb_aem_types_pub.h"
#include "openavb_time_osal_pub.h"

#define AVB_LOG_COMPONENT "clockSourceRuntime"
#include "openavb_pub.h"
#include "openavb_log.h"

typedef struct {
	bool valid;
	U64 timeNs;
	U64 wallCaptureNs;
	bool wallCaptureValid;
	bool uncertain;
	U32 generation;
} openavb_clock_source_runtime_time_t;

typedef struct {
	bool valid;
	U64 anchorNs;
	U32 selectionGeneration;
	U32 recoveryGeneration;
	U32 sourceGeneration;
} openavb_clock_source_runtime_anchor_t;

typedef struct {
	openavb_clock_source_runtime_time_t *pEntries;
	U16 entryCount;
	openavb_clock_source_runtime_time_t fallback;
} openavb_clock_source_runtime_time_bank_t;

typedef struct {
	openavb_clock_source_runtime_anchor_t *pEntries;
	U16 entryCount;
	openavb_clock_source_runtime_anchor_t fallback;
} openavb_clock_source_runtime_anchor_bank_t;

typedef struct {
	openavb_clock_source_runtime_t selection;
	openavb_clock_source_runtime_time_bank_t crfInput;
	openavb_clock_source_runtime_time_bank_t crfOutput;
	openavb_clock_source_runtime_time_bank_t mediaInput;
	openavb_clock_source_runtime_time_bank_t mediaOutput;
	openavb_clock_source_runtime_anchor_bank_t warmupAnchorInput;
	openavb_clock_source_runtime_anchor_bank_t warmupAnchorOutput;
} openavb_clock_source_runtime_state_t;

static pthread_mutex_t gClockSourceRuntimeMutex = PTHREAD_MUTEX_INITIALIZER;
static openavb_clock_source_runtime_state_t gClockSourceRuntime = {0};
static U64 gClockSourceRuntimeCrfDiscontinuityCount = 0;
static U64 gClockSourceRuntimeMediaDiscontinuityCount = 0;

#define CLOCK_SOURCE_RUNTIME_DISCONTINUITY_LOG_NS (100000ULL)
#define CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_WINDOW_NS (250000ULL)
#define CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_MAX_NS (50000ULL)

static const char *clockSourceRuntimeLocationLabel(U16 clock_source_location_type)
{
	if (clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		return "output";
	}
	return "input";
}

static openavb_clock_source_runtime_time_bank_t *clockSourceRuntimeTimeBankForLocation(
	openavb_clock_source_runtime_state_t *pState,
	U16 clock_source_location_type,
	bool mediaClock)
{
	if (!pState) {
		return NULL;
	}
	if (clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		return mediaClock ? &pState->mediaOutput : &pState->crfOutput;
	}
	return mediaClock ? &pState->mediaInput : &pState->crfInput;
}

static openavb_clock_source_runtime_anchor_bank_t *clockSourceRuntimeAnchorBankForLocation(
	openavb_clock_source_runtime_state_t *pState,
	U16 clock_source_location_type)
{
	if (!pState) {
		return NULL;
	}
	if (clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		return &pState->warmupAnchorOutput;
	}
	return &pState->warmupAnchorInput;
}

static void clockSourceRuntimeClearTimeValue(openavb_clock_source_runtime_time_t *pTime)
{
	if (!pTime) {
		return;
	}
	pTime->valid = FALSE;
	pTime->timeNs = 0;
	pTime->wallCaptureNs = 0;
	pTime->wallCaptureValid = FALSE;
	pTime->uncertain = FALSE;
}

static openavb_clock_source_runtime_time_t *clockSourceRuntimeTimeGetSlot(
	openavb_clock_source_runtime_time_bank_t *pBank,
	U16 clock_source_location_index,
	bool create)
{
	if (!pBank) {
		return NULL;
	}

	if (clock_source_location_index == OPENAVB_AEM_DESCRIPTOR_INVALID) {
		return &pBank->fallback;
	}

	if (clock_source_location_index >= pBank->entryCount) {
		if (!create) {
			return NULL;
		}

		U16 oldCount = pBank->entryCount;
		U32 newCount = (U32)clock_source_location_index + 1U;
		openavb_clock_source_runtime_time_t *pNewEntries = (openavb_clock_source_runtime_time_t *)realloc(
			pBank->pEntries,
			newCount * sizeof(*pBank->pEntries));
		if (!pNewEntries) {
			return NULL;
		}
		pBank->pEntries = pNewEntries;
		pBank->entryCount = (U16)newCount;
		memset(&pBank->pEntries[oldCount], 0, (newCount - oldCount) * sizeof(*pBank->pEntries));
	}

	return &pBank->pEntries[clock_source_location_index];
}

static void clockSourceRuntimeTimeBankClearAll(openavb_clock_source_runtime_time_bank_t *pBank)
{
	if (!pBank) {
		return;
	}

	clockSourceRuntimeClearTimeValue(&pBank->fallback);
	for (U16 idx = 0; idx < pBank->entryCount; idx++) {
		clockSourceRuntimeClearTimeValue(&pBank->pEntries[idx]);
	}
}

static void clockSourceRuntimeClearAnchorValue(openavb_clock_source_runtime_anchor_t *pAnchor)
{
	if (!pAnchor) {
		return;
	}
	memset(pAnchor, 0, sizeof(*pAnchor));
}

static openavb_clock_source_runtime_anchor_t *clockSourceRuntimeAnchorGetSlot(
	openavb_clock_source_runtime_anchor_bank_t *pBank,
	U16 clock_source_location_index,
	bool create)
{
	if (!pBank) {
		return NULL;
	}

	if (clock_source_location_index == OPENAVB_AEM_DESCRIPTOR_INVALID) {
		return &pBank->fallback;
	}

	if (clock_source_location_index >= pBank->entryCount) {
		if (!create) {
			return NULL;
		}

		U16 oldCount = pBank->entryCount;
		U32 newCount = (U32)clock_source_location_index + 1U;
		openavb_clock_source_runtime_anchor_t *pNewEntries = (openavb_clock_source_runtime_anchor_t *)realloc(
			pBank->pEntries,
			newCount * sizeof(*pBank->pEntries));
		if (!pNewEntries) {
			return NULL;
		}
		pBank->pEntries = pNewEntries;
		pBank->entryCount = (U16)newCount;
		memset(&pBank->pEntries[oldCount], 0, (newCount - oldCount) * sizeof(*pBank->pEntries));
	}

	return &pBank->pEntries[clock_source_location_index];
}

static void clockSourceRuntimeAnchorBankClearAll(openavb_clock_source_runtime_anchor_bank_t *pBank)
{
	if (!pBank) {
		return;
	}

	clockSourceRuntimeClearAnchorValue(&pBank->fallback);
	for (U16 idx = 0; idx < pBank->entryCount; idx++) {
		clockSourceRuntimeClearAnchorValue(&pBank->pEntries[idx]);
	}
}

void openavbClockSourceRuntimeSetSelection(
	U16 clock_domain_index,
	U16 clock_source_index,
	U16 clock_source_flags,
	U16 clock_source_type,
	U16 clock_source_location_type,
	U16 clock_source_location_index)
{
	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	bool changed = !gClockSourceRuntime.selection.valid ||
		(gClockSourceRuntime.selection.clock_domain_index != clock_domain_index) ||
		(gClockSourceRuntime.selection.clock_source_index != clock_source_index) ||
		(gClockSourceRuntime.selection.clock_source_flags != clock_source_flags) ||
		(gClockSourceRuntime.selection.clock_source_type != clock_source_type) ||
		(gClockSourceRuntime.selection.clock_source_location_type != clock_source_location_type) ||
		(gClockSourceRuntime.selection.clock_source_location_index != clock_source_location_index);

	if (changed) {
		gClockSourceRuntime.selection.valid = TRUE;
		gClockSourceRuntime.selection.clock_domain_index = clock_domain_index;
		gClockSourceRuntime.selection.clock_source_index = clock_source_index;
		gClockSourceRuntime.selection.clock_source_flags = clock_source_flags;
		gClockSourceRuntime.selection.clock_source_type = clock_source_type;
		gClockSourceRuntime.selection.clock_source_location_type = clock_source_location_type;
		gClockSourceRuntime.selection.clock_source_location_index = clock_source_location_index;
		gClockSourceRuntime.selection.generation++;
	}

	pthread_mutex_unlock(&gClockSourceRuntimeMutex);
}

bool openavbClockSourceRuntimeGetSelection(openavb_clock_source_runtime_t *pSelection)
{
	bool ret = FALSE;

	if (!pSelection) {
		return FALSE;
	}

	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	if (gClockSourceRuntime.selection.valid) {
		*pSelection = gClockSourceRuntime.selection;
		ret = TRUE;
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);

	return ret;
}

void openavbClockSourceRuntimeSetCrfTimeForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 crfTimeNs,
	bool uncertain)
{
	U64 nowNs = 0;
	bool haveNow = CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNs);

	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_time_bank_t *pBank =
		clockSourceRuntimeTimeBankForLocation(&gClockSourceRuntime, clock_source_location_type, FALSE);
	openavb_clock_source_runtime_time_t *pCrf =
		clockSourceRuntimeTimeGetSlot(pBank, clock_source_location_index, TRUE);
	if (pCrf) {
		if (pCrf->valid && haveNow && pCrf->wallCaptureValid) {
			U64 projectedPrevNs = pCrf->timeNs;
			U64 wallStepNs = 0;
			if (nowNs >= pCrf->wallCaptureNs) {
				wallStepNs = (nowNs - pCrf->wallCaptureNs);
				projectedPrevNs += wallStepNs;
			}
			S64 stepNs = crfTimeNs >= pCrf->timeNs
				? (S64)(crfTimeNs - pCrf->timeNs)
				: -((S64)(pCrf->timeNs - crfTimeNs));
			S64 errNs = (crfTimeNs >= projectedPrevNs)
				? (S64)(crfTimeNs - projectedPrevNs)
				: -((S64)(projectedPrevNs - crfTimeNs));
			if ((U64)llabs(errNs) > CLOCK_SOURCE_RUNTIME_DISCONTINUITY_LOG_NS) {
				gClockSourceRuntimeCrfDiscontinuityCount++;
				if (gClockSourceRuntimeCrfDiscontinuityCount <= 16 ||
						(gClockSourceRuntimeCrfDiscontinuityCount % 2000ULL) == 0ULL) {
					AVB_LOGF_WARNING(
						"Clock source CRF discontinuity: location=%s/%u new=%llu prev_projected=%llu err=%lldns uncertain=%u generation=%u count=%llu",
						clockSourceRuntimeLocationLabel(clock_source_location_type),
						clock_source_location_index,
						(unsigned long long)crfTimeNs,
						(unsigned long long)projectedPrevNs,
						(long long)errNs,
						uncertain ? 1U : 0U,
						pCrf->generation + 1U,
						(unsigned long long)gClockSourceRuntimeCrfDiscontinuityCount);
					if (clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
						AVB_LOGF_WARNING(
							"TX OUTLIER ROW flags=.Y. stage=clocksrc_runtime event=crf_disc loc=%s/%u new=%llu prev=%llu now=%llu prev_wall=%llu wall_step=%llu step=%lld err=%lld uncertain=%u gen=%u count=%llu",
							clockSourceRuntimeLocationLabel(clock_source_location_type),
							clock_source_location_index,
							(unsigned long long)crfTimeNs,
							(unsigned long long)pCrf->timeNs,
							(unsigned long long)nowNs,
							(unsigned long long)pCrf->wallCaptureNs,
							(unsigned long long)wallStepNs,
							(long long)stepNs,
							(long long)errNs,
							uncertain ? 1U : 0U,
							pCrf->generation + 1U,
							(unsigned long long)gClockSourceRuntimeCrfDiscontinuityCount);
					}
				}
			}
		}
		pCrf->timeNs = crfTimeNs;
		pCrf->wallCaptureNs = nowNs;
		pCrf->wallCaptureValid = haveNow;
		pCrf->uncertain = uncertain;
		pCrf->valid = TRUE;
		pCrf->generation++;
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);
}

void openavbClockSourceRuntimeSetCrfTime(U64 crfTimeNs, bool uncertain)
{
	openavbClockSourceRuntimeSetCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
		OPENAVB_AEM_DESCRIPTOR_INVALID,
		crfTimeNs,
		uncertain);
}

bool openavbClockSourceRuntimeGetCrfTimeForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 *pCrfTimeNs,
	bool *pUncertain,
	U32 *pGeneration)
{
	bool ret = FALSE;
	U64 nowNs = 0;
	bool haveNow = CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNs);

	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_time_bank_t *pBank =
		clockSourceRuntimeTimeBankForLocation(&gClockSourceRuntime, clock_source_location_type, FALSE);
	openavb_clock_source_runtime_time_t *pCrf =
		clockSourceRuntimeTimeGetSlot(pBank, clock_source_location_index, FALSE);
	if (pCrf && pCrf->valid) {
		U64 projectedCrfNs = pCrf->timeNs;
		if (haveNow && pCrf->wallCaptureValid && nowNs >= pCrf->wallCaptureNs) {
			projectedCrfNs += (nowNs - pCrf->wallCaptureNs);
		}
		if (pCrfTimeNs) {
			*pCrfTimeNs = projectedCrfNs;
		}
		if (pUncertain) {
			*pUncertain = pCrf->uncertain;
		}
		if (pGeneration) {
			*pGeneration = pCrf->generation;
		}
		ret = TRUE;
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);

	return ret;
}

bool openavbClockSourceRuntimeGetCrfTime(U64 *pCrfTimeNs, bool *pUncertain, U32 *pGeneration)
{
	return openavbClockSourceRuntimeGetCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
		OPENAVB_AEM_DESCRIPTOR_INVALID,
		pCrfTimeNs,
		pUncertain,
		pGeneration);
}

void openavbClockSourceRuntimeClearCrfTimeForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index)
{
	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_time_bank_t *pBank =
		clockSourceRuntimeTimeBankForLocation(&gClockSourceRuntime, clock_source_location_type, FALSE);
	if (pBank) {
		if (clock_source_location_index == OPENAVB_AEM_DESCRIPTOR_INVALID) {
			clockSourceRuntimeTimeBankClearAll(pBank);
		}
		else {
			openavb_clock_source_runtime_time_t *pCrf =
				clockSourceRuntimeTimeGetSlot(pBank, clock_source_location_index, FALSE);
			if (pCrf) {
				clockSourceRuntimeClearTimeValue(pCrf);
			}
		}
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);
}

void openavbClockSourceRuntimeClearCrfTime(void)
{
	openavbClockSourceRuntimeClearCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
		OPENAVB_AEM_DESCRIPTOR_INVALID);
	openavbClockSourceRuntimeClearCrfTimeForLocation(
		OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
		OPENAVB_AEM_DESCRIPTOR_INVALID);
}

void openavbClockSourceRuntimeSetMediaClockForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 mediaClockNs,
	bool uncertain)
{
	U64 nowNs = 0;
	bool haveNow = CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNs);

	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_time_bank_t *pBank =
		clockSourceRuntimeTimeBankForLocation(&gClockSourceRuntime, clock_source_location_type, TRUE);
	openavb_clock_source_runtime_time_t *pClock =
		clockSourceRuntimeTimeGetSlot(pBank, clock_source_location_index, TRUE);
	if (pClock) {
		U64 storeMediaClockNs = mediaClockNs;
		if (pClock->valid && haveNow && pClock->wallCaptureValid) {
			U64 projectedPrevNs = pClock->timeNs;
			U64 wallStepNs = 0;
			if (nowNs >= pClock->wallCaptureNs) {
				wallStepNs = (nowNs - pClock->wallCaptureNs);
				projectedPrevNs += wallStepNs;
			}
			S64 stepNs = (mediaClockNs >= pClock->timeNs)
				? (S64)(mediaClockNs - pClock->timeNs)
				: -((S64)(pClock->timeNs - mediaClockNs));
			S64 errNs = (mediaClockNs >= projectedPrevNs)
				? (S64)(mediaClockNs - projectedPrevNs)
				: -((S64)(projectedPrevNs - mediaClockNs));
			if (clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT &&
					!uncertain &&
					(U64)llabs(errNs) <= CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_WINDOW_NS) {
				S64 phaseAdjustNs = errNs / 8;
				if (phaseAdjustNs > (S64)CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_MAX_NS) {
					phaseAdjustNs = (S64)CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_MAX_NS;
				}
				else if (phaseAdjustNs < -((S64)CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_MAX_NS)) {
					phaseAdjustNs = -((S64)CLOCK_SOURCE_RUNTIME_OUTPUT_SLEW_MAX_NS);
				}
				storeMediaClockNs = (phaseAdjustNs >= 0)
					? (projectedPrevNs + (U64)phaseAdjustNs)
					: ((projectedPrevNs > (U64)(-phaseAdjustNs))
						? (projectedPrevNs - (U64)(-phaseAdjustNs))
						: 0);
				if (phaseAdjustNs != 0 &&
						(gClockSourceRuntimeMediaDiscontinuityCount < 16 ||
						 (gClockSourceRuntimeMediaDiscontinuityCount % 2000ULL) == 0ULL)) {
					AVB_LOGF_INFO(
						"TX OUTLIER ROW flags=.Y. stage=clocksrc_runtime event=media_slew loc=%s/%u raw=%llu projected=%llu stored=%llu wall_step=%llu step=%lld err=%lld adj=%lld uncertain=%u gen=%u",
						clockSourceRuntimeLocationLabel(clock_source_location_type),
						clock_source_location_index,
						(unsigned long long)mediaClockNs,
						(unsigned long long)projectedPrevNs,
						(unsigned long long)storeMediaClockNs,
						(unsigned long long)wallStepNs,
						(long long)stepNs,
						(long long)errNs,
						(long long)phaseAdjustNs,
						uncertain ? 1U : 0U,
						pClock->generation + 1U);
				}
			}
			if ((U64)llabs(errNs) > CLOCK_SOURCE_RUNTIME_DISCONTINUITY_LOG_NS) {
				gClockSourceRuntimeMediaDiscontinuityCount++;
				if (gClockSourceRuntimeMediaDiscontinuityCount <= 16 ||
						(gClockSourceRuntimeMediaDiscontinuityCount % 2000ULL) == 0ULL) {
					AVB_LOGF_WARNING(
						"Clock source media discontinuity: location=%s/%u new=%llu prev_projected=%llu err=%lldns uncertain=%u generation=%u count=%llu",
						clockSourceRuntimeLocationLabel(clock_source_location_type),
						clock_source_location_index,
						(unsigned long long)mediaClockNs,
						(unsigned long long)projectedPrevNs,
						(long long)errNs,
						uncertain ? 1U : 0U,
						pClock->generation + 1U,
						(unsigned long long)gClockSourceRuntimeMediaDiscontinuityCount);
					if (clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
						AVB_LOGF_WARNING(
							"TX OUTLIER ROW flags=.Y. stage=clocksrc_runtime event=media_disc loc=%s/%u new=%llu prev=%llu now=%llu prev_wall=%llu wall_step=%llu step=%lld err=%lld uncertain=%u gen=%u count=%llu",
							clockSourceRuntimeLocationLabel(clock_source_location_type),
							clock_source_location_index,
							(unsigned long long)mediaClockNs,
							(unsigned long long)pClock->timeNs,
							(unsigned long long)nowNs,
							(unsigned long long)pClock->wallCaptureNs,
							(unsigned long long)wallStepNs,
							(long long)stepNs,
							(long long)errNs,
							uncertain ? 1U : 0U,
							pClock->generation + 1U,
							(unsigned long long)gClockSourceRuntimeMediaDiscontinuityCount);
					}
				}
			}
		}
		pClock->timeNs = storeMediaClockNs;
		pClock->wallCaptureNs = nowNs;
		pClock->wallCaptureValid = haveNow;
		pClock->uncertain = uncertain;
		pClock->valid = TRUE;
		pClock->generation++;
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);
}

bool openavbClockSourceRuntimeGetMediaClockForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 *pMediaClockNs,
	bool *pUncertain,
	U32 *pGeneration)
{
	bool ret = FALSE;
	U64 nowNs = 0;
	bool haveNow = CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNs);

	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_time_bank_t *pBank =
		clockSourceRuntimeTimeBankForLocation(&gClockSourceRuntime, clock_source_location_type, TRUE);
	openavb_clock_source_runtime_time_t *pClock =
		clockSourceRuntimeTimeGetSlot(pBank, clock_source_location_index, FALSE);
	if (pClock && pClock->valid) {
		U64 projectedClockNs = pClock->timeNs;
		if (haveNow && pClock->wallCaptureValid && nowNs >= pClock->wallCaptureNs) {
			projectedClockNs += (nowNs - pClock->wallCaptureNs);
		}
		if (pMediaClockNs) {
			*pMediaClockNs = projectedClockNs;
		}
		if (pUncertain) {
			*pUncertain = pClock->uncertain;
		}
		if (pGeneration) {
			*pGeneration = pClock->generation;
		}
		ret = TRUE;
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);

	return ret;
}

void openavbClockSourceRuntimeClearMediaClockForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index)
{
	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_time_bank_t *pBank =
		clockSourceRuntimeTimeBankForLocation(&gClockSourceRuntime, clock_source_location_type, TRUE);
	if (pBank) {
		if (clock_source_location_index == OPENAVB_AEM_DESCRIPTOR_INVALID) {
			clockSourceRuntimeTimeBankClearAll(pBank);
		}
		else {
			openavb_clock_source_runtime_time_t *pClock =
				clockSourceRuntimeTimeGetSlot(pBank, clock_source_location_index, FALSE);
			if (pClock) {
				clockSourceRuntimeClearTimeValue(pClock);
			}
		}
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);
}

bool openavbClockSourceRuntimeAcquireWarmupAnchorForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U32 selectionGeneration,
	U32 recoveryGeneration,
	U32 sourceGeneration,
	U64 proposedAnchorNs,
	U64 *pAnchorNs,
	bool *pWasCreated)
{
	bool ret = FALSE;

	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	openavb_clock_source_runtime_anchor_bank_t *pBank =
		clockSourceRuntimeAnchorBankForLocation(&gClockSourceRuntime, clock_source_location_type);
	openavb_clock_source_runtime_anchor_t *pAnchor =
		clockSourceRuntimeAnchorGetSlot(pBank, clock_source_location_index, TRUE);
	if (pAnchor) {
		bool create = !pAnchor->valid ||
			(pAnchor->selectionGeneration != selectionGeneration) ||
			(pAnchor->recoveryGeneration != recoveryGeneration) ||
			(pAnchor->sourceGeneration != sourceGeneration);
		if (create) {
			pAnchor->valid = TRUE;
			pAnchor->anchorNs = proposedAnchorNs;
			pAnchor->selectionGeneration = selectionGeneration;
			pAnchor->recoveryGeneration = recoveryGeneration;
			pAnchor->sourceGeneration = sourceGeneration;
		}
		if (pAnchorNs) {
			*pAnchorNs = pAnchor->anchorNs;
		}
		if (pWasCreated) {
			*pWasCreated = create;
		}
		ret = TRUE;
	}
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);

	return ret;
}

void openavbClockSourceRuntimeReset(void)
{
	pthread_mutex_lock(&gClockSourceRuntimeMutex);
	free(gClockSourceRuntime.crfInput.pEntries);
	free(gClockSourceRuntime.crfOutput.pEntries);
	free(gClockSourceRuntime.mediaInput.pEntries);
	free(gClockSourceRuntime.mediaOutput.pEntries);
	free(gClockSourceRuntime.warmupAnchorInput.pEntries);
	free(gClockSourceRuntime.warmupAnchorOutput.pEntries);
	memset(&gClockSourceRuntime, 0, sizeof(gClockSourceRuntime));
	pthread_mutex_unlock(&gClockSourceRuntimeMutex);
}
