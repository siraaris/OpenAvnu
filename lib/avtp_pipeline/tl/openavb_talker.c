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
* MODULE SUMMARY : Talker implementation
*/

#include <stdlib.h>
#include "openavb_platform.h"
#include "openavb_trace.h"
#include "openavb_tl.h"
#include "openavb_avtp.h"
#include "openavb_talker.h"
#include "openavb_avdecc_msg_client.h"
#include "avb_sched.h"

// DEBUG Uncomment to turn on logging for just this module.
//#define AVB_LOG_ON	1

#define	AVB_LOG_COMPONENT	"Talker"
#include "openavb_log.h"

#include "openavb_debug.h"

#define TALKER_WAKE_DIAG_INTERVAL 2048U
#define TALKER_WAKE_LATE_WARN_NS 500000ULL
#define DIRECT_TX_GROUP_UID_COUNT 4U
#define DIRECT_TX_GROUP_FOLLOWER_SLEEP_MSEC 2U
#define DIRECT_TX_GROUP_REQUIRED_MASK ((1u << DIRECT_TX_GROUP_UID_COUNT) - 1u)
#define DIRECT_TX_GROUP_WORK_LEAD_MIN_NS 250000ULL
#define DIRECT_TX_GROUP_WORK_LEAD_INIT_NS 1000000ULL
#define DIRECT_TX_GROUP_WORK_LEAD_MARGIN_NS 250000ULL
#define DIRECT_TX_GROUP_WORK_LEAD_MAX_NS 5000000ULL
#define DIRECT_TX_GROUP_READY_LEAD_NS 250000ULL

typedef struct {
	pthread_mutex_t lock;
	tl_state_t *members[DIRECT_TX_GROUP_UID_COUNT];
	U32 activeMask;
	U32 pendingMask;
	U64 nextCycleNS;
	U64 intervalNS;
	bool useWallTimePacing;
	U64 cycleCount;
	void *sharedRawsock;
	U32 sharedFwmark;
	U64 workLeadNs;
} direct_tx_group_t;

static direct_tx_group_t sDirectTxGroup = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.members = { NULL, NULL, NULL, NULL },
	.activeMask = 0,
	.pendingMask = 0,
	.nextCycleNS = 0,
	.intervalNS = 0,
	.useWallTimePacing = FALSE,
	.cycleCount = 0,
	.sharedRawsock = NULL,
	.sharedFwmark = 0,
	.workLeadNs = DIRECT_TX_GROUP_WORK_LEAD_INIT_NS,
};

static void talkerWakeDiagUpdate(U64 *pMin, U64 *pMax, U64 *pSum, U64 count, U64 value)
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

static void talkerMaybeLogWakeDiag(talker_data_t *pTalkerData)
{
	if (!pTalkerData || pTalkerData->wakeDiagSamples == 0) {
		return;
	}

	if (pTalkerData->wakeDiagSamples < TALKER_WAKE_DIAG_INTERVAL) {
		return;
	}

	AVB_LOGF_WARNING(
		"TALKER WAKE DIAG " STREAMID_FORMAT " wakes=%llu late_events=%llu late_avg=%lluns min=%lluns max=%lluns interval=%lluns",
		STREAMID_ARGS(&pTalkerData->streamID),
		(unsigned long long)pTalkerData->wakeDiagSamples,
		(unsigned long long)pTalkerData->wakeLateCount,
		(unsigned long long)((pTalkerData->wakeDiagSamples > 0) ? (pTalkerData->wakeLateSumNs / pTalkerData->wakeDiagSamples) : 0ULL),
		(unsigned long long)pTalkerData->wakeLateMinNs,
		(unsigned long long)pTalkerData->wakeLateMaxNs,
		(unsigned long long)pTalkerData->intervalNS);

	pTalkerData->wakeDiagSamples = 0;
	pTalkerData->wakeLateCount = 0;
	pTalkerData->wakeLateSumNs = 0;
	pTalkerData->wakeLateMinNs = 0;
	pTalkerData->wakeLateMaxNs = 0;
}

static inline void talkerSleepUntilWallTimeNS(U64 targetWallNs)
{
	while (1) {
		U64 wallNowNs = 0;
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &wallNowNs);
		if (wallNowNs >= targetWallNs) {
			return;
		}

		U64 monoNowNs = 0;
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &monoNowNs);
		SLEEP_UNTIL_NSEC(monoNowNs + (targetWallNs - wallNowNs));
	}
}

static bool xUseDirectTxGroup(const tl_state_t *pTLState)
{
	const talker_data_t *pTalkerData;

	if (!pTLState || pTLState->cfg.role != AVB_ROLE_TALKER) {
		return FALSE;
	}

	pTalkerData = pTLState->pPvtTalkerData;
	if (!pTalkerData) {
		return FALSE;
	}

	if (!pTLState->cfg.intf_cb.intf_get_tx_start_cycle_cb) {
		return FALSE;
	}

	return (pTalkerData->streamID.uniqueID < DIRECT_TX_GROUP_UID_COUNT);
}

static U32 xDirectTxGroupLeaderUid(U32 activeMask)
{
	U32 uid;

	for (uid = 0; uid < DIRECT_TX_GROUP_UID_COUNT; uid++) {
		if (activeMask & (1u << uid)) {
			return uid;
		}
	}

	return DIRECT_TX_GROUP_UID_COUNT;
}

