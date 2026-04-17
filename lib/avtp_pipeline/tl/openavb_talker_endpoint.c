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
* MODULE SUMMARY : Talker implementation for use with endpoint
*/

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "openavb_platform.h"
#include "openavb_trace.h"
#include "openavb_tl.h"
#include "openavb_endpoint.h"
#include "openavb_avtp.h"
#include "openavb_talker.h"
#include "openavb_time.h"
#include "openavb_avdecc_msg.h"

// DEBUG Uncomment to turn on logging for just this module.
//#define AVB_LOG_ON	1

#define	AVB_LOG_COMPONENT	"Talker"
#include "openavb_log.h"

typedef struct {
	pthread_mutex_t lock;
	U32 readyMask;
	tl_state_t *readyStates[4];
} direct_ready_group_t;

typedef struct {
	tl_state_t *pTLState;
	U32 uid;
	bool started;
} direct_ready_start_ctx_t;

static direct_ready_group_t sDirectReadyGroup = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.readyMask = 0,
	.readyStates = { NULL, NULL, NULL, NULL },
};

static void *xStartDirectReadyGroupThread(void *pv)
{
	direct_ready_start_ctx_t *pCtx = (direct_ready_start_ctx_t *)pv;

	if (pCtx && pCtx->pTLState) {
		AVB_LOGF_INFO("Starting grouped direct stream: uid=%u", pCtx->uid);
		pCtx->started = talkerStartStream(pCtx->pTLState);
	}

	return NULL;
}

static void updateTalkerStreamParamsFromEndpoint(openavb_tl_cfg_t *pCfg,
		talker_data_t *pTalkerData,
		AVBStreamID_t *streamID,
		char *ifname,
		U8 destAddr[],
		U8 srClass,
		U32 classRate,
		U16 vlanID,
		U8 priority,
		U16 fwmark)
{
	// Save endpoint/SRP values for active streaming use.
	if (!pCfg->ifname[0]) {
		strncpy(pTalkerData->ifname, ifname, sizeof(pTalkerData->ifname) - 1);
	}
	else {
		strncpy(pTalkerData->ifname, pCfg->ifname, sizeof(pTalkerData->ifname) - 1);
	}
	memcpy(&pTalkerData->streamID, streamID, sizeof(AVBStreamID_t));
	memcpy(&pTalkerData->destAddr, destAddr, ETH_ALEN);
	pTalkerData->srClass = srClass;
	pTalkerData->classRate = classRate;
	pTalkerData->vlanID = vlanID;
	pTalkerData->vlanPCP = priority;
	pTalkerData->fwmark = fwmark;

	// Mirror into cfg as well so descriptor/AECP reads and AVDECC reconnect
	// always use the latest endpoint-assigned stream identity and destination.
	memcpy(pCfg->stream_addr.buffer.ether_addr_octet, streamID->addr, ETH_ALEN);
	pCfg->stream_addr.mac = &(pCfg->stream_addr.buffer);
	pCfg->stream_uid = streamID->uniqueID;
	memcpy(pCfg->dest_addr.buffer.ether_addr_octet, destAddr, ETH_ALEN);
	pCfg->dest_addr.mac = &(pCfg->dest_addr.buffer);
	pCfg->sr_class = srClass;
	pCfg->vlan_id = vlanID;
}

static bool xUseDirectReadyBarrier(tl_state_t *pTLState, const AVBStreamID_t *streamID)
{
	openavb_tl_cfg_t *pCfg;

	if (!pTLState || !streamID) {
		return FALSE;
	}

	if (pTLState->cfg.role != AVB_ROLE_TALKER) {
		return FALSE;
	}

	pCfg = &pTLState->cfg;
	if (!pCfg->intf_cb.intf_get_tx_start_cycle_cb) {
		return FALSE;
	}

	return (streamID->uniqueID < 4u);
}

static void xClearDirectReadyBarrierSlot(const AVBStreamID_t *streamID, tl_state_t *pTLState)
{
	U32 uid;
	U32 mask;

	if (!xUseDirectReadyBarrier(pTLState, streamID)) {
		return;
	}

	uid = (U32)streamID->uniqueID;
	mask = (1u << uid);

	pthread_mutex_lock(&sDirectReadyGroup.lock);
	if (sDirectReadyGroup.readyStates[uid] == pTLState) {
		sDirectReadyGroup.readyStates[uid] = NULL;
		sDirectReadyGroup.readyMask &= ~mask;
	}
	pthread_mutex_unlock(&sDirectReadyGroup.lock);
}

