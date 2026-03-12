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
 ******************************************************************
 * MODULE : ACMP - AVDECC Connection Management Protocol : Talker State Machine
 * MODULE SUMMARY : Implements the ACMP - AVDECC Connection Management Protocol : Talker State Machine
 * IEEE Std 1722.1-2013 clause 8.2.2.6
 ******************************************************************
 */

#include "openavb_platform.h"

#include <signal.h>
#include <errno.h>

#define	AVB_LOG_COMPONENT	"ACMP"
#include "openavb_log.h"

#include "openavb_aem.h"
#include "openavb_acmp_sm_talker.h"
#include "openavb_acmp_sm_listener.h"
#include "openavb_acmp_message.h"
#include "openavb_avdecc_pipeline_interaction_pub.h"

typedef enum {
	OPENAVB_ACMP_SM_TALKER_STATE_WAITING,
	OPENAVB_ACMP_SM_TALKER_STATE_CONNECT,
	OPENAVB_ACMP_SM_TALKER_STATE_DISCONNECT,
	OPENAVB_ACMP_SM_TALKER_STATE_GET_STATE,
	OPENAVB_ACMP_SM_TALKER_STATE_GET_CONNECTION
} openavb_acmp_sm_talker_state_t;

extern openavb_acmp_sm_global_vars_t openavbAcmpSMGlobalVars;

static openavb_acmp_ACMPCommandResponse_t rcvdCmdResp;
static openavb_acmp_ACMPCommandResponse_t *pRcvdCmdResp = &rcvdCmdResp;
static openavb_acmp_sm_talker_vars_t openavbAcmpSMTalkerVars = {0};
static bool bRunning = FALSE;

static bool openavbAcmpSMTalkerCanQueueCmd(const openavb_acmp_ACMPCommandResponse_t *command, const char *cmdName)
{
	if (!command) {
		AVB_LOGF_WARNING("Ignoring %s: null command", cmdName);
		return FALSE;
	}
	if (!bRunning) {
		AVB_LOGF_DEBUG("Ignoring %s: talker state machine not running", cmdName);
		return FALSE;
	}
	return TRUE;
}

extern MUTEX_HANDLE(openavbAcmpMutex);
#define ACMP_LOCK() { MUTEX_CREATE_ERR(); MUTEX_LOCK(openavbAcmpMutex); MUTEX_LOG_ERR("Mutex lock failure"); }
#define ACMP_UNLOCK() { MUTEX_CREATE_ERR(); MUTEX_UNLOCK(openavbAcmpMutex); MUTEX_LOG_ERR("Mutex unlock failure"); }

static MUTEX_HANDLE(openavbAcmpSMTalkerMutex);
#define ACMP_SM_LOCK() { MUTEX_CREATE_ERR(); MUTEX_LOCK(openavbAcmpSMTalkerMutex); MUTEX_LOG_ERR("Mutex lock failure"); }
#define ACMP_SM_UNLOCK() { MUTEX_CREATE_ERR(); MUTEX_UNLOCK(openavbAcmpSMTalkerMutex); MUTEX_LOG_ERR("Mutex unlock failure"); }

SEM_T(openavbAcmpSMTalkerSemaphore);
THREAD_TYPE(openavbAcmpSmTalkerThread);
THREAD_DEFINITON(openavbAcmpSmTalkerThread);

static bool openavbAcmpSMTalkerPostSemaphoreLocked(const char *cmdName)
{
	SEM_ERR_T(err);
	SEM_POST(openavbAcmpSMTalkerSemaphore, err);
	if (SEM_IS_ERR_NONE(err)) {
		return TRUE;
	}

	// During stop, semaphore teardown can race with asynchronous ACMP callbacks.
	// Avoid logging through the generic macro path that has crashed on teardown.
	if (!bRunning) {
		AVB_LOGF_DEBUG("Ignoring %s semaphore post failure during shutdown: ret=%d errno=%d", cmdName, err, errno);
		return FALSE;
	}
	AVB_LOGF_WARNING("%s semaphore post failed: ret=%d errno=%d", cmdName, err, errno);
	return FALSE;
}