static void xDirectTxGroupRegister(tl_state_t *pTLState)
{
	talker_data_t *pTalkerData;
	avtp_stream_t *pStream;
	U32 uid;
	U32 mask;

	if (!xUseDirectTxGroup(pTLState)) {
		return;
	}

	pTalkerData = pTLState->pPvtTalkerData;
	pStream = pTalkerData ? (avtp_stream_t *)pTalkerData->avtpHandle : NULL;
	uid = pTalkerData->streamID.uniqueID;
	mask = (1u << uid);

	pthread_mutex_lock(&sDirectTxGroup.lock);
	if (pStream) {
		if (!sDirectTxGroup.sharedRawsock) {
			sDirectTxGroup.sharedRawsock = pStream->rawsock;
			sDirectTxGroup.sharedFwmark = TC_AVB_MARK(
				TC_AVB_MARK_CLASS(pStream->tx_fwmark),
				TC_AVB_CLASSA_GROUP_STREAM_SLOT);
		}
		else if (pStream->rawsock && pStream->rawsock != sDirectTxGroup.sharedRawsock) {
			openavbRawsockClose(pStream->rawsock);
		}

		pStream->rawsock = sDirectTxGroup.sharedRawsock;
		pStream->owns_rawsock = FALSE;
		pStream->tx_fwmark = sDirectTxGroup.sharedFwmark;
	}
	sDirectTxGroup.members[uid] = pTLState;
	sDirectTxGroup.activeMask |= mask;
	if (sDirectTxGroup.intervalNS == 0 || pTalkerData->intervalNS < sDirectTxGroup.intervalNS) {
		sDirectTxGroup.intervalNS = pTalkerData->intervalNS;
	}
	if (sDirectTxGroup.nextCycleNS == 0 ||
			(pTalkerData->nextCycleNS != 0 && pTalkerData->nextCycleNS < sDirectTxGroup.nextCycleNS)) {
		sDirectTxGroup.nextCycleNS = pTalkerData->nextCycleNS;
	}
	sDirectTxGroup.useWallTimePacing = pTalkerData->useWallTimePacing;
	AVB_LOGF_WARNING("Direct TX group register: uid=%u active_mask=0x%x leader=%u next_cycle=%" PRIu64 " interval=%" PRIu64,
		uid,
		sDirectTxGroup.activeMask,
		xDirectTxGroupLeaderUid(sDirectTxGroup.activeMask),
		sDirectTxGroup.nextCycleNS,
		sDirectTxGroup.intervalNS);
	AVB_LOGF_WARNING("Direct TX group mark: uid=%u shared_fwmark=%u class=%d slot=%u",
		uid,
		sDirectTxGroup.sharedFwmark,
		TC_AVB_MARK_CLASS(sDirectTxGroup.sharedFwmark),
		TC_AVB_MARK_STREAM(sDirectTxGroup.sharedFwmark));
	pthread_mutex_unlock(&sDirectTxGroup.lock);
}

static void xDirectTxGroupUnregister(tl_state_t *pTLState)
{
	talker_data_t *pTalkerData;
	avtp_stream_t *pStream;
	U32 uid;
	U32 mask;

	if (!xUseDirectTxGroup(pTLState)) {
		return;
	}

	pTalkerData = pTLState->pPvtTalkerData;
	pStream = pTalkerData ? (avtp_stream_t *)pTalkerData->avtpHandle : NULL;
	uid = pTalkerData->streamID.uniqueID;
	mask = (1u << uid);

	pthread_mutex_lock(&sDirectTxGroup.lock);
	if (sDirectTxGroup.members[uid] == pTLState) {
		sDirectTxGroup.members[uid] = NULL;
		sDirectTxGroup.activeMask &= ~mask;
	}
	if (sDirectTxGroup.activeMask == 0) {
		if (pStream) {
			pStream->rawsock = NULL;
		}
		if (sDirectTxGroup.sharedRawsock) {
			openavbRawsockClose(sDirectTxGroup.sharedRawsock);
			sDirectTxGroup.sharedRawsock = NULL;
		}
		sDirectTxGroup.pendingMask = 0;
		sDirectTxGroup.nextCycleNS = 0;
		sDirectTxGroup.intervalNS = 0;
		sDirectTxGroup.useWallTimePacing = FALSE;
		sDirectTxGroup.cycleCount = 0;
		sDirectTxGroup.sharedFwmark = 0;
		sDirectTxGroup.workLeadNs = DIRECT_TX_GROUP_WORK_LEAD_INIT_NS;
	}
	AVB_LOGF_WARNING("Direct TX group unregister: uid=%u active_mask=0x%x leader=%u",
		uid,
		sDirectTxGroup.activeMask,
		xDirectTxGroupLeaderUid(sDirectTxGroup.activeMask));
	pthread_mutex_unlock(&sDirectTxGroup.lock);
}