static bool xStartDirectReadyBarrierGroup(const AVBStreamID_t *streamID, tl_state_t *pTLState)
{
	tl_state_t *startStates[4] = { NULL, NULL, NULL, NULL };
	direct_ready_start_ctx_t startCtx[4];
	pthread_t startThreads[4];
	bool threadStarted[4] = { FALSE, FALSE, FALSE, FALSE };
	U32 uid;
	U32 mask;
	U32 startMask = 0;
	U32 i;

	if (!xUseDirectReadyBarrier(pTLState, streamID)) {
		return FALSE;
	}

	uid = (U32)streamID->uniqueID;
	mask = (1u << uid);

	pthread_mutex_lock(&sDirectReadyGroup.lock);
	sDirectReadyGroup.readyStates[uid] = pTLState;
	sDirectReadyGroup.readyMask |= mask;
	AVB_LOGF_WARNING("Direct talker ready barrier: uid=%u ready_mask=0x%x expected=0x0f",
		uid,
		sDirectReadyGroup.readyMask);

	if ((sDirectReadyGroup.readyMask & 0x0f) == 0x0f) {
		startMask = sDirectReadyGroup.readyMask & 0x0f;
		for (i = 0; i < 4; i++) {
			startStates[i] = sDirectReadyGroup.readyStates[i];
			sDirectReadyGroup.readyStates[i] = NULL;
		}
		sDirectReadyGroup.readyMask = 0;
	}
	pthread_mutex_unlock(&sDirectReadyGroup.lock);

	if (startMask == 0) {
		return TRUE;
	}

	AVB_LOGF_WARNING("Direct talker ready barrier release: start_mask=0x%x", startMask);
	for (i = 0; i < 4; i++) {
		if (!startStates[i] || startStates[i]->bStreaming) {
			continue;
		}
		startCtx[i].pTLState = startStates[i];
		startCtx[i].uid = i;
		startCtx[i].started = FALSE;
	}

	for (i = 0; i < 4; i++) {
		if (!startCtx[i].pTLState) {
			continue;
		}
		if (pthread_create(&startThreads[i], NULL, xStartDirectReadyGroupThread, &startCtx[i]) == 0) {
			threadStarted[i] = TRUE;
		}
		else {
			AVB_LOGF_WARNING("Direct talker ready barrier thread create failed for uid=%u; starting inline", i);
			AVB_LOGF_INFO("Starting grouped direct stream: uid=%u", i);
			startCtx[i].started = talkerStartStream(startCtx[i].pTLState);
		}
	}

	for (i = 0; i < 4; i++) {
		if (threadStarted[i]) {
			pthread_join(startThreads[i], NULL);
		}
		if (startCtx[i].pTLState && !startCtx[i].started) {
			AVB_LOGF_WARNING("Grouped direct stream failed to start: uid=%u", i);
		}
	}

	return TRUE;
}

/* Talker callback comes from endpoint, to indicate when listeners
 * come and go. We may need to start or stop the talker thread.
 */