openavb_list_node_t openavbAcmpSMTalker_findListenerPairNodeFromCommand(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);

	openavb_list_node_t node = NULL;

	openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayDataIdx(openavbAcmpSMTalkerVars.talkerStreamInfos, command->talker_unique_id);
	if (pTalkerStreamInfo) {
		node = openavbListIterFirst(pTalkerStreamInfo->connected_listeners);
		while (node) {
			openavb_acmp_ListenerPair_t *pListenerPair = openavbListData(node);
			if (pListenerPair) {
				if (memcmp(pListenerPair->listener_entity_id, command->listener_entity_id, sizeof(pListenerPair->listener_entity_id)) == 0) {
					if (pListenerPair->listener_unique_id == command->listener_unique_id) {
						break;
					}
				}
			}
			node = openavbListIterNext(pTalkerStreamInfo->connected_listeners);
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return node;
}


bool openavbAcmpSMTalker_validTalkerUnique(U16 talkerUniqueId)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);

	if (!openavbAemGetDescriptor(openavbAemGetConfigIdx(), OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT, talkerUniqueId)) {
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return TRUE;
}

U8 openavbAcmpSMTalker_connectTalker(openavb_acmp_ACMPCommandResponse_t *command, bool *pDeferredResponse)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	U8 retStatus = OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;
	if (pDeferredResponse) {
		*pDeferredResponse = FALSE;
	}

	openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayDataIdx(openavbAcmpSMTalkerVars.talkerStreamInfos, command->talker_unique_id);
	if (pTalkerStreamInfo) {
		openavb_list_node_t node = openavbAcmpSMTalker_findListenerPairNodeFromCommand(command);
		if (node) {
			// Refresh stream state from the talker side in case async update delivery is delayed.
			U16 configIdx = openavbAemGetConfigIdx();
			openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput =
				openavbAemGetDescriptor(configIdx, OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT, command->talker_unique_id);
			if (pDescriptorStreamOutput) {
				openavbAVDECCGetTalkerStreamInfo(pDescriptorStreamOutput, configIdx, pTalkerStreamInfo);
			}

				if (memcmp(pTalkerStreamInfo->stream_id, "\x00\x00\x00\x00\x00\x00\x00\x00", 8) == 0 ||
						memcmp(pTalkerStreamInfo->stream_dest_mac, "\x00\x00\x00\x00\x00\x00", 6) == 0) {
					// In the process of connecting. Keep the newest command context so the eventual
					// CONNECT_TX_RESPONSE uses the latest sequence/controller tuple from retries.
				if (!pTalkerStreamInfo->waiting_on_talker) {
					pTalkerStreamInfo->waiting_on_talker = (openavb_acmp_ACMPCommandResponse_t *) malloc(sizeof(openavb_acmp_ACMPCommandResponse_t));
					if (!pTalkerStreamInfo->waiting_on_talker) {
						AVB_TRACE_EXIT(AVB_TRACE_ACMP);
						return OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;
					}
					}
					memcpy(pTalkerStreamInfo->waiting_on_talker, command, sizeof(*command));
					pTalkerStreamInfo->waiting_on_talker->connection_count = pTalkerStreamInfo->connection_count;
					command->connection_count = pTalkerStreamInfo->connection_count;
					if (pDeferredResponse) {
						*pDeferredResponse = TRUE;
					}
					retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
				}
				else {
				// Already connected, so return the current status.
				if (pDescriptorStreamOutput) {
					command->flags = (pDescriptorStreamOutput->acmp_flags &
							(OPENAVB_ACMP_FLAG_CLASS_B |
							 OPENAVB_ACMP_FLAG_FAST_CONNECT |
							 OPENAVB_ACMP_FLAG_SAVED_STATE |
							 OPENAVB_ACMP_FLAG_SUPPORTS_ENCRYPTED |
							 OPENAVB_ACMP_FLAG_ENCRYPTED_PDU));
				}
				memcpy(command->stream_id, pTalkerStreamInfo->stream_id, sizeof(command->stream_id));
				memcpy(command->stream_dest_mac, pTalkerStreamInfo->stream_dest_mac, sizeof(command->stream_dest_mac));
				command->stream_vlan_id = pTalkerStreamInfo->stream_vlan_id;
				command->connection_count = pTalkerStreamInfo->connection_count;
				openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, command, OPENAVB_ACMP_STATUS_SUCCESS);
				retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
			}
		}
			else {
				if (!pTalkerStreamInfo->connected_listeners) {
					pTalkerStreamInfo->connected_listeners = openavbListNewList();
					if (!pTalkerStreamInfo->connected_listeners) {
						AVB_TRACE_EXIT(AVB_TRACE_ACMP);
						return OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;
					}
				}
				node = openavbListNew(pTalkerStreamInfo->connected_listeners, sizeof(openavb_acmp_ListenerPair_t));
				if (node) {
				openavb_acmp_ListenerPair_t *pListenerPair = openavbListData(node);
				memcpy(pListenerPair->listener_entity_id, command->listener_entity_id, sizeof(pListenerPair->listener_entity_id));
				pListenerPair->listener_unique_id = command->listener_unique_id;
				pTalkerStreamInfo->connection_count++;

				U16 configIdx = openavbAemGetConfigIdx();
				openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(configIdx, OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT, command->talker_unique_id);
				if (!pDescriptorStreamOutput) {
					AVB_TRACE_EXIT(AVB_TRACE_ACMP);
					return retStatus;
				}

					if (openavbAVDECCRunTalker(pDescriptorStreamOutput, configIdx, pTalkerStreamInfo)) {

						command->connection_count = pTalkerStreamInfo->connection_count;

					// Wait for the Talker to supply us with updated stream information.
					if (!pTalkerStreamInfo->waiting_on_talker) {
						pTalkerStreamInfo->waiting_on_talker = (openavb_acmp_ACMPCommandResponse_t *) malloc(sizeof(openavb_acmp_ACMPCommandResponse_t));
						if (!pTalkerStreamInfo->waiting_on_talker) {
							AVB_TRACE_EXIT(AVB_TRACE_ACMP);
							return OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;
						}
					}
							memcpy(pTalkerStreamInfo->waiting_on_talker, command, sizeof(openavb_acmp_ACMPCommandResponse_t));
							if (pDeferredResponse) {
								*pDeferredResponse = TRUE;
							}
							retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
						}
					else {
						// Stream startup can race the AVDECC message client attachment.
						// Roll back provisional listener registration so a retry command can reconnect cleanly.
						openavbListDelete(pTalkerStreamInfo->connected_listeners, node);
						pTalkerStreamInfo->connection_count--;
					}
				}
			}
		}
	else {
		retStatus = OPENAVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return retStatus;
}