static bool xDirectTxGroupDoStream(tl_state_t *pTLState)
{
	openavb_tl_cfg_t *pCfg;
	talker_data_t *pTalkerData;
	U32 selfUid;
	U32 leaderUid;
	U32 activeMask;
	U32 pendingMask;
	U32 sentMask = 0;
	U64 targetCycleNS;
	U64 wakeCycleNS;
	U64 intervalNS;
	bool useWallTimePacing;
	U64 nowNS = 0;
	U64 housekeepingNowNS = 0;
	U64 cycleCount = 0;
	U64 sourcePresentationNS = 0;
	tl_state_t *orderedStates[DIRECT_TX_GROUP_UID_COUNT] = { NULL, NULL, NULL, NULL };
	U32 orderCount = 0;
	U32 uid;
	U64 maxSubmitAdvanceNs = 0;
	U64 workLeadNs = DIRECT_TX_GROUP_WORK_LEAD_INIT_NS;
	bool sourceDrivenCycle = FALSE;
	bool newCycle = FALSE;

	if (!xUseDirectTxGroup(pTLState) || !pTLState->bStreaming) {
		return FALSE;
	}

	pCfg = &pTLState->cfg;
	pTalkerData = pTLState->pPvtTalkerData;
	selfUid = pTalkerData->streamID.uniqueID;

	pthread_mutex_lock(&sDirectTxGroup.lock);
	activeMask = sDirectTxGroup.activeMask;
	pendingMask = sDirectTxGroup.pendingMask;
	leaderUid = xDirectTxGroupLeaderUid(activeMask);
	targetCycleNS = sDirectTxGroup.nextCycleNS ? sDirectTxGroup.nextCycleNS : pTalkerData->nextCycleNS;
	intervalNS = sDirectTxGroup.intervalNS ? sDirectTxGroup.intervalNS : pTalkerData->intervalNS;
	useWallTimePacing = sDirectTxGroup.useWallTimePacing;
	workLeadNs = sDirectTxGroup.workLeadNs ? sDirectTxGroup.workLeadNs : DIRECT_TX_GROUP_WORK_LEAD_INIT_NS;
	pthread_mutex_unlock(&sDirectTxGroup.lock);

	if (!(activeMask & (1u << selfUid))) {
		SLEEP_MSEC(DIRECT_TX_GROUP_FOLLOWER_SLEEP_MSEC);
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);
		if (housekeepingNowNS > pTalkerData->nextSecondNS) {
			pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
			return TRUE;
		}
		return FALSE;
	}

	if ((activeMask & DIRECT_TX_GROUP_REQUIRED_MASK) != DIRECT_TX_GROUP_REQUIRED_MASK) {
		if (pTalkerData->wakeDiagLogCount < 32) {
			AVB_LOGF_WARNING(
				"Direct TX group waiting for full registration: uid=%u active_mask=0x%x required_mask=0x%x leader=%u",
				selfUid,
				activeMask,
				DIRECT_TX_GROUP_REQUIRED_MASK,
				leaderUid);
			pTalkerData->wakeDiagLogCount++;
		}
		SLEEP_MSEC(DIRECT_TX_GROUP_FOLLOWER_SLEEP_MSEC);
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);
		if (housekeepingNowNS > pTalkerData->nextSecondNS) {
			pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
			return TRUE;
		}
		return FALSE;
	}

	if (leaderUid != selfUid) {
		SLEEP_MSEC(DIRECT_TX_GROUP_FOLLOWER_SLEEP_MSEC);
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);
		if (housekeepingNowNS > pTalkerData->nextSecondNS) {
			pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
			return TRUE;
		}
		return FALSE;
	}

	pthread_mutex_lock(&sDirectTxGroup.lock);
	activeMask = sDirectTxGroup.activeMask;
	if (sDirectTxGroup.pendingMask == 0) {
		sDirectTxGroup.pendingMask = activeMask & DIRECT_TX_GROUP_REQUIRED_MASK;
		newCycle = TRUE;
	}
	pendingMask = sDirectTxGroup.pendingMask;
	for (uid = 0; uid < DIRECT_TX_GROUP_UID_COUNT; uid++) {
		tl_state_t *pCandState;
		U64 submitAdvanceNs;

		if (!(pendingMask & (1u << uid))) {
			continue;
		}
		pCandState = sDirectTxGroup.members[uid];
		if (!pCandState || !pCandState->bStreaming || !pCandState->pPvtTalkerData) {
			continue;
		}

		submitAdvanceNs = ((U64)pCandState->cfg.tx_submit_ahead_usec +
				(U64)pCandState->cfg.tx_submit_skew_usec) * 1000ULL;
		if (submitAdvanceNs > maxSubmitAdvanceNs) {
			maxSubmitAdvanceNs = submitAdvanceNs;
		}
	}
	cycleCount = newCycle ? ++sDirectTxGroup.cycleCount : sDirectTxGroup.cycleCount;
	pthread_mutex_unlock(&sDirectTxGroup.lock);

	if (newCycle && useWallTimePacing && pCfg->intf_cb.intf_prepare_tx_cycle_cb) {
		if (!pCfg->intf_cb.intf_prepare_tx_cycle_cb(pTLState->pMediaQ, intervalNS, &sourcePresentationNS)) {
			pthread_mutex_lock(&sDirectTxGroup.lock);
			if (sDirectTxGroup.pendingMask == pendingMask) {
				sDirectTxGroup.pendingMask = 0;
				sDirectTxGroup.nextCycleNS = 0;
			}
			pthread_mutex_unlock(&sDirectTxGroup.lock);
			pTalkerData->nextCycleNS = 0;
			SLEEP_NSEC(50000ULL);
			CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);
			if (housekeepingNowNS > pTalkerData->nextSecondNS) {
				pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
				return TRUE;
			}
			return FALSE;
		}

		{
			U64 transitNs = (U64)pCfg->max_transit_usec * 1000ULL;
			targetCycleNS = (sourcePresentationNS > transitNs) ? (sourcePresentationNS - transitNs) : 0ULL;
			sourceDrivenCycle = TRUE;
			pthread_mutex_lock(&sDirectTxGroup.lock);
			sDirectTxGroup.nextCycleNS = targetCycleNS;
			pthread_mutex_unlock(&sDirectTxGroup.lock);
			pTalkerData->nextCycleNS = targetCycleNS;
		}
	}

	wakeCycleNS = targetCycleNS;
	if (maxSubmitAdvanceNs != 0 && wakeCycleNS > maxSubmitAdvanceNs) {
		wakeCycleNS -= maxSubmitAdvanceNs;
	}
	if (workLeadNs != 0 && wakeCycleNS > workLeadNs) {
		wakeCycleNS -= workLeadNs;
	}
	else {
		wakeCycleNS = 0;
	}

	if (useWallTimePacing) {
		U64 wallNowNs = 0;
		U64 staleThresholdNs = intervalNS * 4ULL;
		if (staleThresholdNs < 2000000ULL) {
			staleThresholdNs = 2000000ULL;
		}
		if (CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &wallNowNs) &&
				targetCycleNS != 0 &&
				(targetCycleNS + staleThresholdNs) < wallNowNs) {
			U64 staleTargetCycleNS = targetCycleNS;
			{
				U64 rebasedCycleNS = ((wallNowNs + intervalNS - 1ULL) / intervalNS) * intervalNS;
				pthread_mutex_lock(&sDirectTxGroup.lock);
				if (sourceDrivenCycle || sDirectTxGroup.nextCycleNS < rebasedCycleNS) {
					sDirectTxGroup.nextCycleNS = rebasedCycleNS;
				}
				targetCycleNS = sDirectTxGroup.nextCycleNS;
				pthread_mutex_unlock(&sDirectTxGroup.lock);
				pTalkerData->nextCycleNS = targetCycleNS;
				AVB_LOGF_WARNING(
					"Direct TX grouped cycle rebase: leader=%u stale_target=%" PRIu64 " now=%" PRIu64 " rebased=%" PRIu64 " interval=%" PRIu64 " source_driven=%u",
					selfUid,
					staleTargetCycleNS,
					wallNowNs,
					rebasedCycleNS,
					intervalNS,
					sourceDrivenCycle ? 1u : 0u);
			}
		}
		wakeCycleNS = targetCycleNS;
		if (maxSubmitAdvanceNs != 0 && wakeCycleNS > maxSubmitAdvanceNs) {
			wakeCycleNS -= maxSubmitAdvanceNs;
		}
		if (workLeadNs != 0 && wakeCycleNS > workLeadNs) {
			wakeCycleNS -= workLeadNs;
		}
		else {
			wakeCycleNS = 0;
		}
		talkerSleepUntilWallTimeNS(wakeCycleNS);
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNS);
	}
	else {
		SLEEP_UNTIL_NSEC(wakeCycleNS);
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNS);
	}
	CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);

	pthread_mutex_lock(&sDirectTxGroup.lock);
	activeMask = sDirectTxGroup.activeMask;
	pendingMask = sDirectTxGroup.pendingMask;
	for (uid = 0; uid < DIRECT_TX_GROUP_UID_COUNT; uid++) {
		U32 bestUid = DIRECT_TX_GROUP_UID_COUNT;
		U32 bestSkew = 0;
		U32 cand;

		for (cand = 0; cand < DIRECT_TX_GROUP_UID_COUNT; cand++) {
			tl_state_t *pCandState;
			talker_data_t *pCandTalker;

			if (!(pendingMask & (1u << cand))) {
				continue;
			}
			pCandState = sDirectTxGroup.members[cand];
			if (!pCandState || !pCandState->bStreaming || !pCandState->pPvtTalkerData) {
				continue;
			}
			pCandTalker = pCandState->pPvtTalkerData;
			if (!pCandTalker->avtpHandle) {
				continue;
			}
			if (bestUid == DIRECT_TX_GROUP_UID_COUNT ||
					pCandState->cfg.tx_submit_skew_usec > bestSkew ||
					(pCandState->cfg.tx_submit_skew_usec == bestSkew && cand < bestUid)) {
				U32 alreadyUsed = 0;
				U32 idx;
				for (idx = 0; idx < orderCount; idx++) {
					if (orderedStates[idx] == pCandState) {
						alreadyUsed = 1;
						break;
					}
				}
				if (!alreadyUsed) {
					bestUid = cand;
					bestSkew = pCandState->cfg.tx_submit_skew_usec;
				}
			}
		}

		if (bestUid == DIRECT_TX_GROUP_UID_COUNT) {
			break;
		}
		orderedStates[orderCount++] = sDirectTxGroup.members[bestUid];
	}
	pthread_mutex_unlock(&sDirectTxGroup.lock);

	if (orderCount == 0) {
		SLEEP_NSEC(50000ULL);
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);
		if (housekeepingNowNS > pTalkerData->nextSecondNS) {
			pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
			return TRUE;
		}
		return FALSE;
	}

	if (cycleCount <= 16) {
		talker_data_t *pOrder0 = (orderCount > 0 && orderedStates[0]) ? (talker_data_t *)orderedStates[0]->pPvtTalkerData : NULL;
		talker_data_t *pOrder1 = (orderCount > 1 && orderedStates[1]) ? (talker_data_t *)orderedStates[1]->pPvtTalkerData : NULL;
		talker_data_t *pOrder2 = (orderCount > 2 && orderedStates[2]) ? (talker_data_t *)orderedStates[2]->pPvtTalkerData : NULL;
		talker_data_t *pOrder3 = (orderCount > 3 && orderedStates[3]) ? (talker_data_t *)orderedStates[3]->pPvtTalkerData : NULL;
		AVB_LOGF_WARNING("Direct TX grouped cycle: leader=%u active_mask=0x%x order=%u,%u,%u,%u wake=%" PRIu64 " target=%" PRIu64 " interval=%" PRIu64 " submit_advance=%" PRIu64 " work_lead=%" PRIu64,
			selfUid,
			activeMask,
			pOrder0 ? pOrder0->streamID.uniqueID : 255u,
			pOrder1 ? pOrder1->streamID.uniqueID : 255u,
			pOrder2 ? pOrder2->streamID.uniqueID : 255u,
			pOrder3 ? pOrder3->streamID.uniqueID : 255u,
			wakeCycleNS,
			targetCycleNS,
			intervalNS,
			maxSubmitAdvanceNs,
			workLeadNs);
	}

	{
		U64 burstStartNs = 0;
		U64 burstEndNs = 0;
		U64 burstDurationNs = 0;
		U32 firstUid = 255u;
		U32 lastUid = 255u;
		U64 memberStartNs[DIRECT_TX_GROUP_UID_COUNT] = { 0, 0, 0, 0 };
		U64 memberEndNs[DIRECT_TX_GROUP_UID_COUNT] = { 0, 0, 0, 0 };
		U64 memberDurationNs[DIRECT_TX_GROUP_UID_COUNT] = { 0, 0, 0, 0 };
		U32 memberUid[DIRECT_TX_GROUP_UID_COUNT] = { 255u, 255u, 255u, 255u };
		openavbRC memberRc[DIRECT_TX_GROUP_UID_COUNT] = { 0, 0, 0, 0 };

		if (useWallTimePacing) {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &burstStartNs);
		}
		else {
			CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &burstStartNs);
		}

	for (uid = 0; uid < orderCount; uid++) {
		tl_state_t *pMemberState = orderedStates[uid];
		talker_data_t *pMemberTalker;
		avtp_stream_t *pMemberStream;
		openavbRC rc;

		if (!pMemberState || !pMemberState->bStreaming || !pMemberState->pPvtTalkerData) {
			continue;
		}
		pMemberTalker = pMemberState->pPvtTalkerData;
		pMemberStream = (avtp_stream_t *)pMemberTalker->avtpHandle;
		if (!pMemberStream) {
			continue;
		}

		if (firstUid == 255u) {
			firstUid = pMemberTalker->streamID.uniqueID;
		}
		lastUid = pMemberTalker->streamID.uniqueID;
		memberUid[uid] = pMemberTalker->streamID.uniqueID;
		if (useWallTimePacing) {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &memberStartNs[uid]);
		}
		else {
			CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &memberStartNs[uid]);
		}

		/*
		 * Grouped direct AAF owns submit pacing at the group level. Wake once
		 * at the earliest group submit point, then queue the whole burst
		 * immediately in skew order and flush once on the final member.
		 * This matches the simulator's grouped sender more closely and avoids
		 * holding the earliest packet until the last member's submit slot.
		 */
		pMemberStream->tx_skip_submit_pacing = TRUE;
		pMemberTalker->cntWakes++;
		rc = openavbAvtpTx(
			pMemberTalker->avtpHandle,
			(uid + 1u) == orderCount,
			pMemberState->cfg.tx_blocking_in_intf);
		pMemberStream->tx_skip_submit_pacing = FALSE;
		memberRc[uid] = rc;
		if (useWallTimePacing) {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &memberEndNs[uid]);
		}
		else {
			CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &memberEndNs[uid]);
		}
		if (memberEndNs[uid] >= memberStartNs[uid]) {
			memberDurationNs[uid] = memberEndNs[uid] - memberStartNs[uid];
		}
		if (IS_OPENAVB_SUCCESS(rc)) {
			pMemberTalker->cntFrames++;
			sentMask |= (1u << pMemberTalker->streamID.uniqueID);
		}
		if (cycleCount <= 16) {
			AVB_LOGF_WARNING("Direct TX grouped result: cycle=%" PRIu64 " uid=%u rc=0x%x packet_ready=%u frames=%lu wakes=%lu",
				cycleCount,
				pMemberTalker->streamID.uniqueID,
				(unsigned)rc,
				IS_OPENAVB_SUCCESS(rc) ? 1u : 0u,
				pMemberTalker->cntFrames,
				pMemberTalker->cntWakes);
		}
	}

		if (useWallTimePacing) {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &burstEndNs);
		}
		else {
			CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &burstEndNs);
		}
		if (burstEndNs >= burstStartNs) {
			burstDurationNs = burstEndNs - burstStartNs;
		}
		if (burstDurationNs > 0) {
			U64 desiredWorkLeadNs = burstDurationNs + DIRECT_TX_GROUP_WORK_LEAD_MARGIN_NS;
			if (desiredWorkLeadNs < DIRECT_TX_GROUP_WORK_LEAD_MIN_NS) {
				desiredWorkLeadNs = DIRECT_TX_GROUP_WORK_LEAD_MIN_NS;
			}
			if (desiredWorkLeadNs > DIRECT_TX_GROUP_WORK_LEAD_MAX_NS) {
				desiredWorkLeadNs = DIRECT_TX_GROUP_WORK_LEAD_MAX_NS;
			}
			pthread_mutex_lock(&sDirectTxGroup.lock);
			if (desiredWorkLeadNs > sDirectTxGroup.workLeadNs) {
				sDirectTxGroup.workLeadNs = desiredWorkLeadNs;
			}
			workLeadNs = sDirectTxGroup.workLeadNs;
			pthread_mutex_unlock(&sDirectTxGroup.lock);
		}
		if ((burstDurationNs >= 200000ULL || cycleCount <= 16) && firstUid != 255u) {
			AVB_LOGF_WARNING(
				"Direct TX grouped burst: cycle=%" PRIu64 " leader=%u first_uid=%u last_uid=%u members=%u duration=%" PRIu64 "ns wake=%" PRIu64 " target=%" PRIu64 " submit_advance=%" PRIu64 " work_lead=%" PRIu64,
				cycleCount,
				selfUid,
				firstUid,
				lastUid,
				orderCount,
				burstDurationNs,
				wakeCycleNS,
				targetCycleNS,
				maxSubmitAdvanceNs,
				workLeadNs);
			if (burstDurationNs >= 500000ULL) {
				for (uid = 0; uid < orderCount; uid++) {
					if (memberUid[uid] == 255u) {
						continue;
					}
					AVB_LOGF_WARNING(
						"Direct TX grouped member: cycle=%" PRIu64 " uid=%u rc=0x%x duration=%" PRIu64 "ns start_offset=%" PRIu64 "ns end_offset=%" PRIu64 "ns",
						cycleCount,
						memberUid[uid],
						(unsigned)memberRc[uid],
						memberDurationNs[uid],
						(memberStartNs[uid] >= burstStartNs) ? (memberStartNs[uid] - burstStartNs) : 0ULL,
						(memberEndNs[uid] >= burstStartNs) ? (memberEndNs[uid] - burstStartNs) : 0ULL);
				}
			}
		}
	}

	if (useWallTimePacing) {
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNS);
	}
	else {
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNS);
	}

	pthread_mutex_lock(&sDirectTxGroup.lock);
	if (sDirectTxGroup.activeMask != 0) {
		if (sentMask == 0) {
			sDirectTxGroup.pendingMask = 0;
			sDirectTxGroup.nextCycleNS = 0;
		}
		else {
			sDirectTxGroup.pendingMask &= ~sentMask;
		}
		if (sDirectTxGroup.pendingMask != 0) {
			sDirectTxGroup.nextCycleNS = targetCycleNS;
		}
		else if (sourceDrivenCycle) {
			sDirectTxGroup.nextCycleNS = 0;
		}
		else {
			sDirectTxGroup.nextCycleNS = targetCycleNS + intervalNS;
			if ((sDirectTxGroup.nextCycleNS + (pCfg->max_transmit_deficit_usec * 1000ULL)) < nowNS) {
				nowNS = ((nowNS + intervalNS) / intervalNS) * intervalNS;
				sDirectTxGroup.nextCycleNS = nowNS + intervalNS;
			}
		}
	}
	if (pTalkerData) {
		pTalkerData->nextCycleNS = sDirectTxGroup.nextCycleNS;
	}
	pthread_mutex_unlock(&sDirectTxGroup.lock);

	if (housekeepingNowNS > pTalkerData->nextSecondNS) {
		pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
		return TRUE;
	}

	return FALSE;
}



