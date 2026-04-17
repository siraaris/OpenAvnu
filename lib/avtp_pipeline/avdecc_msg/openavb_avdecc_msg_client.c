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
* MODULE SUMMARY :
*
* Stream clients (talkers or listeners) must connect to the central
* "avdecc_msg" process to support AVDECC control.
* This code provides the means for them to do so.
*
* It provides proxy functions for the process to call.  The arguments
* for those calls are packed into messages, which are unpacked in the
* avdecc_msg process and then used to call the real functions.
*
* Current IPC uses unix sockets.  Can change this by creating a new
* implementations in openavb_avdecc_msg_client.c and openavb_avdecc_msg_server.c
*/

#include <stdlib.h>
#include <string.h>

#include "openavb_platform.h"

#define	AVB_LOG_COMPONENT	"AVDECC Msg"
#include "openavb_pub.h"
#include "openavb_log.h"

#include "openavb_trace.h"
#include "openavb_avdecc_msg_client.h"
#include "openavb_tl.h"
#include "openavb_talker.h"
#include "openavb_listener.h"
#include "openavb_avtp.h"
#include "openavb_clock_source_runtime_pub.h"

// forward declarations
static bool openavbAvdeccMsgClntReceiveFromServer(int avdeccMsgHandle, openavbAvdeccMessage_t *msg);
static avdecc_msg_state_t *openavbAvdeccMsgClntGetStateForTL(tl_state_t *pTLState);
static bool openavbAvdeccMsgClntGetCurrentCounters(tl_state_t *pTLState, openavb_avtp_diag_counters_t *pCounters);

// OSAL specific functions
#include "openavb_avdecc_msg_client_osal.c"

// AvdeccMsgStateListGet() support.
#include "openavb_avdecc_msg.c"

static bool openavbAvdeccMsgClntReceiveFromServer(int avdeccMsgHandle, openavbAvdeccMessage_t *msg)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	if (!msg || avdeccMsgHandle == AVB_AVDECC_MSG_HANDLE_INVALID) {
		AVB_LOG_ERROR("Client receive; invalid argument passed");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return FALSE;
	}

	switch(msg->type) {
		case OPENAVB_AVDECC_MSG_VERSION_CALLBACK:
			AVB_LOG_DEBUG("Message received:  OPENAVB_AVDECC_MSG_VERSION_CALLBACK");
			openavbAvdeccMsgClntCheckVerMatchesSrvr(avdeccMsgHandle, ntohl(msg->params.versionCallback.AVBVersion));
			break;
		case OPENAVB_AVDECC_MSG_LISTENER_STREAM_ID:
			AVB_LOG_DEBUG("Message received:  OPENAVB_AVDECC_MSG_LISTENER_STREAM_ID");
			openavbAvdeccMsgClntHndlListenerStreamIDFromServer(avdeccMsgHandle,
				msg->params.listenerStreamID.sr_class, msg->params.listenerStreamID.stream_src_mac, ntohs(msg->params.listenerStreamID.stream_uid),
				msg->params.listenerStreamID.stream_dest_mac, ntohs(msg->params.listenerStreamID.stream_vlan_id));
			break;
		case OPENAVB_AVDECC_MSG_S2C_TALKER_STREAM_ID:
			AVB_LOG_DEBUG("Message received:  OPENAVB_AVDECC_MSG_S2C_TALKER_STREAM_ID");
			openavbAvdeccMsgClntHndlTalkerStreamIDFromServer(avdeccMsgHandle,
				msg->params.s2cTalkerStreamID.sr_class,
				msg->params.s2cTalkerStreamID.stream_id_valid,
				msg->params.s2cTalkerStreamID.stream_src_mac,
				ntohs(msg->params.s2cTalkerStreamID.stream_uid),
				msg->params.s2cTalkerStreamID.stream_dest_valid,
				msg->params.s2cTalkerStreamID.stream_dest_mac,
				msg->params.s2cTalkerStreamID.stream_vlan_id_valid,
				ntohs(msg->params.s2cTalkerStreamID.stream_vlan_id));
			break;
		case OPENAVB_AVDECC_MSG_CLIENT_CHANGE_REQUEST:
			AVB_LOG_DEBUG("Message received:  OPENAVB_AVDECC_MSG_CLIENT_CHANGE_REQUEST");
			openavbAvdeccMsgClntHndlChangeRequestFromServer(avdeccMsgHandle, (openavbAvdeccMsgStateType_t) msg->params.clientChangeRequest.desired_state);
			break;
		case OPENAVB_AVDECC_MSG_CLOCK_SOURCE_UPDATE:
			AVB_LOG_DEBUG("Message received:  OPENAVB_AVDECC_MSG_CLOCK_SOURCE_UPDATE");
			openavbAvdeccMsgClntHndlClockSourceUpdateFromServer(
				avdeccMsgHandle,
				ntohs(msg->params.clockSourceUpdate.clock_domain_index),
				ntohs(msg->params.clockSourceUpdate.clock_source_index),
				ntohs(msg->params.clockSourceUpdate.clock_source_flags),
				ntohs(msg->params.clockSourceUpdate.clock_source_type),
				ntohs(msg->params.clockSourceUpdate.clock_source_location_type),
				ntohs(msg->params.clockSourceUpdate.clock_source_location_index));
			break;
		default:
			AVB_LOG_ERROR("Client receive: unexpected message");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
			return FALSE;
	}
	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return TRUE;
}