void openavbAcmpSMTalker_txResponse(U8 messageType, openavb_acmp_ACMPCommandResponse_t *response, U8 error)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	openavbAcmpMessageSend(messageType, response, error);
	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

U8 openavbAcmpSMTalker_disconnectTalker(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	U8 retStatus = OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;

	openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayDataIdx(openavbAcmpSMTalkerVars.talkerStreamInfos, command->talker_unique_id);
	if (pTalkerStreamInfo) {
		openavb_list_node_t node = openavbAcmpSMTalker_findListenerPairNodeFromCommand(command);
		if (!node) {
			// Already disconnected, so return the current status.
			memcpy(command->stream_id, pTalkerStreamInfo->stream_id, sizeof(command->stream_id));
			memcpy(command->stream_dest_mac, pTalkerStreamInfo->stream_dest_mac, sizeof(command->stream_dest_mac));
			command->connection_count = pTalkerStreamInfo->connection_count;
			retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
		}
		else {
			openavbListDelete(pTalkerStreamInfo->connected_listeners, node);
			pTalkerStreamInfo->connection_count--;

			U16 configIdx = openavbAemGetConfigIdx();
			openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(configIdx, OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT, command->talker_unique_id);
			if (!pDescriptorStreamOutput) {
				AVB_TRACE_EXIT(AVB_TRACE_ACMP);
				return retStatus;
			}
			openavbAVDECCGetTalkerStreamInfo(pDescriptorStreamOutput, configIdx, pTalkerStreamInfo);

			// Stop the Talker if connection_count is 0.
			if (pTalkerStreamInfo->connection_count > 0 ||
					openavbAVDECCStopTalker(pDescriptorStreamOutput, configIdx, pTalkerStreamInfo)) {
				memcpy(command->stream_id, pTalkerStreamInfo->stream_id, sizeof(command->stream_id));
				memcpy(command->stream_dest_mac, pTalkerStreamInfo->stream_dest_mac, sizeof(command->stream_dest_mac));
				command->connection_count = pTalkerStreamInfo->connection_count;
				retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
			}
		}
	}
	else {
		retStatus = OPENAVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return retStatus;
}

U8 openavbAcmpSMTalker_getState(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	U8 retStatus = OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;

	openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayDataIdx(openavbAcmpSMTalkerVars.talkerStreamInfos, command->talker_unique_id);
	if (pTalkerStreamInfo) {
		U16 configIdx = openavbAemGetConfigIdx();
		openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(configIdx, OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT, command->talker_unique_id);
		if (!pDescriptorStreamOutput) {
			AVB_TRACE_EXIT(AVB_TRACE_ACMP);
			return retStatus;
		}
		openavbAVDECCGetTalkerStreamInfo(pDescriptorStreamOutput, configIdx, pTalkerStreamInfo);

		memcpy(command->stream_id, pTalkerStreamInfo->stream_id, sizeof(command->stream_id));
		memcpy(command->stream_dest_mac, pTalkerStreamInfo->stream_dest_mac, sizeof(command->stream_dest_mac));
		command->stream_vlan_id = pTalkerStreamInfo->stream_vlan_id;
		// Milan 1.3 requires GET_TX_STATE to advertise 0 connection_count.
		// Controllers are expected to use GET_TX_CONNECTION for per-listener detail.
		command->connection_count = 0;
		retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return retStatus;
}

U8 openavbAcmpSMTalker_getConnection(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	U8 retStatus = OPENAVB_ACMP_STATUS_TALKER_MISBEHAVING;

	openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayDataIdx(openavbAcmpSMTalkerVars.talkerStreamInfos, command->talker_unique_id);
	if (pTalkerStreamInfo) {
		int count = command->connection_count;

		openavb_list_node_t node = openavbListIterFirst(pTalkerStreamInfo->connected_listeners);
		while (node && count-- > 0) {
			node = openavbListIterNext(pTalkerStreamInfo->connected_listeners);
		}

		openavb_acmp_ListenerPair_t *pListenerPair = openavbListData(node);
		if (pListenerPair) {
			memcpy(command->stream_id, pTalkerStreamInfo->stream_id, sizeof(command->stream_id));
			memcpy(command->stream_dest_mac, pTalkerStreamInfo->stream_dest_mac, sizeof(command->stream_dest_mac));
			command->stream_vlan_id = pTalkerStreamInfo->stream_vlan_id;
			command->connection_count = pTalkerStreamInfo->connection_count;

			memcpy(command->listener_entity_id, pListenerPair->listener_entity_id, sizeof(command->listener_entity_id));
			command->listener_unique_id = pListenerPair->listener_unique_id;
			retStatus = OPENAVB_ACMP_STATUS_SUCCESS;
		}
		else {
			retStatus = OPENAVB_ACMP_STATUS_NO_SUCH_CONNECTION;
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return retStatus;
}

void openavbAcmpSMTalkerStateMachine()
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);

	openavb_acmp_sm_talker_state_t state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;

	// Lock such that the mutex is held unless waiting for a semaphore. Synchronous processing of command responses.
	ACMP_SM_LOCK();
	while (bRunning) {
		switch (state) {
			case OPENAVB_ACMP_SM_TALKER_STATE_WAITING:
				AVB_TRACE_LINE(AVB_TRACE_ACMP);
				AVB_LOG_DEBUG("State:  OPENAVB_ACMP_SM_TALKER_STATE_WAITING");

				openavbAcmpSMTalkerVars.rcvdConnectTX = FALSE;
				openavbAcmpSMTalkerVars.rcvdDisconnectTX = FALSE;
				openavbAcmpSMTalkerVars.rcvdGetTXState = FALSE;
				openavbAcmpSMTalkerVars.rcvdGetTXConnection = FALSE;

				// Wait for a change in state
				while (state == OPENAVB_ACMP_SM_TALKER_STATE_WAITING && bRunning) {
					AVB_TRACE_LINE(AVB_TRACE_ACMP);

					ACMP_SM_UNLOCK();
					SEM_ERR_T(err);
					SEM_WAIT(openavbAcmpSMTalkerSemaphore, err);
					ACMP_SM_LOCK();

					if (SEM_IS_ERR_NONE(err)) {
						if (openavbAcmpSMTalkerVars.doTerminate) {
							bRunning = FALSE;
						}
						else if (memcmp(pRcvdCmdResp->talker_entity_id, openavbAcmpSMGlobalVars.my_id, sizeof(openavbAcmpSMGlobalVars.my_id)) == 0) {

							if (openavbAcmpSMTalkerVars.rcvdConnectTX) {
								state = OPENAVB_ACMP_SM_TALKER_STATE_CONNECT;
							}
							else if (openavbAcmpSMTalkerVars.rcvdDisconnectTX) {
								state = OPENAVB_ACMP_SM_TALKER_STATE_DISCONNECT;
							}
							else if (openavbAcmpSMTalkerVars.rcvdGetTXState) {
								state = OPENAVB_ACMP_SM_TALKER_STATE_GET_STATE;
							}
							else if (openavbAcmpSMTalkerVars.rcvdGetTXConnection) {
								state = OPENAVB_ACMP_SM_TALKER_STATE_GET_CONNECTION;
							}
						}
					}
				}
				break;

				case OPENAVB_ACMP_SM_TALKER_STATE_CONNECT:
					{
						AVB_TRACE_LINE(AVB_TRACE_ACMP);
						AVB_LOG_DEBUG("State:  OPENAVB_ACMP_SM_TALKER_STATE_CONNECT");

							U8 error;
							bool deferredResponse = FALSE;
							openavb_acmp_ACMPCommandResponse_t response;
						memcpy(&response, pRcvdCmdResp, sizeof(response));
					if (!openavbAcmpSMTalker_validTalkerUnique(pRcvdCmdResp->talker_unique_id)) {
						// Talker ID is not recognized.
						error = OPENAVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
						openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, &response, error);
						state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;
						break;
					}

					// Try and start the Talker.
						error = openavbAcmpSMTalker_connectTalker(&response, &deferredResponse);
						if (error != OPENAVB_ACMP_STATUS_SUCCESS) {
							openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, &response, error);
							state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;
							break;
						}

						// Avoid listener timeout while stream details are still being populated asynchronously.
						if (deferredResponse) {
							openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, &response, OPENAVB_ACMP_STATUS_SUCCESS);
						}

						// openavbAcmpSMTalker_connectTalker() either sent a response, or updated the state
						// to indicate we are waiting for information from the Talker.
					// Either way, there is nothing else to do for now.
					state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;
				}
				break;

			case OPENAVB_ACMP_SM_TALKER_STATE_DISCONNECT:
				{
					AVB_TRACE_LINE(AVB_TRACE_ACMP);
					AVB_LOG_DEBUG("State:  OPENAVB_ACMP_SM_TALKER_STATE_DISCONNECT");

					U8 error;
					openavb_acmp_ACMPCommandResponse_t response;
					memcpy(&response, pRcvdCmdResp, sizeof(response));
					if (openavbAcmpSMTalker_validTalkerUnique(pRcvdCmdResp->talker_unique_id)) {
						error = openavbAcmpSMTalker_disconnectTalker(&response);
					}
					else {
						error = OPENAVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
					}
					openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, &response, error);

					state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;
				}
				break;

			case OPENAVB_ACMP_SM_TALKER_STATE_GET_STATE:
				{
					AVB_LOG_DEBUG("State:  OPENAVB_ACMP_SM_TALKER_STATE_GET_STATE");

					U8 error;
					openavb_acmp_ACMPCommandResponse_t response;
					memcpy(&response, pRcvdCmdResp, sizeof(response));
					if (openavbAcmpSMTalker_validTalkerUnique(pRcvdCmdResp->talker_unique_id)) {
						error = openavbAcmpSMTalker_getState(&response);
					}
					else {
						error = OPENAVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
					}
					openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, &response, error);

					state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;
				}
				break;

				case OPENAVB_ACMP_SM_TALKER_STATE_GET_CONNECTION:
					{
						AVB_LOG_DEBUG("State:  OPENAVB_ACMP_SM_TALKER_STATE_GET_CONNECTION");

						U8 error;
						openavb_acmp_ACMPCommandResponse_t response;
						memcpy(&response, pRcvdCmdResp, sizeof(response));
						if (openavbAcmpSMTalker_validTalkerUnique(pRcvdCmdResp->talker_unique_id)) {
							error = openavbAcmpSMTalker_getConnection(&response);
						}
					else {
						error = OPENAVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
					}
					openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, &response, error);

					state = OPENAVB_ACMP_SM_TALKER_STATE_WAITING;
				}
				break;
		}
	}
	ACMP_SM_UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

