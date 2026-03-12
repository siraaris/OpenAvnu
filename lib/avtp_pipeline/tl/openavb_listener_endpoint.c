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
* MODULE SUMMARY : Listener implementation
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "openavb_platform.h"
#include "openavb_trace.h"
#include "openavb_tl.h"
#include "openavb_endpoint.h"
#include "openavb_avtp.h"
#include "openavb_listener.h"
#include "openavb_avdecc_msg.h"

// DEBUG Uncomment to turn on logging for just this module.
//#define AVB_LOG_ON	1

#define	AVB_LOG_COMPONENT	"Listener"
#include "openavb_log.h"

/* Listener callback comes from endpoint, to indicate when talkers
 * come and go. We may need to start or stop the listener thread.
 */
void openavbEptClntNotifyLstnrOfSrpCb(int endpointHandle,
	AVBStreamID_t 	*streamID,
	char 			*ifname,
	U8 			destAddr[],
	openavbSrpAttribType_t tlkrDecl,
	AVBTSpec_t		*tSpec,
	U8				srClassID,
	U32			latency,
	openavbSrpFailInfo_t *failInfo)
{
	AVB_TRACE_ENTRY(AVB_TRACE_TL);

	static const U8 emptyMAC[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
	tl_state_t *pTLState = TLHandleListGet(endpointHandle);

	if (!pTLState) {
		AVB_LOG_WARNING("Unable to get listener from endpoint handle.");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}
	openavb_tl_cfg_t *pCfg = &pTLState->cfg;
	listener_data_t *pListenerData = pTLState->pPvtListenerData;
	if (!pListenerData) {
		AVB_LOG_WARNING("Listener private data unavailable for endpoint callback.");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	// If not a listener, ignore this callback.
	if (pTLState->cfg.role != AVB_ROLE_LISTENER) {
		AVB_LOG_DEBUG("Ignoring Listener callback");
		AVB_TRACE_EXIT(AVB_TRACE_TL);
		return;
	}

	AVB_LOGF_DEBUG("%s streaming=%d, tlkrDecl=%d", __FUNCTION__, pTLState->bStreaming, tlkrDecl);
	AVB_LOGF_INFO("Listener SRP callback: stream=" STREAMID_FORMAT " tlkrDecl=0x%02x dest=" ETH_FORMAT " class=%u max_interval=%u max_frame=%u latency=%u",
		STREAMID_ARGS(streamID),
		(U8)tlkrDecl,
		ETH_OCTETS(destAddr),
		srClassID,
		tSpec ? tSpec->maxIntervalFrames : 0,
		tSpec ? tSpec->maxFrameSize : 0,
		latency);

	// Publish SRP declaration/failure details to AVDECC only when the value changes.
	// This keeps MSRP flags information current without continuously pushing duplicates.
	U8 failureBridgeId[8] = {0};
	U8 failureCode = 0;
	if (failInfo) {
		memcpy(failureBridgeId, failInfo->BridgeID, sizeof(failureBridgeId));
		failureCode = failInfo->FailureCode;
	}
	bool srpInfoChanged =
		(!pListenerData->srpInfoPublished) ||
		(pListenerData->srpTalkerDecl != (U8)tlkrDecl) ||
		(pListenerData->srpFailureCode != failureCode) ||
		(memcmp(pListenerData->srpFailureBridgeId, failureBridgeId, sizeof(failureBridgeId)) != 0);
	if (srpInfoChanged &&
			pTLState->bAvdeccMsgRunning &&
			pTLState->avdeccMsgHandle != AVB_AVDECC_MSG_HANDLE_INVALID) {
		if (openavbAvdeccMsgClntListenerSrpInfo(
				pTLState->avdeccMsgHandle,
				(U8)tlkrDecl,
				failureCode,
				failureBridgeId)) {
			pListenerData->srpInfoPublished = TRUE;
			pListenerData->srpTalkerDecl = (U8)tlkrDecl;
			pListenerData->srpFailureCode = failureCode;
			memcpy(pListenerData->srpFailureBridgeId, failureBridgeId, sizeof(failureBridgeId));
		}
		else {
			AVB_LOGF_WARNING("Failed to publish listener SRP info to AVDECC (decl=0x%02x)", tlkrDecl);
		}
	}

	if (!pTLState->bStreaming
		&& tlkrDecl == openavbSrp_AtTyp_TalkerAdvertise) {
		// 	if(x_cfg.noSrp) this is sort of a recursive call into openavbEptClntAttachStream()
		// but we are OK due to the intervening IPC.
		bool rc = openavbEptClntAttachStream(pTLState->endpointHandle, streamID, openavbSrp_LDSt_Ready);
		if (rc) {
			// Save data provided by endpoint/SRP
			if (!pCfg->ifname[0]) {
				strncpy(pListenerData->ifname, ifname, sizeof(pListenerData->ifname) - 1);
			} else {
				strncpy(pListenerData->ifname, pCfg->ifname, sizeof(pListenerData->ifname) - 1);
			}
			memcpy(&pListenerData->streamID, streamID, sizeof(AVBStreamID_t));
			if (memcmp(destAddr, emptyMAC, ETH_ALEN) != 0) {
				memcpy(&pListenerData->destAddr, destAddr, ETH_ALEN);
				memcpy(&pListenerData->tSpec, tSpec, sizeof(AVBTSpec_t));
			}
			else {
				// manual stream configuration required to be obtained from config file;
				// see comments at call to strmRegCb() in openavbEptClntAttachStream() in openavb_endpoint.c
				AVB_LOG_INFO("Endpoint Configuration requires manual stream configuration on listener");
				if ((!pCfg->dest_addr.mac) || memcmp(&(pCfg->dest_addr.mac->ether_addr_octet[0]), &(emptyMAC[0]), ETH_ALEN) == 0) {
					AVB_LOG_ERROR("  Configuration Error - dest_addr required in listener config file");
				}
				else {
					memcpy(&pListenerData->destAddr, &pCfg->dest_addr.mac->ether_addr_octet, ETH_ALEN);
					AVB_LOGF_INFO("  Listener configured dest_addr is " ETH_FORMAT,
						ETH_OCTETS(pListenerData->destAddr));
				}
				if ((!pCfg->max_interval_frames) || (!pCfg->max_frame_size)) {
					AVB_LOG_ERROR("  Configuration Error - both max_interval_frames and max_frame_size required in listener config file");
				}
				else {
					pListenerData->tSpec.maxIntervalFrames = pCfg->max_interval_frames;
					pListenerData->tSpec.maxFrameSize      = pCfg->max_frame_size;
					AVB_LOGF_INFO("  Listener configured max_interval_frames = %u, max_frame_size = %u",
						pListenerData->tSpec.maxIntervalFrames, pListenerData->tSpec.maxFrameSize);
				}
			}

			// We should start streaming
			AVB_LOGF_INFO("Starting stream: "STREAMID_FORMAT " dest=" ETH_FORMAT,
				STREAMID_ARGS(streamID),
				ETH_OCTETS(pListenerData->destAddr));
			listenerStartStream(pTLState);
		}
		else {
			AVB_LOG_DEBUG("Failed to attach listener stream");
		}
	}
	else if (pTLState->bStreaming
		&& tlkrDecl != openavbSrp_AtTyp_TalkerAdvertise) {
		AVB_LOGF_INFO("Stopping stream: "STREAMID_FORMAT, STREAMID_ARGS(streamID));
		pListenerData->aecpCounters.stream_interrupted++;
		listenerStopStream(pTLState);

		// We're still interested in the stream
		openavbEptClntAttachStream(pTLState->endpointHandle, streamID, openavbSrp_LDSt_Interest);

		// Notify AVDECC that fast connect is desired.
		if (pTLState->bAvdeccMsgRunning) {
			openavbAvdeccMsgClntChangeNotification(pTLState->avdeccMsgHandle, OPENAVB_AVDECC_MSG_STOPPED_UNEXPECTEDLY);
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_TL);
}

bool openavbTLRunListenerInit(int h, AVBStreamID_t *streamID)
{
	return(openavbEptClntAttachStream(h, streamID, openavbSrp_LDSt_Interest));
}