static avdecc_msg_state_t *openavbAvdeccMsgClntGetStateForTL(tl_state_t *pTLState)
{
	int i;

	if (!pTLState) {
		return NULL;
	}

	for (i = 0; i < MAX_AVDECC_MSG_CLIENTS; ++i) {
		avdecc_msg_state_t *pAvdeccMsgState = AvdeccMsgStateListGetIndex(i);
		if (!pAvdeccMsgState) {
			break;
		}
		if (pAvdeccMsgState->pTLState == pTLState) {
			return pAvdeccMsgState;
		}
	}

	return NULL;
}

static bool openavbAvdeccMsgClntGetCurrentCounters(tl_state_t *pTLState, openavb_avtp_diag_counters_t *pCounters)
{
	if (!pTLState || !pCounters) {
		return false;
	}

	memset(pCounters, 0, sizeof(*pCounters));

	if (pTLState->cfg.role == AVB_ROLE_LISTENER) {
		listener_data_t *pListenerData = pTLState->pPvtListenerData;

		if (!pListenerData) {
			return true;
		}

		*pCounters = pListenerData->aecpCounters;
		{
			MUTEX_CREATE_ERR();
			MUTEX_LOCK(pListenerData->streamMutex);
			MUTEX_LOG_ERR("Mutex lock failure");
		}
		if (pListenerData->avtpHandle) {
			openavb_avtp_diag_counters_t liveCounters;
			openavbAvtpGetDiagCounters(pListenerData->avtpHandle, &liveCounters);
			openavbAvtpDiagCountersAccumulate(pCounters, &liveCounters);
		}
		{
			MUTEX_CREATE_ERR();
			MUTEX_UNLOCK(pListenerData->streamMutex);
			MUTEX_LOG_ERR("Mutex unlock failure");
		}
		return true;
	}

	if (pTLState->cfg.role == AVB_ROLE_TALKER) {
		talker_data_t *pTalkerData = pTLState->pPvtTalkerData;

		if (!pTalkerData) {
			return true;
		}

		*pCounters = pTalkerData->aecpCounters;
		if (pTalkerData->avtpHandle) {
			openavb_avtp_diag_counters_t liveCounters;
			openavbAvtpGetDiagCounters(pTalkerData->avtpHandle, &liveCounters);
			openavbAvtpDiagCountersAccumulate(pCounters, &liveCounters);
		}
		return true;
	}

	return false;
}