void* openavbAcmpSMTalkerThreadFn(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	openavbAcmpSMTalkerStateMachine();
	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return NULL;
}

bool openavbAcmpSMTalkerStart()
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);

	openavbAcmpSMTalkerVars.talkerStreamInfos = openavbArrayNewArray(sizeof(openavb_acmp_TalkerStreamInfo_t));
	if (!openavbAcmpSMTalkerVars.talkerStreamInfos) {
		AVB_LOG_ERROR("Unable to create talkerStreamInfos array. ACMP protocol not started.");
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return FALSE;
	}
	openavb_array_t streamOutputDescriptorArray = openavbAemGetDescriptorArray(openavbAemGetConfigIdx(), OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT);
	if (streamOutputDescriptorArray) {
		openavbArrayMultiNew(openavbAcmpSMTalkerVars.talkerStreamInfos, openavbArrayCount(streamOutputDescriptorArray));
	}

	MUTEX_ATTR_HANDLE(mta);
	MUTEX_ATTR_INIT(mta);
	MUTEX_ATTR_SET_TYPE(mta, MUTEX_ATTR_TYPE_DEFAULT);
	MUTEX_ATTR_SET_NAME(mta, "openavbAcmpSMTalkerMutex");
	MUTEX_CREATE_ERR();
	MUTEX_CREATE(openavbAcmpSMTalkerMutex, mta);
	MUTEX_LOG_ERR("Could not create/initialize 'openavbAcmpSMTalkerMutex' mutex");

	SEM_ERR_T(err);
	SEM_INIT(openavbAcmpSMTalkerSemaphore, 1, err);
	SEM_LOG_ERR(err);

	openavbAcmpSMTalkerVars.doTerminate = FALSE;

	// Start the State Machine
	bool errResult;
	bRunning = TRUE;
	THREAD_CREATE(openavbAcmpSmTalkerThread, openavbAcmpSmTalkerThread, NULL, openavbAcmpSMTalkerThreadFn, NULL);
	THREAD_CHECK_ERROR(openavbAcmpSmTalkerThread, "Thread / task creation failed", errResult);
	if (errResult) {
		bRunning = FALSE;
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
	return TRUE;
}