bool talkerStartStream(tl_state_t *pTLState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return FALSE;
	}

	openavb_tl_cfg_t *pCfg = &pTLState->cfg;
	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;

	assert(!pTLState->bStreaming);

	AVB_LOGF_WARNING(STREAMID_FORMAT", talkerStartStream: name=%s initial=%d defer_selected=%d stable_wait=%u",
		STREAMID_ARGS(&pTalkerData->streamID),
		pCfg->friendly_name,
		pCfg->initial_state,
		pCfg->defer_start_until_selected_clock ? 1 : 0,
		pCfg->defer_start_stable_usec);

	pTalkerData->wakeFrames = pCfg->max_interval_frames * pCfg->batch_factor;

	// Set a max_transmit_deficit_usec default
	if (pCfg->max_transmit_deficit_usec == 0)
		pCfg->max_transmit_deficit_usec = 50000;

	U32 transmitInterval = pTalkerData->classRate;
	if (pCfg->map_cb.map_transmit_interval_cb(pTLState->pMediaQ)) {
		// Override the class observation interval with the one provided by the mapping module.
		transmitInterval = pCfg->map_cb.map_transmit_interval_cb(pTLState->pMediaQ);
	}

	if (pCfg->intf_cb.intf_enable_fixed_timestamp) {
		pCfg->intf_cb.intf_enable_fixed_timestamp(pTLState->pMediaQ, pCfg->fixed_timestamp, transmitInterval, pCfg->batch_factor);
	} else if (pCfg->fixed_timestamp) {
		AVB_LOG_ERROR("Fixed timestamp enabled but interface doesn't support it");
	}

	openavbRC rc = openavbAvtpTxInit(pTLState->pMediaQ,
		&pCfg->map_cb,
		&pCfg->intf_cb,
		pTalkerData->ifname,
		&pTalkerData->streamID,
		pTalkerData->destAddr,
		pCfg->max_transit_usec,
		pCfg->tx_submit_ahead_usec,
		pCfg->tx_submit_skew_usec,
		pTalkerData->fwmark,
		pTalkerData->vlanID,
		pTalkerData->vlanPCP,
		pTalkerData->wakeFrames * pCfg->raw_tx_buffers,
		&pTalkerData->avtpHandle);
	if (IS_OPENAVB_FAILURE(rc)) {
		AVB_LOG_ERROR("Failed to create AVTP stream");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return FALSE;
	}

	avtp_stream_t *pStream = (avtp_stream_t *)(pTalkerData->avtpHandle);

	pTalkerData->wakeRate = transmitInterval / pCfg->batch_factor;

	pTalkerData->sleepUsec = MICROSECONDS_PER_SECOND / pTalkerData->wakeRate;
	pTalkerData->intervalNS = NANOSECONDS_PER_SECOND / pTalkerData->wakeRate;

	U32 SRKbps = ((unsigned long)pTalkerData->classRate * (unsigned long)pCfg->max_interval_frames * (unsigned long)pStream->frameLen * 8L) / 1000;
	U32 DataKbps = ((unsigned long)pTalkerData->wakeRate * (unsigned long)pCfg->max_interval_frames * (unsigned long)pStream->frameLen * 8L) / 1000;

	AVB_LOGF_INFO(STREAMID_FORMAT", sr-rate=%" PRIu32 ", data-rate=%lu, frames=%" PRIu16 ", size=%" PRIu16 ", batch=%" PRIu32 ", sleep=%" PRIu64 "us, sr-Kbps=%d, data-Kbps=%d",
		STREAMID_ARGS(&pTalkerData->streamID), pTalkerData->classRate, pTalkerData->wakeRate,
		pTalkerData->tSpec.maxIntervalFrames, pTalkerData->tSpec.maxFrameSize,
		pCfg->batch_factor, pTalkerData->intervalNS / 1000, SRKbps, DataKbps);


	// number of intervals per report
	pTalkerData->wakesPerReport = pCfg->report_seconds * NANOSECONDS_PER_SECOND / pTalkerData->intervalNS;
	// counts of intervals and frames between reports
	pTalkerData->cntFrames = 0;
	pTalkerData->cntWakes = 0;

	// setup the initial times
	U64 nowNS;
	U64 housekeepingNowNS = 0;
	U64 sharedNextCycleNS = 0;
	pTalkerData->useWallTimePacing = FALSE;