void openavbEptClntNotifyTlkrOfSrpCb(int                      endpointHandle,
                                 AVBStreamID_t           *streamID,
                                 char                    *ifname,
                                 U8                       destAddr[],
                                 openavbSrpLsnrDeclSubtype_t  lsnrDecl,
                                 U8                       srClass,
                                 U32                      classRate,
                                 U16                      vlanID,
                                 U8                       priority,
                                 U16                      fwmark)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	tl_state_t *pTLState = TLHandleListGet(endpointHandle);
	if (!pTLState) {
		AVB_LOG_WARNING("Unable to get talker from endpoint handle.");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}
	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;
	if (!pTalkerData) {
		AVB_LOG_WARNING("Talker private data unavailable for endpoint callback.");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	// If not a talker, ignore this callback.
	if (pTLState->cfg.role != AVB_ROLE_TALKER) {
		AVB_LOG_DEBUG("Ignoring Talker callback");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	AVB_LOGF_DEBUG("%s streaming=%d, lsnrDecl=%d", __FUNCTION__, pTLState->bStreaming, lsnrDecl);
	AVB_LOGF_WARNING("Talker endpoint callback: uid=%u decl=%d streaming=%d dest=" ETH_FORMAT " class=%c rate=%u vlan=%u pcp=%u fwmark=%u",
		streamID->uniqueID,
		lsnrDecl,
		pTLState->bStreaming ? 1 : 0,
		ETH_OCTETS(destAddr),
		AVB_CLASS_LABEL(srClass),
		classRate,
		vlanID,
		priority,
		fwmark);

	openavb_tl_cfg_t *pCfg = &pTLState->cfg;

	if (!pTLState->bStreaming) {
		if (lsnrDecl == openavbSrp_LDSt_Ready
			|| lsnrDecl == openavbSrp_LDSt_Ready_Failed) {

			updateTalkerStreamParamsFromEndpoint(
				pCfg, pTalkerData, streamID, ifname, destAddr, srClass, classRate, vlanID, priority, fwmark);
			AVB_LOGF_INFO("Talker stream params: uid=%u class=%c classRate=%u vlanID=%u pcp=%u fwmark=%u",
				streamID->uniqueID, AVB_CLASS_LABEL(srClass), classRate, vlanID, priority, fwmark);

			if (!xStartDirectReadyBarrierGroup(streamID, pTLState)) {
				// We should start streaming
				AVB_LOGF_INFO("Starting stream: "STREAMID_FORMAT, STREAMID_ARGS(streamID));
				talkerStartStream(pTLState);
			}
		}
		else if (lsnrDecl == openavbSrp_LDSt_Stream_Info) {
			// Stream information is available does NOT mean listener is ready. Stream not started yet.
			updateTalkerStreamParamsFromEndpoint(
				pCfg, pTalkerData, streamID, ifname, destAddr, srClass, classRate, vlanID, priority, fwmark);
			AVB_LOGF_INFO("Talker stream info: uid=%u class=%c classRate=%u vlanID=%u pcp=%u fwmark=%u",
				streamID->uniqueID, AVB_CLASS_LABEL(srClass), classRate, vlanID, priority, fwmark);
		}
	}
	else {
		if (lsnrDecl == openavbSrp_LDSt_None
			|| lsnrDecl == openavbSrp_LDSt_Ignore) {
			xClearDirectReadyBarrierSlot(streamID, pTLState);
			// Nobody is listening any more
			AVB_LOGF_INFO("Stopping stream: "STREAMID_FORMAT, STREAMID_ARGS(streamID));
			talkerStopStream(pTLState);
		}
	}

	// Let the AVDECC Msg server know our current stream ID, in case it was updated by MAAP.
	if (pTLState->avdeccMsgHandle != AVB_AVDECC_MSG_HANDLE_INVALID) {
		if (!openavbAvdeccMsgClntTalkerStreamID(pTLState->avdeccMsgHandle,
				pTalkerData->srClass, pTalkerData->streamID.addr, pTalkerData->streamID.uniqueID,
				pTalkerData->destAddr, pTalkerData->vlanID)) {
			AVB_LOG_ERROR("openavbAvdeccMsgClntTalkerStreamID() failed");
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

bool openavbTLRunTalkerInit(tl_state_t *pTLState)
{
	openavb_tl_cfg_t *pCfg = &pTLState->cfg;
	talker_data_t *pTalkerData = pTLState->pPvtTalkerData;

	AVBStreamID_t streamID;
	memset(&streamID, 0, sizeof(AVBStreamID_t));
	if (pCfg->stream_addr.mac)
		memcpy(streamID.addr, pCfg->stream_addr.mac, ETH_ALEN);
	streamID.uniqueID = pCfg->stream_uid;

	unsigned int maxBitrate = 0;
	if (pCfg->intf_cb.intf_get_src_bitrate_cb != NULL) {
		maxBitrate = pCfg->intf_cb.intf_get_src_bitrate_cb(pTLState->pMediaQ);
	}
	if (maxBitrate > 0) {
		if (pCfg->map_cb.map_set_src_bitrate_cb != NULL) {
			pCfg->map_cb.map_set_src_bitrate_cb(pTLState->pMediaQ, maxBitrate);
		}

		if (pCfg->map_cb.map_get_max_interval_frames_cb != NULL) {
			unsigned int map_intv_frames = pCfg->map_cb.map_get_max_interval_frames_cb(pTLState->pMediaQ, pTLState->cfg.sr_class);
			pCfg->max_interval_frames = map_intv_frames > 0 ? map_intv_frames : pCfg->max_interval_frames;
		}
	}
	pTalkerData->tSpec.maxIntervalFrames = pCfg->max_interval_frames;

	// The TSpec frame size is the L2 payload - i.e. no Ethernet headers, VLAN, FCS, etc...
	pTalkerData->tSpec.maxFrameSize = pCfg->map_cb.map_max_data_size_cb(pTLState->pMediaQ);

	AVB_LOGF_INFO("Register "STREAMID_FORMAT": class: %c frame size: %d  frame interval: %d", STREAMID_ARGS(&streamID), AVB_CLASS_LABEL(pCfg->sr_class), pTalkerData->tSpec.maxFrameSize, pTalkerData->tSpec.maxIntervalFrames);

	// Tell endpoint to register our stream.
	// SRP will send out talker declarations on the LAN.
	// If there are listeners, we'll get callback (above.)
	U32 transmitInterval = pTalkerData->classRate;
	if (pCfg->map_cb.map_transmit_interval_cb(pTLState->pMediaQ)) {
		// Override the class observation interval with the one provided by the mapping module.
		transmitInterval = pCfg->map_cb.map_transmit_interval_cb(pTLState->pMediaQ);
	}
	return (openavbEptClntRegisterStream(pTLState->endpointHandle,
			&streamID,
			pCfg->dest_addr.mac->ether_addr_octet,
			pCfg->backup_dest_addr_valid, // If we have a backup dest_addr, then the current one was forced and MAAP should not be used.
			&pTalkerData->tSpec,
			pCfg->sr_class,
			pCfg->sr_rank,
			pCfg->internal_latency,
			transmitInterval));
}

void openavbTLRunTalkerFinish(tl_state_t *pTLState)
{
}