void openavbAcmpSMTalkerStop()
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);

	if (bRunning) {
		ACMP_SM_LOCK();
		openavbAcmpSMTalkerVars.doTerminate = TRUE;
		bRunning = FALSE;
		(void)openavbAcmpSMTalkerPostSemaphoreLocked("TALKER_STOP");
		ACMP_SM_UNLOCK();

		THREAD_JOIN(openavbAcmpSmTalkerThread, NULL);
	}

	ACMP_SM_LOCK();
	SEM_ERR_T(err);
	SEM_DESTROY(openavbAcmpSMTalkerSemaphore, err);
	SEM_LOG_ERR(err);

	openavb_array_elem_t node = openavbArrayIterFirst(openavbAcmpSMTalkerVars.talkerStreamInfos);
	while (node) {
		openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayData(node);
		if (pTalkerStreamInfo != NULL) {
				openavbListDeleteListShallow(pTalkerStreamInfo->connected_listeners);
			pTalkerStreamInfo->connected_listeners = NULL;
			if (pTalkerStreamInfo->waiting_on_talker) {
				free(pTalkerStreamInfo->waiting_on_talker);
				pTalkerStreamInfo->waiting_on_talker = NULL;
			}
			pTalkerStreamInfo->connection_count = 0;
		}
		node = openavbArrayIterNext(openavbAcmpSMTalkerVars.talkerStreamInfos);
	}
	openavbArrayDeleteArray(openavbAcmpSMTalkerVars.talkerStreamInfos);
	openavbAcmpSMTalkerVars.talkerStreamInfos = NULL;
	ACMP_SM_UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