#if IGB_LAUNCHTIME_ENABLED || ATL_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
	if (!pCfg->tx_blocking_in_intf) {
		pTalkerData->useWallTimePacing = TRUE;
	}
#endif

	bool useBusySpinTimebase = FALSE;
	if (pCfg->spin_wait && !pTalkerData->useWallTimePacing) {
#if !IGB_LAUNCHTIME_ENABLED && !ATL_LAUNCHTIME_ENABLED && !SOCKET_LAUNCHTIME_ENABLED
		// Busy-spin wait path compares against WALLTIME.
		useBusySpinTimebase = TRUE;
#endif
	}

	if (pTalkerData->useWallTimePacing || useBusySpinTimebase) {
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNS);
	}
	else {
		// Sleep-based wait path uses CLOCK_MONOTONIC absolute deadlines.
		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNS);
	}
	AVB_LOGF_INFO(STREAMID_FORMAT", talker pacing base=%s",
		STREAMID_ARGS(&pTalkerData->streamID),
		pTalkerData->useWallTimePacing ? "gptp-walltime" : (useBusySpinTimebase ? "walltime-spin" : "monotonic"));

	// Align clock : allows for some performance gain
	nowNS = ((nowNS + (pTalkerData->intervalNS)) / pTalkerData->intervalNS) * pTalkerData->intervalNS;

	if (pTalkerData->useWallTimePacing &&
		pCfg->intf_cb.intf_get_tx_start_cycle_cb &&
		pCfg->intf_cb.intf_get_tx_start_cycle_cb(pTLState->pMediaQ, pTalkerData->intervalNS, &sharedNextCycleNS) &&
		sharedNextCycleNS > nowNS) {
		pTalkerData->nextCycleNS = sharedNextCycleNS;
		AVB_LOGF_INFO(STREAMID_FORMAT", talker using shared start cycle=%" PRIu64 " interval=%" PRIu64,
			STREAMID_ARGS(&pTalkerData->streamID),
			pTalkerData->nextCycleNS,
			pTalkerData->intervalNS);
	}
	else {
		pTalkerData->nextCycleNS = nowNS + pTalkerData->intervalNS;
	}

	// Keep reporting/housekeeping on monotonic time so counter publication
	// is not stranded if gPTP walltime steps during startup or GM changes.
	CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);
	pTalkerData->nextReportNS = housekeepingNowNS + (pCfg->report_seconds * NANOSECONDS_PER_SECOND);
	pTalkerData->lastReportFrames = 0;
	pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
	// Clear stats
	openavbTalkerClearStats(pTLState);
	pTalkerData->aecpCounters.stream_start++;

	// we're good to go!
	pTLState->bStreaming = TRUE;
	xDirectTxGroupRegister(pTLState);
	openavbAvdeccMsgClntPublishCounters(pTLState);

	AVB_TRACE_EXIT(AVB_TRACE_TL);
	return TRUE;
}