bool openavbAvdeccMsgClntRequestVersionFromServer(int avdeccMsgHandle)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	openavbAvdeccMessage_t msgBuf;

	memset(&msgBuf, 0, OPENAVB_AVDECC_MSG_LEN);
	msgBuf.type = OPENAVB_AVDECC_MSG_VERSION_REQUEST;
	bool ret = openavbAvdeccMsgClntSendToServer(avdeccMsgHandle, &msgBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

void openavbAvdeccMsgClntCheckVerMatchesSrvr(int avdeccMsgHandle, U32 AVBVersion)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	if (AVBVersion == AVB_CORE_VER_FULL) {
		pState->verState = OPENAVB_AVDECC_MSG_VER_VALID;
		AVB_LOG_DEBUG("AVDECC Versions Match");
	}
	else {
		pState->verState = OPENAVB_AVDECC_MSG_VER_INVALID;
		AVB_LOG_WARNING("AVDECC Versions Do Not Match");
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
}

bool openavbAvdeccMsgClntInitIdentify(int avdeccMsgHandle, const char * friendly_name, U8 talker)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	openavbAvdeccMessage_t msgBuf;

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	memset(&msgBuf, 0, OPENAVB_AVDECC_MSG_LEN);
	msgBuf.type = OPENAVB_AVDECC_MSG_CLIENT_INIT_IDENTIFY;
	openavbAvdeccMsgParams_ClientInitIdentify_t * pParams =
		&(msgBuf.params.clientInitIdentify);
	strncpy(pParams->friendly_name, friendly_name, sizeof(pParams->friendly_name));
	pParams->talker = talker;
	bool ret = openavbAvdeccMsgClntSendToServer(avdeccMsgHandle, &msgBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

bool openavbAvdeccMsgClntTalkerStreamID(int avdeccMsgHandle, U8 sr_class, const U8 stream_src_mac[6], U16 stream_uid, const U8 stream_dest_mac[6], U16 stream_vlan_id)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	openavbAvdeccMessage_t msgBuf;

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	// Send a default stream_vlan_id value if none specified.
	if (stream_vlan_id == 0) {
		stream_vlan_id = 2; // SR Class default VLAN Id values per IEEE 802.1Q-2011 Table 9-2
 	}

	// Send the stream information to the server.
	memset(&msgBuf, 0, OPENAVB_AVDECC_MSG_LEN);
	msgBuf.type = OPENAVB_AVDECC_MSG_C2S_TALKER_STREAM_ID;
	openavbAvdeccMsgParams_C2S_TalkerStreamID_t * pParams =
		&(msgBuf.params.c2sTalkerStreamID);
	pParams->sr_class = sr_class;
	memcpy(pParams->stream_src_mac, stream_src_mac, 6);
	pParams->stream_uid = htons(stream_uid);
	memcpy(pParams->stream_dest_mac, stream_dest_mac, 6);
	pParams->stream_vlan_id = htons(stream_vlan_id);
	bool ret = openavbAvdeccMsgClntSendToServer(avdeccMsgHandle, &msgBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

bool openavbAvdeccMsgClntListenerSrpInfo(int avdeccMsgHandle, U8 talker_decl, U8 msrp_failure_code, const U8 msrp_failure_bridge_id[8])
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	openavbAvdeccMessage_t msgBuf;

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	memset(&msgBuf, 0, OPENAVB_AVDECC_MSG_LEN);
	msgBuf.type = OPENAVB_AVDECC_MSG_C2S_LISTENER_SRP_INFO;
	openavbAvdeccMsgParams_C2S_ListenerSrpInfo_t *pParams =
		&(msgBuf.params.c2sListenerSrpInfo);
	pParams->talker_decl = talker_decl;
	pParams->msrp_failure_code = msrp_failure_code;
	if (msrp_failure_bridge_id != NULL) {
		memcpy(pParams->msrp_failure_bridge_id, msrp_failure_bridge_id, sizeof(pParams->msrp_failure_bridge_id));
	}
	bool ret = openavbAvdeccMsgClntSendToServer(avdeccMsgHandle, &msgBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

bool openavbAvdeccMsgClntHndlListenerStreamIDFromServer(int avdeccMsgHandle, U8 sr_class, const U8 stream_src_mac[6], U16 stream_uid, const U8 stream_dest_mac[6], U16 stream_vlan_id)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	bool restartAfterConfigChange = FALSE;
	bool pauseAfterRestart = FALSE;

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	if (!pState->pTLState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d state not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	AVB_LOGF_INFO("Listener stream update from AVDECC: handle=%d class=%c stream=" ETH_FORMAT "/%u dest=" ETH_FORMAT " vlan=%u",
		avdeccMsgHandle,
		AVB_CLASS_LABEL(sr_class),
		ETH_OCTETS(stream_src_mac),
		stream_uid,
		ETH_OCTETS(stream_dest_mac),
		stream_vlan_id);

	// Determine if the supplied information differs from the current settings.
	// If AVDECC supplies an unspecified value, keep current listener configuration.
	openavb_tl_cfg_t * pCfg = &(pState->pTLState->cfg);
	const U8 zeroMac[6] = {0};
	U8 effective_stream_src_mac[6];
	U16 effective_stream_uid;
	U8 effective_stream_dest_mac[6];
	U16 effective_stream_vlan_id;

	bool serverStreamIdUnspecified = (memcmp(stream_src_mac, zeroMac, 6) == 0 && stream_uid == 0);
	bool serverDestUnspecified = (memcmp(stream_dest_mac, zeroMac, 6) == 0);
	bool serverVlanUnspecified = (stream_vlan_id == 0);

	if (serverStreamIdUnspecified &&
		(memcmp(pCfg->stream_addr.buffer.ether_addr_octet, zeroMac, 6) != 0 || pCfg->stream_uid != 0)) {
		memcpy(effective_stream_src_mac, pCfg->stream_addr.buffer.ether_addr_octet, 6);
		effective_stream_uid = pCfg->stream_uid;
		AVB_LOGF_DEBUG("Ignoring unspecified AVDECC listener stream_id; keeping configured stream_id " ETH_FORMAT "/%u",
			ETH_OCTETS(effective_stream_src_mac), effective_stream_uid);
	}
	else {
		memcpy(effective_stream_src_mac, stream_src_mac, 6);
		effective_stream_uid = stream_uid;
	}

	if (serverDestUnspecified && memcmp(pCfg->dest_addr.buffer.ether_addr_octet, zeroMac, 6) != 0) {
		memcpy(effective_stream_dest_mac, pCfg->dest_addr.buffer.ether_addr_octet, 6);
	}
	else {
		memcpy(effective_stream_dest_mac, stream_dest_mac, 6);
	}

	if (serverVlanUnspecified && pCfg->vlan_id != 0) {
		effective_stream_vlan_id = pCfg->vlan_id;
	}
	else {
		effective_stream_vlan_id = stream_vlan_id;
	}

	if (pCfg->sr_class != sr_class ||
			memcmp(pCfg->stream_addr.buffer.ether_addr_octet, effective_stream_src_mac, 6) != 0 ||
			pCfg->stream_uid != effective_stream_uid ||
			memcmp(pCfg->dest_addr.buffer.ether_addr_octet, effective_stream_dest_mac, 6) != 0 ||
			pCfg->vlan_id != effective_stream_vlan_id) {
		// If the Listener is running, stop the Listener before updating the information.
		if (pState->pTLState->bRunning) {
			AVB_LOG_DEBUG("Forcing Listener to Stop to change streaming settings");
			restartAfterConfigChange = TRUE;
			pauseAfterRestart = pState->pTLState->bPaused;
			openavbAvdeccMsgClntHndlChangeRequestFromServer(avdeccMsgHandle, OPENAVB_AVDECC_MSG_STOPPED);
		}

		// Update the stream information supplied by the server.
		pCfg->sr_class = sr_class;
		memcpy(pCfg->stream_addr.buffer.ether_addr_octet, effective_stream_src_mac, 6);
		pCfg->stream_addr.mac = &(pCfg->stream_addr.buffer); // Indicate that the MAC Address is valid.
		pCfg->stream_uid = effective_stream_uid;
		memcpy(pCfg->dest_addr.buffer.ether_addr_octet, effective_stream_dest_mac, 6);
		pCfg->dest_addr.mac = &(pCfg->dest_addr.buffer); // Indicate that the MAC Address is valid.
		pCfg->vlan_id = effective_stream_vlan_id;
		AVB_LOGF_INFO("Listener stream config updated from AVDECC: class=%c stream=" ETH_FORMAT "/%u dest=" ETH_FORMAT " vlan=%u",
			AVB_CLASS_LABEL(pCfg->sr_class),
			ETH_OCTETS(pCfg->stream_addr.buffer.ether_addr_octet),
			pCfg->stream_uid,
			ETH_OCTETS(pCfg->dest_addr.buffer.ether_addr_octet),
			pCfg->vlan_id);
	}

	if (restartAfterConfigChange) {
		AVB_LOG_INFO("Restarting listener after AVDECC stream configuration update");
		openavbAvdeccMsgClntHndlChangeRequestFromServer(avdeccMsgHandle, OPENAVB_AVDECC_MSG_RUNNING);
		if (pauseAfterRestart) {
			openavbAvdeccMsgClntHndlChangeRequestFromServer(avdeccMsgHandle, OPENAVB_AVDECC_MSG_PAUSED);
		}
	}

	AVB_LOGF_DEBUG("AVDECC-supplied (Listener) sr_class:  %u", pCfg->sr_class);
	AVB_LOGF_DEBUG("AVDECC-supplied (Listener) stream_id:  " ETH_FORMAT "/%u",
		ETH_OCTETS(pCfg->stream_addr.buffer.ether_addr_octet), pCfg->stream_uid);
	AVB_LOGF_DEBUG("AVDECC-supplied (Listener) dest_addr:  " ETH_FORMAT,
		ETH_OCTETS(pCfg->dest_addr.buffer.ether_addr_octet));
	AVB_LOGF_DEBUG("AVDECC-supplied (Listener) vlan_id:  %u", pCfg->vlan_id);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return true;
}

bool openavbAvdeccMsgClntHndlTalkerStreamIDFromServer(int avdeccMsgHandle,
	U8 sr_class, U8 stream_id_valid, const U8 stream_src_mac[6], U16 stream_uid, U8 stream_dest_valid, const U8 stream_dest_mac[6], U8 stream_vlan_id_valid, U16 stream_vlan_id)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	if (!pState->pTLState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d state not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	// Update the stream information supplied by the server.
	openavb_tl_cfg_t * pCfg = &(pState->pTLState->cfg);
	pCfg->sr_class = sr_class;
	AVB_LOGF_DEBUG("AVDECC-supplied (Talker) sr_class:  %u", pCfg->sr_class);
	if (stream_id_valid) {
		if (memcmp(stream_src_mac, "\x00\x00\x00\x00\x00\x00", 6) == 0 && stream_uid == 0) {
			// Restore from the backup value.
			if (pCfg->backup_stream_id_valid) {
				memcpy(pCfg->stream_addr.buffer.ether_addr_octet, pCfg->backup_stream_addr, 6);
				pCfg->stream_addr.mac = &(pCfg->stream_addr.buffer); // Indicate that the MAC Address is valid.
				pCfg->stream_uid = pCfg->backup_stream_uid;
				pCfg->backup_stream_id_valid = FALSE;
				AVB_LOGF_DEBUG("AVDECC-supplied (Talker) reverted to default stream_id:  " ETH_FORMAT "/%u",
					ETH_OCTETS(pCfg->stream_addr.buffer.ether_addr_octet), pCfg->stream_uid);
			}
		}
		else {
			// Backup the current value.
			if (!pCfg->backup_stream_id_valid) {
				memcpy(pCfg->backup_stream_addr, pCfg->stream_addr.buffer.ether_addr_octet, 6);
				pCfg->backup_stream_uid = pCfg->stream_uid;
				pCfg->backup_stream_id_valid = TRUE;
			}

			// Save the new value.
			memcpy(pCfg->stream_addr.buffer.ether_addr_octet, stream_src_mac, 6);
			pCfg->stream_addr.mac = &(pCfg->stream_addr.buffer); // Indicate that the MAC Address is valid.
			pCfg->stream_uid = stream_uid;
			AVB_LOGF_DEBUG("AVDECC-supplied (Talker) stream_id:  " ETH_FORMAT "/%u",
				ETH_OCTETS(pCfg->stream_addr.buffer.ether_addr_octet), pCfg->stream_uid);
		}
	}
	if (stream_dest_valid) {
		if (memcmp(stream_dest_mac, "\x00\x00\x00\x00\x00\x00", 6) == 0) {
			// Restore from the backup value.
			if (pCfg->backup_dest_addr_valid) {
				memcpy(pCfg->dest_addr.buffer.ether_addr_octet, pCfg->backup_dest_addr, 6);
				pCfg->dest_addr.mac = &(pCfg->dest_addr.buffer); // Indicate that the MAC Address is valid.
				pCfg->backup_dest_addr_valid = FALSE;
				AVB_LOGF_DEBUG("AVDECC-supplied (Talker) reverted to default dest_addr:  " ETH_FORMAT,
					ETH_OCTETS(pCfg->dest_addr.buffer.ether_addr_octet));
			}
		}
		else {
			// Backup the current value.
			if (!pCfg->backup_dest_addr_valid) {
				memcpy(pCfg->backup_dest_addr, pCfg->dest_addr.buffer.ether_addr_octet, 6);
				pCfg->backup_dest_addr_valid = TRUE;
			}

			// Save the new value.
			memcpy(pCfg->dest_addr.buffer.ether_addr_octet, stream_dest_mac, 6);
			pCfg->dest_addr.mac = &(pCfg->dest_addr.buffer); // Indicate that the MAC Address is valid.
			AVB_LOGF_DEBUG("AVDECC-supplied (Talker) dest_addr:  " ETH_FORMAT,
				ETH_OCTETS(pCfg->dest_addr.buffer.ether_addr_octet));
		}
	}
	if (stream_vlan_id_valid) {
		if (stream_vlan_id == 0) {
			// Restore from the backup value.
			if (pCfg->backup_vlan_id_valid) {
				pCfg->vlan_id = pCfg->backup_vlan_id;
				pCfg->backup_vlan_id_valid = FALSE;
				AVB_LOGF_DEBUG("AVDECC-supplied (Talker) reverted to default vlan_id:  %u", pCfg->vlan_id);
			}
		}
		else {
			// Backup the current value.
			if (!pCfg->backup_vlan_id_valid) {
				pCfg->backup_vlan_id_valid = pCfg->vlan_id;
				pCfg->backup_vlan_id_valid = TRUE;
			}

			// Save the new value.
			pCfg->vlan_id = stream_vlan_id;
			AVB_LOGF_DEBUG("AVDECC-supplied (Talker) vlan_id:  %u", pCfg->vlan_id);
		}
	}

	// Notify AVDECC of the Talker values to be used after the update.
	openavbAvdeccMsgClntTalkerStreamID(avdeccMsgHandle, pCfg->sr_class, pCfg->stream_addr.buffer.ether_addr_octet, pCfg->stream_uid, pCfg->dest_addr.buffer.ether_addr_octet, pCfg->vlan_id);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return true;
}

bool openavbAvdeccMsgClntHndlChangeRequestFromServer(int avdeccMsgHandle, openavbAvdeccMsgStateType_t desiredState)
{
	bool ret = false;
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState || !pState->pTLState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	switch (desiredState) {
	case OPENAVB_AVDECC_MSG_STOPPED:
		// Stop requested.
		AVB_LOGF_DEBUG("Stop state requested for client %d", avdeccMsgHandle);
		if (pState->pTLState->bRunning) {
			if (openavbTLStop((tl_handle_t) pState->pTLState)) {
				// NOTE:  openavbTLStop() call will cause client change notification if successful.
				AVB_LOGF_INFO("Client %d state changed to Stopped", avdeccMsgHandle);
				ret = true;
			} else {
				// Notify server of issues.
				AVB_LOGF_ERROR("Unable to change client %d state to Stopped", avdeccMsgHandle);
				openavbAvdeccMsgClntChangeNotification(avdeccMsgHandle, OPENAVB_AVDECC_MSG_UNKNOWN);
			}
		} else {
			// Notify server we are already in this state.
			AVB_LOGF_INFO("Client %d state is already at Stopped", avdeccMsgHandle);
			openavbAvdeccMsgClntChangeNotification(avdeccMsgHandle, OPENAVB_AVDECC_MSG_STOPPED);
			ret = true;
		}
		break;

	case OPENAVB_AVDECC_MSG_RUNNING:
		// Run requested.
		AVB_LOGF_DEBUG("Run state requested for client %d", avdeccMsgHandle);
		if (!(pState->pTLState->bRunning)) {
			if (pState->pTLState && pState->pTLState->cfg.role == AVB_ROLE_TALKER) {
				AVB_LOGF_WARNING("Client handling RUNNING request: handle=%d name=%s uid=%u running=%d paused=%d",
					avdeccMsgHandle,
					pState->pTLState->cfg.friendly_name,
					pState->pTLState->cfg.stream_uid,
					pState->pTLState->bRunning ? 1 : 0,
					pState->pTLState->bPaused ? 1 : 0);
			}
			if (openavbTLRun((tl_handle_t) pState->pTLState)) {
				// NOTE:  openavbTLRun() call will cause client change notification if successful.
				AVB_LOGF_INFO("Client %d state changed to Running", avdeccMsgHandle);
				ret = true;
			} else {
				// Notify server of issues.
				AVB_LOGF_ERROR("Unable to change client %d state to Running", avdeccMsgHandle);
				openavbAvdeccMsgClntChangeNotification(avdeccMsgHandle, OPENAVB_AVDECC_MSG_UNKNOWN);
			}
		}
		else if (pState->pTLState->bRunning && pState->pTLState->bPaused) {
			if (pState->pTLState->cfg.role == AVB_ROLE_TALKER) {
				openavbTLPauseTalker(pState->pTLState, FALSE);
				// NOTE:  openavbTLPauseTalker() call will cause Talker change notification.
				AVB_LOGF_INFO("Talker %d state changed from Paused to Running", avdeccMsgHandle);
				ret = true;
			} else {
				openavbTLPauseListener(pState->pTLState, FALSE);
				// NOTE:  openavbTLPauseListener() call will cause Listener change notification.
				AVB_LOGF_INFO("Listener %d state changed from Paused to Running", avdeccMsgHandle);
				ret = true;
			}
		}
		else {
			// Notify server we are already in this state.
			AVB_LOGF_INFO("Client %d state is already at Running", avdeccMsgHandle);
			openavbAvdeccMsgClntChangeNotification(avdeccMsgHandle, OPENAVB_AVDECC_MSG_RUNNING);
			ret = true;
		}
		break;

	case OPENAVB_AVDECC_MSG_PAUSED:
		// Running with Pause requested.
		AVB_LOGF_DEBUG("Paused state requested for client %d", avdeccMsgHandle);
		if (!(pState->pTLState->bRunning)) {
			// Notify server of issues.
			AVB_LOGF_ERROR("Client %d attempted to pause the stream while not Running.", avdeccMsgHandle);
			openavbAvdeccMsgClntChangeNotification(avdeccMsgHandle, OPENAVB_AVDECC_MSG_UNKNOWN);
		}
		else if (pState->pTLState->bRunning && !(pState->pTLState->bPaused)) {
			if (pState->pTLState->cfg.role == AVB_ROLE_TALKER) {
				openavbTLPauseTalker(pState->pTLState, TRUE);
				// NOTE:  openavbTLPauseTalker() call will cause Talker change notification.
				AVB_LOGF_INFO("Talker %d state changed from Running to Paused", avdeccMsgHandle);
				ret = true;
			} else {
				openavbTLPauseListener(pState->pTLState, TRUE);
				// NOTE:  openavbTLPauseListener() call will cause Listener change notification.
				AVB_LOGF_INFO("Listener %d state changed from Running to Paused", avdeccMsgHandle);
				ret = true;
			}
		}
		else {
			// Notify server we are already in this state.
			AVB_LOGF_INFO("Client %d state is already at Paused", avdeccMsgHandle);
			openavbAvdeccMsgClntChangeNotification(avdeccMsgHandle, OPENAVB_AVDECC_MSG_PAUSED);
			ret = true;
		}
		break;

	default:
		AVB_LOGF_ERROR("openavbAvdeccMsgClntHndlChangeRequestFromServer invalid state %d", desiredState);
		break;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

bool openavbAvdeccMsgClntChangeNotification(int avdeccMsgHandle, openavbAvdeccMsgStateType_t currentState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	openavbAvdeccMessage_t msgBuf;

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	memset(&msgBuf, 0, OPENAVB_AVDECC_MSG_LEN);
	msgBuf.type = OPENAVB_AVDECC_MSG_CLIENT_CHANGE_NOTIFICATION;
	openavbAvdeccMsgParams_ClientChangeNotification_t * pParams =
		&(msgBuf.params.clientChangeNotification);
	pParams->current_state = (U8) currentState;
	bool ret = openavbAvdeccMsgClntSendToServer(avdeccMsgHandle, &msgBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

bool openavbAvdeccMsgClntCountersUpdate(int avdeccMsgHandle, const openavb_avtp_diag_counters_t *pCounters)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	openavbAvdeccMessage_t msgBuf;

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}
	if (!pCounters) {
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	memset(&msgBuf, 0, OPENAVB_AVDECC_MSG_LEN);
	msgBuf.type = OPENAVB_AVDECC_MSG_C2S_COUNTERS_UPDATE;
	msgBuf.params.c2sCountersUpdate.counters = *pCounters;

	bool ret = openavbAvdeccMsgClntSendToServer(avdeccMsgHandle, &msgBuf);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return ret;
}

bool openavbAvdeccMsgClntPublishCounters(tl_state_t *pTLState)
{
	openavb_avtp_diag_counters_t counters;
	avdecc_msg_state_t *pState;

	if (!pTLState || !pTLState->bAvdeccMsgRunning) {
		return false;
	}

	pState = openavbAvdeccMsgClntGetStateForTL(pTLState);
	if (!pState) {
		return false;
	}
	if (!openavbAvdeccMsgClntGetCurrentCounters(pTLState, &counters)) {
		return false;
	}

	return openavbAvdeccMsgClntCountersUpdate(pState->avdeccMsgHandle, &counters);
}

bool openavbAvdeccMsgClntHndlClockSourceUpdateFromServer(
	int avdeccMsgHandle,
	U16 clock_domain_index,
	U16 clock_source_index,
	U16 clock_source_flags,
	U16 clock_source_type,
	U16 clock_source_location_type,
	U16 clock_source_location_index)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	avdecc_msg_state_t *pState = AvdeccMsgStateListGet(avdeccMsgHandle);
	if (!pState || !pState->pTLState) {
		AVB_LOGF_ERROR("avdeccMsgHandle %d not valid", avdeccMsgHandle);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return false;
	}

	tl_state_t *pTLState = pState->pTLState;
	pTLState->clock_domain_index = clock_domain_index;
	pTLState->clock_source_index = clock_source_index;
	pTLState->clock_source_flags = clock_source_flags;
	pTLState->clock_source_type = clock_source_type;
	pTLState->clock_source_location_type = clock_source_location_type;
	pTLState->clock_source_location_index = clock_source_location_index;
	pTLState->clock_source_generation++;

	openavbClockSourceRuntimeSetSelection(
		clock_domain_index,
		clock_source_index,
		clock_source_flags,
		clock_source_type,
		clock_source_location_type,
		clock_source_location_index);

	// For talkers, request AVTP media-restart bit toggle on the next packets.
	if (pTLState->cfg.role == AVB_ROLE_TALKER &&
			pTLState->pPvtTalkerData) {
		talker_data_t *pTalkerData = (talker_data_t *)pTLState->pPvtTalkerData;
		if (pTalkerData->avtpHandle) {
			openavbAvtpRequestMediaRestart(pTalkerData->avtpHandle);
		}
	}

	AVB_LOGF_INFO("Clock source update received: domain=%u source=%u flags=0x%04x type=0x%04x location=0x%04x/%u generation=%u",
		clock_domain_index,
		clock_source_index,
		clock_source_flags,
		clock_source_type,
		clock_source_location_type,
		clock_source_location_index,
		pTLState->clock_source_generation);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return true;
}


// Called from openavbAvdeccMsgThreadFn() which is started from openavbTLRun()
void openavbAvdeccMsgRunTalker(avdecc_msg_state_t *pState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	if (!pState) {
		AVB_LOG_ERROR("Invalid AVDECC Msg State");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	openavb_tl_cfg_t * cfg = &(pState->pTLState->cfg);
	if (cfg->role != AVB_ROLE_TALKER) {
		AVB_LOG_ERROR("openavbAvdeccMsgRunTalker() passed a non-Talker state");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	if (!pState->bConnected) {
		AVB_LOG_WARNING("Failed to connect to AVDECC Msg");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Let the AVDECC Msg server know our identity.
	if (!openavbAvdeccMsgClntInitIdentify(pState->avdeccMsgHandle, cfg->friendly_name, true)) {
		AVB_LOG_ERROR("openavbAvdeccMsgClntInitIdentify() failed");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Let the AVDECC Msg server know our stream ID.
	if (!openavbAvdeccMsgClntTalkerStreamID(pState->avdeccMsgHandle,
			cfg->sr_class, cfg->stream_addr.buffer.ether_addr_octet, cfg->stream_uid,
			cfg->dest_addr.buffer.ether_addr_octet, cfg->vlan_id)) {
		AVB_LOG_ERROR("openavbAvdeccMsgClntTalkerStreamID() failed");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Let the AVDECC Msg server know our current state.
	if (!openavbAvdeccMsgClntNotifyCurrentState(pState->pTLState)) {
		AVB_LOG_ERROR("Initial openavbAvdeccMsgClntNotifyCurrentState() failed");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Do until we are stopped or lose connection to AVDECC Msg server.
	while (pState->pTLState->bAvdeccMsgRunning && pState->bConnected) {

		// Look for messages from AVDECC Msg.
		if (!openavbAvdeccMsgClntService(pState->avdeccMsgHandle, 1000)) {
			AVB_LOG_WARNING("Lost connection to AVDECC Msg");
			pState->bConnected = FALSE;
			pState->avdeccMsgHandle = AVB_AVDECC_MSG_HANDLE_INVALID;
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
}

// Called from openavbAvdeccMsgThreadFn() which is started from openavbTLRun()
void openavbAvdeccMsgRunListener(avdecc_msg_state_t *pState)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);

	if (!pState && !pState->pTLState) {
		AVB_LOG_ERROR("Invalid AVDECC Msg State");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	openavb_tl_cfg_t * cfg = &(pState->pTLState->cfg);
	if (cfg->role != AVB_ROLE_LISTENER) {
		AVB_LOG_ERROR("openavbAvdeccMsgRunListener() passed a non-Listener state");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	if (!pState->bConnected) {
		AVB_LOG_WARNING("Failed to connect to AVDECC Msg");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Let the AVDECC Msg server know our identity.
	if (!openavbAvdeccMsgClntInitIdentify(pState->avdeccMsgHandle, cfg->friendly_name, false)) {
		AVB_LOG_ERROR("openavbAvdeccMsgClntInitIdentify() failed");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Let the AVDECC Msg server know our current state.
	if (!openavbAvdeccMsgClntNotifyCurrentState(pState->pTLState)) {
		AVB_LOG_ERROR("Initial openavbAvdeccMsgClntNotifyCurrentState() failed");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
		return;
	}

	// Do until we are stopped or lose connection to AVDECC Msg.
	while (pState->pTLState->bAvdeccMsgRunning && pState->bConnected) {

		// Look for messages from AVDECC Msg.
		if (!openavbAvdeccMsgClntService(pState->avdeccMsgHandle, 1000)) {
			AVB_LOG_WARNING("Lost connection to AVDECC Msg");
			pState->bConnected = FALSE;
			pState->avdeccMsgHandle = AVB_AVDECC_MSG_HANDLE_INVALID;
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
}

// AVDECC Msg thread function
void* openavbAvdeccMsgThreadFn(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC_MSG);
	avdecc_msg_state_t avdeccMsgState;

	// Perform the base initialization.
	openavbAvdeccMsgInitialize();

	// Initialize our local state structure.
	memset(&avdeccMsgState, 0, sizeof(avdeccMsgState));
	avdeccMsgState.pTLState = (tl_state_t *)pv;

	avdeccMsgState.avdeccMsgHandle =
		avdeccMsgState.pTLState->avdeccMsgHandle = AVB_AVDECC_MSG_HANDLE_INVALID;
	while (avdeccMsgState.pTLState->bAvdeccMsgRunning) {
		AVB_TRACE_LINE(AVB_TRACE_AVDECC_MSG_DETAIL);

		if (avdeccMsgState.avdeccMsgHandle == AVB_AVDECC_MSG_HANDLE_INVALID) {
			avdeccMsgState.avdeccMsgHandle = openavbAvdeccMsgClntOpenSrvrConnection();
			if (avdeccMsgState.avdeccMsgHandle == AVB_AVDECC_MSG_HANDLE_INVALID) {
				// error connecting to AVDECC Msg, already logged
				SLEEP(1);
				continue;
			}
			avdeccMsgState.pTLState->avdeccMsgHandle = avdeccMsgState.avdeccMsgHandle;
		}
		AvdeccMsgStateListAdd(&avdeccMsgState);

		// Validate the AVB version for client and server are the same before continuing
		avdeccMsgState.verState = OPENAVB_AVDECC_MSG_VER_UNKNOWN;
		avdeccMsgState.bConnected = openavbAvdeccMsgClntRequestVersionFromServer(avdeccMsgState.avdeccMsgHandle);
		if (avdeccMsgState.pTLState->bAvdeccMsgRunning && avdeccMsgState.bConnected && avdeccMsgState.verState == OPENAVB_AVDECC_MSG_VER_UNKNOWN) {
			// Check for AVDECC Msg version message. Timeout in 500 msec.
			if (!openavbAvdeccMsgClntService(avdeccMsgState.avdeccMsgHandle, 500)) {
				AVB_LOG_WARNING("Lost connection to AVDECC Msg, will retry");
				avdeccMsgState.bConnected = FALSE;
				avdeccMsgState.avdeccMsgHandle =
					avdeccMsgState.pTLState->avdeccMsgHandle = AVB_AVDECC_MSG_HANDLE_INVALID;
			}
		}
		if (avdeccMsgState.verState == OPENAVB_AVDECC_MSG_VER_UNKNOWN) {
			AVB_LOG_ERROR("AVB core version not reported by AVDECC Msg server.  Will reconnect to AVDECC Msg and check again.");
		} else if (avdeccMsgState.verState == OPENAVB_AVDECC_MSG_VER_INVALID) {
			AVB_LOG_ERROR("AVB core version is different than AVDECC Msg AVB core version.  Will reconnect to AVDECC Msg and check again.");
		} else {
			AVB_LOG_DEBUG("AVB core version matches AVDECC Msg AVB core version.");
		}

		if (avdeccMsgState.bConnected && avdeccMsgState.verState == OPENAVB_AVDECC_MSG_VER_VALID) {
			if (avdeccMsgState.pTLState->cfg.role == AVB_ROLE_TALKER) {
				openavbAvdeccMsgRunTalker(&avdeccMsgState);
			}
			else {
				openavbAvdeccMsgRunListener(&avdeccMsgState);
			}
		}

		// Close the AVDECC Msg connection.
		AvdeccMsgStateListRemove(&avdeccMsgState);
		openavbAvdeccMsgClntCloseSrvrConnection(avdeccMsgState.avdeccMsgHandle);
		avdeccMsgState.bConnected = FALSE;
		avdeccMsgState.avdeccMsgHandle =
			avdeccMsgState.pTLState->avdeccMsgHandle = AVB_AVDECC_MSG_HANDLE_INVALID;

		if (avdeccMsgState.pTLState->bAvdeccMsgRunning && avdeccMsgState.verState == OPENAVB_AVDECC_MSG_VER_VALID) {
			SLEEP(1);
		}
	}

	avdeccMsgState.pTLState = NULL;

	// Perform the base cleanup.
	openavbAvdeccMsgCleanup();

	THREAD_JOINABLE(avdeccMsgState.pTLState->avdeccMsgThread);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC_MSG);
	return NULL;
}


// Client-side helper function.
bool openavbAvdeccMsgClntNotifyCurrentState(tl_state_t *pTLState)
{
	avdecc_msg_state_t *pAvdeccMsgState;
	bool ret;

	if (!pTLState) { return FALSE; }
	if (!(pTLState->bAvdeccMsgRunning)) { return FALSE; }

	pAvdeccMsgState = openavbAvdeccMsgClntGetStateForTL(pTLState);
	if (!pAvdeccMsgState) {
		return FALSE;
	}

	if (pTLState->bRunning) {
		ret = openavbAvdeccMsgClntChangeNotification(
			pAvdeccMsgState->avdeccMsgHandle,
			(pTLState->bPaused ? OPENAVB_AVDECC_MSG_PAUSED : OPENAVB_AVDECC_MSG_RUNNING));
	}
	else {
		ret = openavbAvdeccMsgClntChangeNotification(
			pAvdeccMsgState->avdeccMsgHandle,
			OPENAVB_AVDECC_MSG_STOPPED);
	}

	if (!ret) {
		return FALSE;
	}

	return openavbAvdeccMsgClntPublishCounters(pTLState);
}