void openavbAcmpSMTalkerSet_rcvdConnectTXCmd(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	if (!openavbAcmpSMTalkerCanQueueCmd(command, "CONNECT_TX_COMMAND")) {
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}
	ACMP_SM_LOCK();
	if (!bRunning) {
		ACMP_SM_UNLOCK();
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}

	memcpy(pRcvdCmdResp, command, sizeof(*command));
	openavbAcmpSMTalkerVars.rcvdConnectTX = TRUE;
	(void)openavbAcmpSMTalkerPostSemaphoreLocked("CONNECT_TX_COMMAND");

	ACMP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

void openavbAcmpSMTalkerSet_rcvdDisconnectTXCmd(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	if (!openavbAcmpSMTalkerCanQueueCmd(command, "DISCONNECT_TX_COMMAND")) {
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}
	ACMP_SM_LOCK();
	if (!bRunning) {
		ACMP_SM_UNLOCK();
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}

	memcpy(pRcvdCmdResp, command, sizeof(*command));
	openavbAcmpSMTalkerVars.rcvdDisconnectTX = TRUE;
	(void)openavbAcmpSMTalkerPostSemaphoreLocked("DISCONNECT_TX_COMMAND");

	ACMP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

void openavbAcmpSMTalkerSet_rcvdGetTXState(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	if (!openavbAcmpSMTalkerCanQueueCmd(command, "GET_TX_STATE_COMMAND")) {
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}
	ACMP_SM_LOCK();
	if (!bRunning) {
		ACMP_SM_UNLOCK();
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}

	memcpy(pRcvdCmdResp, command, sizeof(*command));
	openavbAcmpSMTalkerVars.rcvdGetTXState = TRUE;
	(void)openavbAcmpSMTalkerPostSemaphoreLocked("GET_TX_STATE_COMMAND");

	ACMP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

void openavbAcmpSMTalkerSet_rcvdGetTXConnectionCmd(openavb_acmp_ACMPCommandResponse_t *command)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	if (!openavbAcmpSMTalkerCanQueueCmd(command, "GET_TX_CONNECTION_COMMAND")) {
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}
	ACMP_SM_LOCK();
	if (!bRunning) {
		ACMP_SM_UNLOCK();
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}

	memcpy(pRcvdCmdResp, command, sizeof(*command));
	openavbAcmpSMTalkerVars.rcvdGetTXConnection = TRUE;
	(void)openavbAcmpSMTalkerPostSemaphoreLocked("GET_TX_CONNECTION_COMMAND");

	ACMP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}

void openavbAcmpSMTalkerSet_doTerminate(bool value)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);
	openavbAcmpSMTalkerVars.doTerminate = value;
	if (!bRunning) {
		AVB_TRACE_EXIT(AVB_TRACE_ACMP);
		return;
	}

	ACMP_SM_LOCK();
	if (bRunning) {
		(void)openavbAcmpSMTalkerPostSemaphoreLocked("TALKER_TERMINATE");
	}
	ACMP_SM_UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}