void talkerStopStream(tl_state_t *pTLState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	openavb_avtp_diag_counters_t liveCounters;
	if (!pTalkerData) {
		AVB_LOG_ERROR("Invalid listener data");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	void *rawsock = NULL;
	if (pTalkerData->avtpHandle) {
		rawsock = ((avtp_stream_t*)pTalkerData->avtpHandle)->rawsock;
	}

	openavbTalkerAddStat(pTLState, TL_STAT_TX_CALLS, pTalkerData->cntWakes);
	openavbTalkerAddStat(pTLState, TL_STAT_TX_FRAMES, pTalkerData->cntFrames);
//	openavbTalkerAddStat(pTLState, TL_STAT_TX_LATE, 0);		// Can't calculate at this time
	openavbTalkerAddStat(pTLState, TL_STAT_TX_BYTES, openavbAvtpBytes(pTalkerData->avtpHandle));

	AVB_LOGF_INFO("TX "STREAMID_FORMAT", Totals: calls=%" PRIu64 ", frames=%" PRIu64 ", late=%" PRIu64 ", bytes=%" PRIu64 ", TXOutOfBuffs=%ld",
		STREAMID_ARGS(&pTalkerData->streamID),
		openavbTalkerGetStat(pTLState, TL_STAT_TX_CALLS),
		openavbTalkerGetStat(pTLState, TL_STAT_TX_FRAMES),
		openavbTalkerGetStat(pTLState, TL_STAT_TX_LATE),
		openavbTalkerGetStat(pTLState, TL_STAT_TX_BYTES),
		rawsock ? openavbRawsockGetTXOutOfBuffers(rawsock) : 0
		);

	if (pTLState->bStreaming) {
		xDirectTxGroupUnregister(pTLState);
		openavbAvtpGetDiagCounters(pTalkerData->avtpHandle, &liveCounters);
		openavbAvtpDiagCountersAccumulate(&pTalkerData->aecpCounters, &liveCounters);
		pTalkerData->aecpCounters.stream_stop++;
		openavbAvtpShutdownTalker(pTalkerData->avtpHandle);
		pTalkerData->avtpHandle = NULL;
		pTLState->bStreaming = FALSE;
	}
	openavbAvdeccMsgClntPublishCounters(pTLState);

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

static inline void talkerShowStats(talker_data_t *pTalkerData, tl_state_t *pTLState)
{
	S32 late = pTalkerData->wakesPerReport - pTalkerData->cntWakes;
	U64 bytes = openavbAvtpBytes(pTalkerData->avtpHandle);
	if (late < 0) late = 0;
	U32 txbuf = openavbAvtpTxBufferLevel(pTalkerData->avtpHandle);
	U32 mqbuf = openavbMediaQCountItems(pTLState->pMediaQ, TRUE);

	AVB_LOGRT_INFO(LOG_RT_BEGIN, LOG_RT_ITEM, FALSE, "TX UID:%d, ", LOG_RT_DATATYPE_U16, &pTalkerData->streamID.uniqueID);
	AVB_LOGRT_INFO(FALSE, LOG_RT_ITEM, FALSE, "calls=%ld, ", LOG_RT_DATATYPE_U32, &pTalkerData->cntWakes);
	AVB_LOGRT_INFO(FALSE, LOG_RT_ITEM, FALSE, "frames=%ld, ", LOG_RT_DATATYPE_U32, &pTalkerData->cntFrames);
	AVB_LOGRT_INFO(FALSE, LOG_RT_ITEM, FALSE, "late=%d, ", LOG_RT_DATATYPE_U32, &late);
	AVB_LOGRT_INFO(FALSE, LOG_RT_ITEM, FALSE, "bytes=%lld, ", LOG_RT_DATATYPE_U64, &bytes);
	AVB_LOGRT_INFO(FALSE, LOG_RT_ITEM, FALSE, "txbuf=%d, ", LOG_RT_DATATYPE_U32, &txbuf);
	AVB_LOGRT_INFO(FALSE, LOG_RT_ITEM, LOG_RT_END, "mqbuf=%d, ", LOG_RT_DATATYPE_U32, &mqbuf);

	openavbTalkerAddStat(pTLState, TL_STAT_TX_LATE, late);
	openavbTalkerAddStat(pTLState, TL_STAT_TX_BYTES, bytes);
	openavbAvdeccMsgClntPublishCounters(pTLState);
}

static inline bool talkerDoStream(tl_state_t *pTLState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return FALSE;
	}

	openavb_tl_cfg_t *pCfg = &pTLState->cfg;
	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	bool bRet = FALSE;

	if (pTLState->bStreaming) {
		U64 nowNS;
		U64 housekeepingNowNS = 0;
		U64 wakeWallNs = 0;
		bool usedBusySpin = FALSE;

		if (xUseDirectTxGroup(pTLState)) {
			AVB_TRACE_EXIT(AVB_TRACE_TL);
			return xDirectTxGroupDoStream(pTLState);
		}

		if (!pCfg->tx_blocking_in_intf) {

			if (pTalkerData->useWallTimePacing) {
				talkerSleepUntilWallTimeNS(pTalkerData->nextCycleNS);
			}
			else if (!pCfg->spin_wait) {
				// sleep until the next interval
				SLEEP_UNTIL_NSEC(pTalkerData->nextCycleNS);
			} else {
#if !IGB_LAUNCHTIME_ENABLED && !ATL_LAUNCHTIME_ENABLED && !SOCKET_LAUNCHTIME_ENABLED
				SPIN_UNTIL_NSEC(pTalkerData->nextCycleNS);
				usedBusySpin = TRUE;
#else
				// In launch-time builds, SPIN_UNTIL_NSEC is disabled. Keep
				// pacing by sleeping to the cycle boundary.
				SLEEP_UNTIL_NSEC(pTalkerData->nextCycleNS);
#endif
			}

			if (pTalkerData->useWallTimePacing &&
					CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &wakeWallNs) &&
					wakeWallNs >= pTalkerData->nextCycleNS) {
				U64 wakeLateNs = wakeWallNs - pTalkerData->nextCycleNS;
				talkerWakeDiagUpdate(
					&pTalkerData->wakeLateMinNs,
					&pTalkerData->wakeLateMaxNs,
					&pTalkerData->wakeLateSumNs,
					pTalkerData->wakeDiagSamples,
					wakeLateNs);
				pTalkerData->wakeDiagSamples++;
				if (wakeLateNs > 0) {
					pTalkerData->wakeLateCount++;
				}
				if (wakeLateNs >= TALKER_WAKE_LATE_WARN_NS && pTalkerData->wakeDiagLogCount < 32) {
					AVB_LOGF_WARNING(
						"TALKER WAKE LATE " STREAMID_FORMAT " target=%llu wake=%llu late=%lluns interval=%lluns",
						STREAMID_ARGS(&pTalkerData->streamID),
						(unsigned long long)pTalkerData->nextCycleNS,
						(unsigned long long)wakeWallNs,
						(unsigned long long)wakeLateNs,
						(unsigned long long)pTalkerData->intervalNS);
					pTalkerData->wakeDiagLogCount++;
				}
				talkerMaybeLogWakeDiag(pTalkerData);
			}

			//AVB_DBG_INTERVAL(8000, TRUE);

			// send the frames for this interval
			int i;
			for (i = pTalkerData->wakeFrames; i > 0; i--) {
				// Keep going until the final iteration so pending batched frames
				// are always given a chance to flush on i == 1.
				if (IS_OPENAVB_SUCCESS(openavbAvtpTx(pTalkerData->avtpHandle, i == 1, pCfg->tx_blocking_in_intf)))
					pTalkerData->cntFrames++;
				else if (i == 1)
					break;
			}
		}
		else {
			// Interface module block option
			if (IS_OPENAVB_SUCCESS(openavbAvtpTx(pTalkerData->avtpHandle, TRUE, pCfg->tx_blocking_in_intf)))
				pTalkerData->cntFrames++;
		}

		if (pTalkerData->useWallTimePacing) {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNS);
		}
		else if (!usedBusySpin) {
			CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNS);
		} else {
			CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNS);
		}

		CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &housekeepingNowNS);

		if (pTalkerData->cntWakes++ % pTalkerData->wakeRate == 0) {
			// time to service the endpoint IPC
			bRet = TRUE;

			// Don't need to check again for another second.
			pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
		}

		if (pCfg->report_seconds > 0) {
			if (housekeepingNowNS > pTalkerData->nextReportNS) {
				talkerShowStats(pTalkerData, pTLState);
			  
				openavbTalkerAddStat(pTLState, TL_STAT_TX_CALLS, pTalkerData->cntWakes);
				openavbTalkerAddStat(pTLState, TL_STAT_TX_FRAMES, pTalkerData->cntFrames);

				pTalkerData->cntFrames = 0;
				pTalkerData->cntWakes = 0;
				pTalkerData->nextReportNS = housekeepingNowNS + (pCfg->report_seconds * NANOSECONDS_PER_SECOND);
			}
		} else if (pCfg->report_frames > 0 && pTalkerData->cntFrames != pTalkerData->lastReportFrames) {
			if (pTalkerData->cntFrames % pCfg->report_frames == 1) {
				talkerShowStats(pTalkerData, pTLState);
				pTalkerData->lastReportFrames = pTalkerData->cntFrames;
			}
		}

		if (housekeepingNowNS > pTalkerData->nextSecondNS) {
			pTalkerData->nextSecondNS = housekeepingNowNS + NANOSECONDS_PER_SECOND;
			bRet = TRUE;
		}

		if (!pCfg->tx_blocking_in_intf) {
			pTalkerData->nextCycleNS += pTalkerData->intervalNS;

			if ((pTalkerData->nextCycleNS + (pCfg->max_transmit_deficit_usec * 1000)) < nowNS) {
				// Hit max deficit time. Something must be wrong. Reset the cycle timer.	
				// Align clock : allows for some performance gain
				nowNS = ((nowNS + (pTalkerData->intervalNS)) / pTalkerData->intervalNS) * pTalkerData->intervalNS;
				pTalkerData->nextCycleNS = nowNS + pTalkerData->intervalNS;
			}				
		}
	}
	else {
		SLEEP_MSEC(10);

		// time to service the endpoint IPC
		bRet = TRUE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_TL);
	return bRet;
}