void openavbAcmpSMTalker_updateStreamInfo(openavb_tl_data_cfg_t *pCfg)
{
	AVB_TRACE_ENTRY(AVB_TRACE_ACMP);

	ACMP_SM_LOCK();

	// Find the talker stream waiting for the update.
	openavb_array_elem_t node = openavbArrayIterFirst(openavbAcmpSMTalkerVars.talkerStreamInfos);
	while (node) {
		openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo = openavbArrayData(node);
		if (pTalkerStreamInfo != NULL && pTalkerStreamInfo->waiting_on_talker != NULL) {
			U16 configIdx = openavbAemGetConfigIdx();
			S32 talker_unique_id = openavbArrayGetIdx(node);
			openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(configIdx, OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT, talker_unique_id);
			if (pDescriptorStreamOutput && pDescriptorStreamOutput->stream == pCfg) {

				// Verify that the destination address is valid.
				if (pCfg->dest_addr.mac == NULL ||
						memcmp(pCfg->dest_addr.buffer.ether_addr_octet, "\x00\x00\x00\x00\x00\x00", 6) == 0) {
					AVB_LOG_DEBUG("stream_dest_mac not yet valid.  Continue to wait.");
					continue;
				}

				// We will handle the GET_TX_CONNECTION_RESPONSE command response here.
				AVB_LOGF_DEBUG("Received an update for talker_unique_id %d", talker_unique_id);
				openavb_acmp_ACMPCommandResponse_t *response = pTalkerStreamInfo->waiting_on_talker;
				pTalkerStreamInfo->waiting_on_talker = NULL;

				// Update the talker stream information with the information from the configuration.
				memcpy(pTalkerStreamInfo->stream_id, pCfg->stream_addr.mac, ETH_ALEN);
				U8 *pStreamUID = pTalkerStreamInfo->stream_id + 6;
				*(U16 *)(pStreamUID) = htons(pCfg->stream_uid);
				memcpy(pTalkerStreamInfo->stream_dest_mac, pCfg->dest_addr.buffer.ether_addr_octet, ETH_ALEN);
				pTalkerStreamInfo->stream_vlan_id = pCfg->vlan_id;

				// Update the GET_TX_CONNECTION_RESPONSE command information.
				memcpy(response->stream_id, pTalkerStreamInfo->stream_id, sizeof(response->stream_id));
				memcpy(response->stream_dest_mac, pTalkerStreamInfo->stream_dest_mac, sizeof(response->stream_dest_mac));
				response->stream_vlan_id = pTalkerStreamInfo->stream_vlan_id;
				response->connection_count = pTalkerStreamInfo->connection_count;

				// Save the response flags for reference later.
				pDescriptorStreamOutput->acmp_flags = response->flags;

				// Save the stream information for reference later.
				// (This is currently only used by Listeners, but it doesn't hurt to have it just in case.)
				memcpy(pDescriptorStreamOutput->acmp_stream_id, response->stream_id, 8);
				memcpy(pDescriptorStreamOutput->acmp_dest_addr, response->stream_dest_mac, 6);
				pDescriptorStreamOutput->acmp_stream_vlan_id = response->stream_vlan_id;

				// Send the response.
				openavbAcmpSMTalker_txResponse(OPENAVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, response, OPENAVB_ACMP_STATUS_SUCCESS);
				AVB_LOG_DEBUG("Sent a CONNECT_TX_RESPONSE command");

				// Done with the response command.
				free(response);
				break;
			}
		}
		node = openavbArrayIterNext(openavbAcmpSMTalkerVars.talkerStreamInfos);
	}

	ACMP_SM_UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_ACMP);
}