// Called from openavbTLThreadFn() which is started from openavbTLRun() 
void openavbTLRunTalker(tl_state_t *pTLState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	pTLState->pPvtTalkerData = calloc(1, sizeof(talker_data_t));
	if (!pTLState->pPvtTalkerData) {
		AVB_LOG_WARNING("Failed to allocate talker data.");
		return;
	}

	// Create Stats Mutex
	{
		MUTEX_ATTR_HANDLE(mta);
		MUTEX_ATTR_INIT(mta);
		MUTEX_ATTR_SET_TYPE(mta, MUTEX_ATTR_TYPE_DEFAULT);
		MUTEX_ATTR_SET_NAME(mta, "TLStatsMutex");
		MUTEX_CREATE_ERR();
		MUTEX_CREATE(pTLState->statsMutex, mta);
		MUTEX_LOG_ERR("Could not create/initialize 'TLStatsMutex' mutex");
	}

	/* If using endpoint register talker,
	   else register with tpsec */
	pTLState->bConnected = openavbTLRunTalkerInit(pTLState); 

	if (pTLState->bConnected) {
		bool bServiceIPC;

		// Notify AVDECC Msg of the state change.
		openavbAvdeccMsgClntNotifyCurrentState(pTLState);

		// Do until we are stopped or lose connection to endpoint
		while (pTLState->bRunning && pTLState->bConnected) {

			// Talk (or just sleep if not streaming.)
			bServiceIPC = talkerDoStream(pTLState);

			// TalkerDoStream() returns TRUE occasionally,
			// so that we can service our IPC at that low rate.
			if (bServiceIPC) {
				// Look for messages from endpoint.  Don't block (timeout=0)
				if (!openavbEptClntService(pTLState->endpointHandle, 0)) {
					AVB_LOGF_WARNING("Lost connection to endpoint, will retry "STREAMID_FORMAT, STREAMID_ARGS(&(((talker_data_t *)pTLState->pPvtTalkerData)->streamID)));
					pTLState->bConnected = FALSE;
					pTLState->endpointHandle = 0;
				}
			}
		}

		// Stop streaming
		talkerStopStream(pTLState);

		{
			MUTEX_CREATE_ERR();
			MUTEX_DESTROY(pTLState->statsMutex); // Destroy Stats Mutex
			MUTEX_LOG_ERR("Error destroying mutex");
		}

		// withdraw our talker registration
		if (pTLState->bConnected)
			openavbEptClntStopStream(pTLState->endpointHandle, &(((talker_data_t *)pTLState->pPvtTalkerData)->streamID));

		openavbTLRunTalkerFinish(pTLState);

		// Notify AVDECC Msg of the state change.
		openavbAvdeccMsgClntNotifyCurrentState(pTLState);
	}
	else {
		AVB_LOGF_WARNING("Failed to connect to endpoint"STREAMID_FORMAT, STREAMID_ARGS(&(((talker_data_t *)pTLState->pPvtTalkerData)->streamID)));
	}

	if (pTLState->pPvtTalkerData) {
		free(pTLState->pPvtTalkerData);
		pTLState->pPvtTalkerData = NULL;
	}

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

void openavbTLPauseTalker(tl_state_t *pTLState, bool bPause)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	if (!pTalkerData) {
		AVB_LOG_ERROR("Invalid private talker data");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	pTLState->bPaused = bPause;
	openavbAvtpPause(pTalkerData->avtpHandle, bPause);

	// Notify AVDECC Msg of the state change.
	openavbAvdeccMsgClntNotifyCurrentState(pTLState);

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

void openavbTalkerClearStats(tl_state_t *pTLState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	if (!pTalkerData) {
		AVB_LOG_ERROR("Invalid private talker data");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	LOCK_STATS();
	memset(&pTalkerData->stats, 0, sizeof(pTalkerData->stats));
	UNLOCK_STATS();

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

void openavbTalkerAddStat(tl_state_t *pTLState, tl_stat_t stat, U64 val)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	if (!pTalkerData) {
		AVB_LOG_ERROR("Invalid private talker data");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	LOCK_STATS();
	switch (stat) {
		case TL_STAT_TX_CALLS:
			pTalkerData->stats.totalCalls += val;
			break;
		case TL_STAT_TX_FRAMES:
			pTalkerData->stats.totalFrames += val;
			break;
		case TL_STAT_TX_LATE:
			pTalkerData->stats.totalLate += val;
			break;
		case TL_STAT_TX_BYTES:
			pTalkerData->stats.totalBytes += val;
			break;
		case TL_STAT_RX_CALLS:
		case TL_STAT_RX_FRAMES:
		case TL_STAT_RX_LOST:
		case TL_STAT_RX_BYTES:
			break;
	}
	UNLOCK_STATS();

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

U64 openavbTalkerGetStat(tl_state_t *pTLState, tl_stat_t stat)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);
	U64 val = 0;

	if (!pTLState) {
		AVB_LOG_ERROR("Invalid TLState");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return 0;
	}

	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	if (!pTalkerData) {
		AVB_LOG_ERROR("Invalid private talker data");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return 0;
	}

	LOCK_STATS();
	switch (stat) {
		case TL_STAT_TX_CALLS:
			val = pTalkerData->stats.totalCalls;
			break;
		case TL_STAT_TX_FRAMES:
			val = pTalkerData->stats.totalFrames;
			break;
		case TL_STAT_TX_LATE:
			val = pTalkerData->stats.totalLate;
			break;
		case TL_STAT_TX_BYTES:
			val = pTalkerData->stats.totalBytes;
			break;
		case TL_STAT_RX_CALLS:
		case TL_STAT_RX_FRAMES:
		case TL_STAT_RX_LOST:
		case TL_STAT_RX_BYTES:
			break;
	}
	UNLOCK_STATS();

	AVB_TRACE_EXIT(AVB_TRACE_TL);
	return val;
}
