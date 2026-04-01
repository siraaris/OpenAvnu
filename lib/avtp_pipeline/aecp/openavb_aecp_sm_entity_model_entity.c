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
 * MODULE : AVDECC Enumeration and control protocol (AECP) : Entity Model Entity State Machine
 * MODULE SUMMARY : Implements the AVDECC Enumeration and control protocol (AECP) : Entity Model Entity State Machine
 * IEEE Std 1722.1-2013 clause 9.2.2.3
 ******************************************************************
 */

#include "openavb_platform.h"

#include <stdlib.h>
#include <errno.h>

#define	AVB_LOG_COMPONENT	"AECP"
#include "openavb_log.h"

#include "openavb_aem.h"
#include "openavb_aecp.h"
#include "openavb_aecp_message.h"
#include "openavb_aecp_sm_entity_model_entity.h"
#include "openavb_list.h"

#include "openavb_descriptor_avb_interface_pub.h"
#include "openavb_descriptor_stream_io_pub.h"
#include "openavb_descriptor_stream_port_io_pub.h"
#include "openavb_grandmaster_osal_pub.h"
#include "openavb_avdecc_pub.h"
#include "openavb_avdecc_read_ini_pub.h"

#include "openavb_acmp.h"
#include "openavb_acmp_sm_listener.h"
#include "openavb_avdecc_pipeline_interaction_pub.h"
#include "openavb_aecp_cmd_get_counters.h"
#include "openavb_time.h"

typedef enum {
	OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING,
	OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE,
	OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_RECEIVED_COMMAND,
} openavb_aecp_sm_entity_model_entity_state_t;

extern openavb_aecp_sm_global_vars_t openavbAecpSMGlobalVars;
extern openavb_avdecc_cfg_t gAvdeccCfg;
openavb_aecp_sm_entity_model_entity_vars_t openavbAecpSMEntityModelEntityVars;

extern MUTEX_HANDLE(openavbAemMutex);
#define AEM_LOCK() { MUTEX_CREATE_ERR(); MUTEX_LOCK(openavbAemMutex); MUTEX_LOG_ERR("Mutex lock failure"); }
#define AEM_UNLOCK() { MUTEX_CREATE_ERR(); MUTEX_UNLOCK(openavbAemMutex); MUTEX_LOG_ERR("Mutex unlock failure"); }

MUTEX_HANDLE(openavbAecpQueueMutex);
#define AECP_QUEUE_LOCK() { MUTEX_CREATE_ERR(); MUTEX_LOCK(openavbAecpQueueMutex); MUTEX_LOG_ERR("Mutex lock failure"); }
#define AECP_QUEUE_UNLOCK() { MUTEX_CREATE_ERR(); MUTEX_UNLOCK(openavbAecpQueueMutex); MUTEX_LOG_ERR("Mutex unlock failure"); }

MUTEX_HANDLE(openavbAecpSMMutex);
#define AECP_SM_LOCK() { MUTEX_CREATE_ERR(); MUTEX_LOCK(openavbAecpSMMutex); MUTEX_LOG_ERR("Mutex lock failure"); }
#define AECP_SM_UNLOCK() { MUTEX_CREATE_ERR(); MUTEX_UNLOCK(openavbAecpSMMutex); MUTEX_LOG_ERR("Mutex unlock failure"); }

SEM_T(openavbAecpSMEntityModelEntityWaitingSemaphore);
THREAD_TYPE(openavbAecpSMEntityModelEntityThread);
THREAD_DEFINITON(openavbAecpSMEntityModelEntityThread);


static openavb_list_t s_commandQueue = NULL;
static openavb_list_t s_unsolicitedQueue = NULL;
static openavb_list_t s_unsolicitedStateCache = NULL;
static bool s_identifyLogActive = FALSE;
static bool s_identifyLogTimeoutArmed = FALSE;
static struct timespec s_identifyLogTimeout;
static bool s_acquiredOwnerActivityValid = FALSE;
static bool s_lockedOwnerActivityValid = FALSE;
static bool s_unsolicitedControllerActivityValid = FALSE;
static struct timespec s_acquiredOwnerActivity;
static struct timespec s_lockedOwnerActivity;
static struct timespec s_unsolicitedControllerActivity;

#define OPENAVB_AECP_OWNERSHIP_TIMEOUT_SEC (60u)
#define OPENAVB_AECP_MILAN_PROTOCOL_VERSION (1u)
#define OPENAVB_AECP_MILAN_DEFAULT_MEDIA_CLOCK_PRIORITY (128u)
#define OPENAVB_AECP_MILAN_VERSION(major, minor, patch, build) \
	((((U32)(major) & 0xffu) << 24) | (((U32)(minor) & 0xffu) << 16) | (((U32)(patch) & 0xffu) << 8) | ((U32)(build) & 0xffu))

static const U32 s_openavbAecpMilanCertificationVersion = OPENAVB_AECP_MILAN_VERSION(0, 0, 0, 0);
static const U32 s_openavbAecpMilanSpecificationVersion = OPENAVB_AECP_MILAN_VERSION(1, 3, 0, 0);
static const U32 s_openavbAecpMilanFeaturesFlags = OPENAVB_AECP_MILAN_INFO_FEATURE_FLAG_MVU_BINDING;

static const U8 s_openavbAecpMvuProtocolId[OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH] = {
	0x00, 0x1b, 0xc5, 0x0a, 0xc1, 0x00
};
static bool s_openavbSystemUniqueIdValid = FALSE;
static U64 s_openavbSystemUniqueId = 0;
static U8 s_openavbSystemName[OPENAVB_AEM_STRLEN_MAX];

typedef struct {
	U8 message_type;
	U16 command_type;
	U16 descriptor_type;
	U16 descriptor_index;
	openavb_aecp_AEMCommandResponse_t response;
} openavb_aecp_unsolicited_state_cache_entry_t;

static U16 openavbAecpGetIdentifyTimeoutSec(void)
{
	return (gAvdeccCfg.identify_control_timeout_sec != 0) ?
		gAvdeccCfg.identify_control_timeout_sec : 10;
}

static void openavbAecpEnsureSystemUniqueId(void)
{
	U16 i;

	if (s_openavbSystemUniqueIdValid) {
		return;
	}

	s_openavbSystemUniqueId = 0;
	if (gAvdeccCfg.pDescriptorEntity) {
		for (i = 0; i < sizeof(gAvdeccCfg.pDescriptorEntity->entity_id); i++) {
			s_openavbSystemUniqueId <<= 8;
			s_openavbSystemUniqueId |= gAvdeccCfg.pDescriptorEntity->entity_id[i];
		}
	}

	memset(s_openavbSystemName, 0, sizeof(s_openavbSystemName));
	if (gAvdeccCfg.entity_name[0] != '\0') {
		size_t entityNameLen = strnlen(gAvdeccCfg.entity_name, sizeof(s_openavbSystemName) - 1);
		memcpy(s_openavbSystemName, gAvdeccCfg.entity_name, entityNameLen);
	}

	s_openavbSystemUniqueIdValid = TRUE;
}

static void openavbAecpCopyMvuProtocolId(U8 protocol_id[OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH]);
static void processVendorUniqueCommand(void);

static U16 openavbAecpNextUnsolicitedSequenceIdLocked(U8 messageType)
{
	switch (messageType) {
		case OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND:
		case OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE:
			return ++openavbAecpSMEntityModelEntityVars.unsolicitedMvuSequenceId;

		case OPENAVB_AECP_MESSAGE_TYPE_AEM_COMMAND:
		case OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE:
		default:
			return ++openavbAecpSMEntityModelEntityVars.unsolicitedAemSequenceId;
	}
}

// Returns 1 if the queue was not empty before adding the new command,
//  0 if the queue was empty before adding the new command,
//  or -1 if an error occurred.
static int addCommandToQueue(openavb_aecp_AEMCommandResponse_t *command)
{
	int returnVal;

	if (!s_commandQueue) { return -1; }
	if (!command) { return -1; }

	AECP_QUEUE_LOCK();
	// Determine if the queue has something in it.
	returnVal = (openavbListFirst(s_commandQueue) != NULL ? 1 : 0);

	// Add the command to the end of the linked list.
	if (openavbListAdd(s_commandQueue, command) == NULL) {
		returnVal = -1;
	}
	AECP_QUEUE_UNLOCK();
	return (returnVal);
}

static openavb_aecp_AEMCommandResponse_t * getNextCommandFromQueue(void)
{
	openavb_aecp_AEMCommandResponse_t *item = NULL;
	openavb_list_node_t node;

	if (!s_commandQueue) { return NULL; }

	AECP_QUEUE_LOCK();
	node = openavbListFirst(s_commandQueue);
	if (node) {
		item = openavbListData(node);
		openavbListDelete(s_commandQueue, node);
	}
	AECP_QUEUE_UNLOCK();
	return item;
}

static bool hasQueuedCommands(void)
{
	bool hasCommands = FALSE;

	if (!s_commandQueue) {
		return FALSE;
	}

	AECP_QUEUE_LOCK();
	hasCommands = (openavbListFirst(s_commandQueue) != NULL);
	AECP_QUEUE_UNLOCK();
	return hasCommands;
}

// Returns 1 if the queue was not empty before adding the new response,
//  0 if the queue was empty before adding the new response,
//  or -1 if an error occurred.
static int addUnsolicitedToQueue(const openavb_aecp_AEMCommandResponse_t *response)
{
	int returnVal;
	openavb_aecp_AEMCommandResponse_t *item;

	if (!s_unsolicitedQueue) { return -1; }
	if (!response) { return -1; }

	item = calloc(1, sizeof(*item));
	if (!item) { return -1; }
	memcpy(item, response, sizeof(*item));

	AECP_QUEUE_LOCK();
	returnVal = (openavbListFirst(s_unsolicitedQueue) != NULL ? 1 : 0);
	if (openavbListAdd(s_unsolicitedQueue, item) == NULL) {
		returnVal = -1;
	}
	AECP_QUEUE_UNLOCK();

	if (returnVal < 0) {
		free(item);
	}
	return returnVal;
}

static openavb_aecp_AEMCommandResponse_t *getNextUnsolicitedFromQueue(void)
{
	openavb_aecp_AEMCommandResponse_t *item = NULL;
	openavb_list_node_t node;

	if (!s_unsolicitedQueue) { return NULL; }

	AECP_QUEUE_LOCK();
	node = openavbListFirst(s_unsolicitedQueue);
	if (node) {
		item = openavbListData(node);
		openavbListDelete(s_unsolicitedQueue, node);
	}
	AECP_QUEUE_UNLOCK();
	return item;
}

static bool hasQueuedUnsolicited(void)
{
	bool hasItems = FALSE;

	if (!s_unsolicitedQueue) {
		return FALSE;
	}

	AECP_QUEUE_LOCK();
	hasItems = (openavbListFirst(s_unsolicitedQueue) != NULL);
	AECP_QUEUE_UNLOCK();
	return hasItems;
}

static bool openavbAecpQueueUnsolicitedResponseLocked(const openavb_aecp_AEMCommandResponse_t *pResponse)
{
	int result;
	openavb_aecp_AEMCommandResponse_t queuedResponse;

	if (!pResponse) {
		return FALSE;
	}

	queuedResponse = *pResponse;
	queuedResponse.commonPdu.sequence_id =
		openavbAecpNextUnsolicitedSequenceIdLocked(queuedResponse.headers.message_type);

	result = addUnsolicitedToQueue(&queuedResponse);

	if (result >= 0) {
		openavbAecpSMEntityModelEntityVars.doUnsolicited = TRUE;
		return TRUE;
	}
	return FALSE;
}

static void openavbAecpNormalizeUnsolicitedCounters(
	U16 descriptor_type,
	openavb_aecp_AEMCommandResponse_t *pResponse)
{
	openavb_aecp_response_data_get_counters_t *pCountersRsp;
	U32 keepMask = 0;
	U8 normalizedBlock[sizeof(((openavb_aecp_response_data_get_counters_t *)0)->counters_block)] = {0};

	if (!pResponse ||
			pResponse->entityModelPdu.command_type != OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS) {
		return;
	}

	pCountersRsp = &pResponse->entityModelPdu.command_data.getCountersRsp;
	switch (descriptor_type) {
		case OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT:
			keepMask =
				OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_COUNTER_MEDIA_LOCKED |
				OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_COUNTER_MEDIA_UNLOCKED |
				OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_COUNTER_STREAM_RESET;
			if ((pCountersRsp->counters_valid & OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_COUNTER_MEDIA_LOCKED) != 0) {
				memcpy(&normalizedBlock[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_OFFSET_MEDIA_LOCKED],
					&pCountersRsp->counters_block[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_OFFSET_MEDIA_LOCKED],
					sizeof(U32));
			}
			if ((pCountersRsp->counters_valid & OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_COUNTER_MEDIA_UNLOCKED) != 0) {
				memcpy(&normalizedBlock[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_OFFSET_MEDIA_UNLOCKED],
					&pCountersRsp->counters_block[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_OFFSET_MEDIA_UNLOCKED],
					sizeof(U32));
			}
			if ((pCountersRsp->counters_valid & OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_COUNTER_STREAM_RESET) != 0) {
				memcpy(&normalizedBlock[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_OFFSET_STREAM_RESET],
					&pCountersRsp->counters_block[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_INPUT_OFFSET_STREAM_RESET],
					sizeof(U32));
			}
			break;

		case OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT:
			keepMask =
				OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_COUNTER_STREAM_START |
				OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_COUNTER_STREAM_STOP |
				OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_COUNTER_STREAM_INTERRUPTED;
			if ((pCountersRsp->counters_valid & OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_COUNTER_STREAM_START) != 0) {
				memcpy(&normalizedBlock[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_OFFSET_STREAM_START],
					&pCountersRsp->counters_block[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_OFFSET_STREAM_START],
					sizeof(U32));
			}
			if ((pCountersRsp->counters_valid & OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_COUNTER_STREAM_STOP) != 0) {
				memcpy(&normalizedBlock[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_OFFSET_STREAM_STOP],
					&pCountersRsp->counters_block[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_OFFSET_STREAM_STOP],
					sizeof(U32));
			}
			if ((pCountersRsp->counters_valid & OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_COUNTER_STREAM_INTERRUPTED) != 0) {
				memcpy(&normalizedBlock[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_OFFSET_STREAM_INTERRUPTED],
					&pCountersRsp->counters_block[OPENAVB_AEM_GET_COUNTERS_COMMAND_STREAM_OUTPUT_OFFSET_STREAM_INTERRUPTED],
					sizeof(U32));
			}
			break;

		default:
			return;
	}

	pCountersRsp->counters_valid &= keepMask;
	memcpy(pCountersRsp->counters_block, normalizedBlock, sizeof(pCountersRsp->counters_block));
}

static void openavbAecpNormalizeUnsolicitedResponse(
	U16 descriptor_type,
	bool normalizeCounters,
	openavb_aecp_AEMCommandResponse_t *pResponse)
{
	if (!pResponse) {
		return;
	}

	memset(pResponse->host, 0, sizeof(pResponse->host));
	memset(pResponse->headers.target_entity_id, 0, sizeof(pResponse->headers.target_entity_id));
	memset(pResponse->commonPdu.controller_entity_id, 0, sizeof(pResponse->commonPdu.controller_entity_id));
	pResponse->commonPdu.sequence_id = 0;
	if (normalizeCounters) {
		openavbAecpNormalizeUnsolicitedCounters(descriptor_type, pResponse);
	}
}

static void openavbAecpResetUnsolicitedStateCacheLocked(void)
{
	if (s_unsolicitedStateCache) {
		openavbListDeleteList(s_unsolicitedStateCache);
	}
	s_unsolicitedStateCache = openavbListNewList();
}

static bool openavbAecpQueueChangedUnsolicitedResponseLocked(
	U16 descriptor_type,
	U16 descriptor_index,
	bool normalizeCounters,
	const openavb_aecp_AEMCommandResponse_t *pResponse)
{
	openavb_aecp_AEMCommandResponse_t normalized;
	openavb_list_node_t node;

	if (!pResponse) {
		return FALSE;
	}

	if (!s_unsolicitedStateCache) {
		s_unsolicitedStateCache = openavbListNewList();
		if (!s_unsolicitedStateCache) {
			return FALSE;
		}
	}

	normalized = *pResponse;
	openavbAecpNormalizeUnsolicitedResponse(descriptor_type, normalizeCounters, &normalized);

	node = openavbListIterFirst(s_unsolicitedStateCache);
	while (node) {
		openavb_aecp_unsolicited_state_cache_entry_t *pEntry = openavbListData(node);
		if (pEntry &&
				pEntry->message_type == normalized.headers.message_type &&
				pEntry->command_type == normalized.entityModelPdu.command_type &&
				pEntry->descriptor_type == descriptor_type &&
				pEntry->descriptor_index == descriptor_index) {
			if (memcmp(&pEntry->response, &normalized, sizeof(normalized)) == 0) {
				return FALSE;
			}

			pEntry->response = normalized;
			return openavbAecpQueueUnsolicitedResponseLocked(pResponse);
		}
		node = openavbListIterNext(s_unsolicitedStateCache);
	}

	node = openavbListNew(s_unsolicitedStateCache, sizeof(openavb_aecp_unsolicited_state_cache_entry_t));
	if (!node) {
		return FALSE;
	}

	openavb_aecp_unsolicited_state_cache_entry_t *pEntry = openavbListData(node);
	if (!pEntry) {
		openavbListDelete(s_unsolicitedStateCache, node);
		return FALSE;
	}

	memset(pEntry, 0, sizeof(*pEntry));
	pEntry->message_type = normalized.headers.message_type;
	pEntry->command_type = normalized.entityModelPdu.command_type;
	pEntry->descriptor_type = descriptor_type;
	pEntry->descriptor_index = descriptor_index;
	pEntry->response = normalized;

	// Prime the cache on first observation after registration/reset, but do not
	// emit an unsolicited notification for that initial snapshot.
	return FALSE;
}

static bool openavbAecpIsIdentifyControl(const openavb_aem_descriptor_control_t *pDescriptorControl)
{
	return (pDescriptorControl &&
		pDescriptorControl->control_type == OPENAVB_AEM_CONTROL_TYPE_IDENTIFY &&
		pDescriptorControl->control_value_type == OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8);
}

static bool openavbAecpCanBypassControllerRestrictionForControl(
	const openavb_aem_descriptor_control_t *pDescriptorControl)
{
	return openavbAecpIsIdentifyControl(pDescriptorControl);
}

static void openavbAecpPopulateSetControlResponse(
	openavb_aecp_commandresponse_data_set_control_t *pRsp,
	const openavb_aem_descriptor_control_t *pDescriptorControl)
{
	int i1;

	if (!pRsp || !pDescriptorControl) {
		return;
	}

	pRsp->descriptor_type = pDescriptorControl->descriptor_type;
	pRsp->descriptor_index = pDescriptorControl->descriptor_index;
	pRsp->valuesCount = pDescriptorControl->number_of_values;

	for (i1 = 0; i1 < pDescriptorControl->number_of_values; i1++) {
		switch (pDescriptorControl->control_value_type) {
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT8:
				pRsp->values.linear_int8[i1] = pDescriptorControl->value_details.linear_int8[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8:
				pRsp->values.linear_uint8[i1] = pDescriptorControl->value_details.linear_uint8[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT16:
				pRsp->values.linear_int16[i1] = pDescriptorControl->value_details.linear_int16[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT16:
				pRsp->values.linear_uint16[i1] = pDescriptorControl->value_details.linear_uint16[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT32:
				pRsp->values.linear_int32[i1] = pDescriptorControl->value_details.linear_int32[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT32:
				pRsp->values.linear_uint32[i1] = pDescriptorControl->value_details.linear_uint32[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT64:
				pRsp->values.linear_int64[i1] = pDescriptorControl->value_details.linear_int64[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT64:
				pRsp->values.linear_uint64[i1] = pDescriptorControl->value_details.linear_uint64[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_FLOAT:
				pRsp->values.linear_float[i1] = pDescriptorControl->value_details.linear_float[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_DOUBLE:
				pRsp->values.linear_double[i1] = pDescriptorControl->value_details.linear_double[i1].current;
				break;
			default:
				break;
		}
	}
}

static void openavbAecpPopulateGetControlResponse(
	openavb_aecp_response_data_get_control_t *pRsp,
	const openavb_aem_descriptor_control_t *pDescriptorControl)
{
	int i1;

	if (!pRsp || !pDescriptorControl) {
		return;
	}

	pRsp->descriptor_type = pDescriptorControl->descriptor_type;
	pRsp->descriptor_index = pDescriptorControl->descriptor_index;
	pRsp->valuesCount = pDescriptorControl->number_of_values;

	for (i1 = 0; i1 < pDescriptorControl->number_of_values; i1++) {
		switch (pDescriptorControl->control_value_type) {
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT8:
				pRsp->values.linear_int8[i1] = pDescriptorControl->value_details.linear_int8[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8:
				pRsp->values.linear_uint8[i1] = pDescriptorControl->value_details.linear_uint8[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT16:
				pRsp->values.linear_int16[i1] = pDescriptorControl->value_details.linear_int16[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT16:
				pRsp->values.linear_uint16[i1] = pDescriptorControl->value_details.linear_uint16[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT32:
				pRsp->values.linear_int32[i1] = pDescriptorControl->value_details.linear_int32[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT32:
				pRsp->values.linear_uint32[i1] = pDescriptorControl->value_details.linear_uint32[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT64:
				pRsp->values.linear_int64[i1] = pDescriptorControl->value_details.linear_int64[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT64:
				pRsp->values.linear_uint64[i1] = pDescriptorControl->value_details.linear_uint64[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_FLOAT:
				pRsp->values.linear_float[i1] = pDescriptorControl->value_details.linear_float[i1].current;
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_DOUBLE:
				pRsp->values.linear_double[i1] = pDescriptorControl->value_details.linear_double[i1].current;
				break;
			default:
				break;
		}
	}
}

static bool openavbAecpSetDescriptorControlValues(
	openavb_aem_descriptor_control_t *pDescriptorControl,
	const openavb_aecp_commandresponse_data_set_control_t *pCmd,
	bool *pIdentifyChanged)
{
	int i1;

	if (!pDescriptorControl || !pCmd) {
		return FALSE;
	}

	if (pIdentifyChanged) {
		*pIdentifyChanged = FALSE;
	}

	if (openavbAecpIsIdentifyControl(pDescriptorControl)) {
		U8 newValue;
		if (pCmd->valuesCount < 1) {
			return FALSE;
		}

		newValue = (pCmd->values.linear_uint8[0] != 0) ? 1 : 0;
		if (pIdentifyChanged) {
			*pIdentifyChanged = (pDescriptorControl->value_details.linear_uint8[0].current != newValue);
		}
		pDescriptorControl->number_of_values = 1;
		pDescriptorControl->value_details.linear_uint8[0].current = newValue;
		return TRUE;
	}

	pDescriptorControl->number_of_values = pCmd->valuesCount;
	for (i1 = 0; i1 < pCmd->valuesCount; i1++) {
		switch (pDescriptorControl->control_value_type) {
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT8:
				pDescriptorControl->value_details.linear_int8[i1].current = pCmd->values.linear_int8[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8:
				pDescriptorControl->value_details.linear_uint8[i1].current = pCmd->values.linear_uint8[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT16:
				pDescriptorControl->value_details.linear_int16[i1].current = pCmd->values.linear_int16[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT16:
				pDescriptorControl->value_details.linear_uint16[i1].current = pCmd->values.linear_uint16[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT32:
				pDescriptorControl->value_details.linear_int32[i1].current = pCmd->values.linear_int32[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT32:
				pDescriptorControl->value_details.linear_uint32[i1].current = pCmd->values.linear_uint32[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT64:
				pDescriptorControl->value_details.linear_int64[i1].current = pCmd->values.linear_int64[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT64:
				pDescriptorControl->value_details.linear_uint64[i1].current = pCmd->values.linear_uint64[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_FLOAT:
				pDescriptorControl->value_details.linear_float[i1].current = pCmd->values.linear_float[i1];
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_DOUBLE:
				pDescriptorControl->value_details.linear_double[i1].current = pCmd->values.linear_double[i1];
				break;
			default:
				return FALSE;
		}
	}

	return TRUE;
}

static void openavbAecpRegisterUnsolicitedController(const openavb_aecp_AEMCommandResponse_t *pCommand)
{
	bool sameController = FALSE;

	if (!pCommand) {
		return;
	}

	sameController =
		openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered &&
		memcmp(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId,
			pCommand->commonPdu.controller_entity_id,
			sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId)) == 0 &&
		memcmp(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac,
			pCommand->host,
			sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac)) == 0;

	openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered = TRUE;
	memcpy(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId,
		pCommand->commonPdu.controller_entity_id,
		sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId));
	memcpy(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac,
		pCommand->host,
		sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac));
	CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &s_unsolicitedControllerActivity);
	s_unsolicitedControllerActivityValid = TRUE;
	if (!sameController) {
		openavbAecpResetUnsolicitedStateCacheLocked();
	}
}

static bool openavbAecpIsRegisteredUnsolicitedController(const openavb_aecp_AEMCommandResponse_t *pCommand)
{
	if (!pCommand || !openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return FALSE;
	}

	return (memcmp(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId,
		pCommand->commonPdu.controller_entity_id,
		sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId)) == 0);
}

static void openavbAecpClearUnsolicitedController(void)
{
	openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered = FALSE;
	memset(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId, 0,
		sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId));
	memset(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac, 0,
		sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac));
	s_unsolicitedControllerActivityValid = FALSE;
	memset(&s_unsolicitedControllerActivity, 0, sizeof(s_unsolicitedControllerActivity));
	openavbAecpResetUnsolicitedStateCacheLocked();
}

static void openavbAecpQueueIdentifyNotificationLocked(void)
{
	openavb_aecp_AEMCommandResponse_t unsolicited;

	if (!openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return;
	}

	memset(&unsolicited, 0, sizeof(unsolicited));
	memcpy(unsolicited.host,
		openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac,
		sizeof(unsolicited.host));
	unsolicited.headers.cd = 1;
	unsolicited.headers.subtype = OPENAVB_AECP_AVTP_SUBTYPE;
	unsolicited.headers.sv = 0;
	unsolicited.headers.version = 0;
	unsolicited.headers.message_type = OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE;
	unsolicited.headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
	memcpy(unsolicited.headers.target_entity_id,
		openavbAecpSMGlobalVars.myEntityID,
		sizeof(unsolicited.headers.target_entity_id));
	memcpy(unsolicited.commonPdu.controller_entity_id,
		openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId,
		sizeof(unsolicited.commonPdu.controller_entity_id));
	unsolicited.commonPdu.sequence_id =
		openavbAecpNextUnsolicitedSequenceIdLocked(OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	unsolicited.entityModelPdu.u = 1;
	unsolicited.entityModelPdu.command_type = OPENAVB_AEM_COMMAND_CODE_IDENTIFY_NOTIFICATION;
	(void)openavbAecpQueueUnsolicitedResponseLocked(&unsolicited);
}

static void openavbAecpArmIdentifyTimeoutLocked(void)
{
	U16 timeoutSec = openavbAecpGetIdentifyTimeoutSec();

	CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &s_identifyLogTimeout);
	openavbTimeTimespecAddUsec(&s_identifyLogTimeout,
		(U32)timeoutSec * MICROSECONDS_PER_SECOND);
	s_identifyLogTimeoutArmed = TRUE;
}

static void openavbAecpApplyIdentifyActionLocked(bool identifyOn, bool identifyChanged)
{
	U16 timeoutSec = openavbAecpGetIdentifyTimeoutSec();

	if (identifyOn) {
		if (identifyChanged) {
			AVB_LOGF_INFO("IDENTIFY action start: logging enabled for %u seconds",
				timeoutSec);
		}
		else if (s_identifyLogActive) {
			AVB_LOGF_INFO("IDENTIFY action refresh: extending logging window to %u seconds",
				timeoutSec);
		}

		s_identifyLogActive = TRUE;
		openavbAecpArmIdentifyTimeoutLocked();
	}
	else {
		if (identifyChanged && s_identifyLogActive) {
			AVB_LOG_INFO("IDENTIFY action stop: control set to 0");
		}

		s_identifyLogActive = FALSE;
		s_identifyLogTimeoutArmed = FALSE;
	}
}

static bool openavbAecpHandleIdentifyTimeoutLocked(void)
{
	struct timespec now;
	openavb_aem_descriptor_control_t *pIdentifyControl;

	if (!s_identifyLogTimeoutArmed) {
		return FALSE;
	}

	CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &now);
	if (openavbTimeTimespecCmp(&now, &s_identifyLogTimeout) < 0) {
		return FALSE;
	}

	s_identifyLogTimeoutArmed = FALSE;
	if (!s_identifyLogActive) {
		return FALSE;
	}

	s_identifyLogActive = FALSE;
	AVB_LOGF_INFO("IDENTIFY action stop: timeout after %u seconds",
		openavbAecpGetIdentifyTimeoutSec());

	pIdentifyControl = openavbAemGetDescriptor(openavbAemGetConfigIdx(),
		OPENAVB_AEM_DESCRIPTOR_CONTROL,
		openavbAvdeccGetIdentifyControlIndex());
	if (openavbAecpIsIdentifyControl(pIdentifyControl) &&
			pIdentifyControl->value_details.linear_uint8[0].current != 0) {
		pIdentifyControl->value_details.linear_uint8[0].current = 0;
		openavbAecpQueueIdentifyNotificationLocked();
	}

	return TRUE;
}

static bool openavbAecpControllerActivityExpired(const struct timespec *pLastActivity, bool valid)
{
	struct timespec now;
	struct timespec expiresAt;

	if (!valid || !pLastActivity) {
		return FALSE;
	}

	CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &now);
	expiresAt = *pLastActivity;
	openavbTimeTimespecAddUsec(&expiresAt,
		OPENAVB_AECP_OWNERSHIP_TIMEOUT_SEC * MICROSECONDS_PER_SECOND);
	return (openavbTimeTimespecCmp(&now, &expiresAt) >= 0);
}

static bool openavbAecpGetControllerActivityDeadline(
	struct timespec *pLastActivity,
	bool valid,
	struct timespec *pDeadline)
{
	if (!valid || !pLastActivity || !pDeadline) {
		return FALSE;
	}

	*pDeadline = *pLastActivity;
	openavbTimeTimespecAddUsec(pDeadline,
		OPENAVB_AECP_OWNERSHIP_TIMEOUT_SEC * MICROSECONDS_PER_SECOND);
	return TRUE;
}

static void openavbAecpSelectEarlierDeadline(
	struct timespec *pCandidate,
	bool *pDeadlineValid,
	struct timespec *pDeadline)
{
	if (!pCandidate || !pDeadlineValid || !pDeadline) {
		return;
	}

	if (!*pDeadlineValid || openavbTimeTimespecCmp(pCandidate, pDeadline) < 0) {
		*pDeadline = *pCandidate;
		*pDeadlineValid = TRUE;
	}
}

static void openavbAecpClearAcquiredOwnerActivityLocked(void)
{
	s_acquiredOwnerActivityValid = FALSE;
	memset(&s_acquiredOwnerActivity, 0, sizeof(s_acquiredOwnerActivity));
}

static void openavbAecpClearLockedOwnerActivityLocked(void)
{
	s_lockedOwnerActivityValid = FALSE;
	memset(&s_lockedOwnerActivity, 0, sizeof(s_lockedOwnerActivity));
}

static void openavbAecpMarkAcquiredOwnerActivityLocked(void)
{
	CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &s_acquiredOwnerActivity);
	s_acquiredOwnerActivityValid = TRUE;
}

static void openavbAecpMarkLockedOwnerActivityLocked(void)
{
	CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &s_lockedOwnerActivity);
	s_lockedOwnerActivityValid = TRUE;
}

static void openavbAecpClearEntityAcquiredLocked(openavb_avdecc_entity_model_t *pAem, const char *reason)
{
	if (!pAem || !pAem->entityAcquired) {
		return;
	}

	if (reason) {
		AVB_LOGF_INFO("Clearing stale entity acquisition held by " ENTITYID_FORMAT " (%s)",
			pAem->acquiredControllerId,
			reason);
	}
	pAem->entityAcquired = FALSE;
	memset(pAem->acquiredControllerId, 0, sizeof(pAem->acquiredControllerId));
	openavbAecpClearAcquiredOwnerActivityLocked();
}

static void openavbAecpClearEntityLockedLocked(openavb_avdecc_entity_model_t *pAem, const char *reason)
{
	if (!pAem || !pAem->entityLocked) {
		return;
	}

	if (reason) {
		AVB_LOGF_INFO("Clearing stale entity lock held by " ENTITYID_FORMAT " (%s)",
			pAem->lockedControllerId,
			reason);
	}
	pAem->entityLocked = FALSE;
	memset(pAem->lockedControllerId, 0, sizeof(pAem->lockedControllerId));
	openavbAecpClearLockedOwnerActivityLocked();
}

static bool openavbAecpExpireOwnershipLocked(openavb_avdecc_entity_model_t *pAem)
{
	bool changed = FALSE;

	if (!pAem) {
		return FALSE;
	}

	if (pAem->entityAcquired &&
			openavbAecpControllerActivityExpired(&s_acquiredOwnerActivity, s_acquiredOwnerActivityValid)) {
		openavbAecpClearEntityAcquiredLocked(pAem, "owner activity timeout");
		changed = TRUE;
	}

	if (pAem->entityLocked &&
			openavbAecpControllerActivityExpired(&s_lockedOwnerActivity, s_lockedOwnerActivityValid)) {
		openavbAecpClearEntityLockedLocked(pAem, "owner activity timeout");
		changed = TRUE;
	}

	return changed;
}

static bool openavbAecpHandleControllerTimeoutsLocked(void)
{
	bool changed = FALSE;
	openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();

	if (pAem) {
		AEM_LOCK();
		changed = openavbAecpExpireOwnershipLocked(pAem) || changed;
		AEM_UNLOCK();
	}

	return changed;
}

static bool openavbAecpHandleMaintenanceTimeoutsLocked(void)
{
	bool changed = FALSE;

	if (openavbAecpHandleControllerTimeoutsLocked()) {
		changed = TRUE;
	}
	if (openavbAecpHandleIdentifyTimeoutLocked()) {
		changed = TRUE;
	}

	return changed;
}

static bool openavbAecpGetNextMaintenanceDeadlineLocked(struct timespec *pDeadline)
{
	struct timespec candidate;
	bool haveDeadline = FALSE;

	if (!pDeadline) {
		return FALSE;
	}

	if (s_identifyLogTimeoutArmed) {
		*pDeadline = s_identifyLogTimeout;
		haveDeadline = TRUE;
	}

	if (openavbAecpGetControllerActivityDeadline(&s_acquiredOwnerActivity,
			s_acquiredOwnerActivityValid, &candidate)) {
		openavbAecpSelectEarlierDeadline(&candidate, &haveDeadline, pDeadline);
	}
	if (openavbAecpGetControllerActivityDeadline(&s_lockedOwnerActivity,
			s_lockedOwnerActivityValid, &candidate)) {
		openavbAecpSelectEarlierDeadline(&candidate, &haveDeadline, pDeadline);
	}
	return haveDeadline;
}

static void openavbAecpNoteControllerActivityLocked(const openavb_aecp_AEMCommandResponse_t *pCommand)
{
	openavb_avdecc_entity_model_t *pAem;

	if (!pCommand) {
		return;
	}

	pAem = openavbAemGetModel();
	if (!pAem) {
		return;
	}

	AEM_LOCK();
	openavbAecpExpireOwnershipLocked(pAem);
	if (pAem->entityAcquired &&
			memcmp(pAem->acquiredControllerId,
				pCommand->commonPdu.controller_entity_id,
				sizeof(pAem->acquiredControllerId)) == 0) {
		openavbAecpMarkAcquiredOwnerActivityLocked();
	}
	if (pAem->entityLocked &&
			memcmp(pAem->lockedControllerId,
				pCommand->commonPdu.controller_entity_id,
				sizeof(pAem->lockedControllerId)) == 0) {
		openavbAecpMarkLockedOwnerActivityLocked();
	}
	AEM_UNLOCK();

	if (openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered &&
			memcmp(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId,
				pCommand->commonPdu.controller_entity_id,
				sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId)) == 0) {
		memcpy(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac,
			pCommand->host,
			sizeof(openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac));
		CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &s_unsolicitedControllerActivity);
		s_unsolicitedControllerActivityValid = TRUE;
	}
}

static void openavbAecpBumpAvailableIndexLocked(void)
{
	openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();

	if (!pAem || !pAem->pDescriptorEntity) {
		return;
	}

	AEM_LOCK();
	pAem->pDescriptorEntity->available_index++;
	AEM_UNLOCK();
}

static void openavbAecpPrepareUnsolicitedBaseLocked(
	openavb_aecp_AEMCommandResponse_t *pCommand,
	U8 messageType,
	U16 commandType)
{
	if (!pCommand) {
		return;
	}

	memset(pCommand, 0, sizeof(*pCommand));
	memcpy(pCommand->host,
		openavbAecpSMEntityModelEntityVars.unsolicitedControllerMac,
		sizeof(pCommand->host));
	pCommand->headers.cd = 1;
	pCommand->headers.subtype = OPENAVB_AECP_AVTP_SUBTYPE;
	pCommand->headers.sv = 0;
	pCommand->headers.version = 0;
	pCommand->headers.message_type = messageType;
	memcpy(pCommand->headers.target_entity_id,
		openavbAecpSMGlobalVars.myEntityID,
		sizeof(pCommand->headers.target_entity_id));
	memcpy(pCommand->commonPdu.controller_entity_id,
		openavbAecpSMEntityModelEntityVars.unsolicitedControllerEntityId,
		sizeof(pCommand->commonPdu.controller_entity_id));
	pCommand->commonPdu.sequence_id = 0;
	pCommand->entityModelPdu.command_type = commandType;
	pCommand->entityModelPdu.u = 1;
}

static bool openavbAecpBuildUnsolicitedStreamInfoLocked(
	U16 descriptor_type,
	U16 descriptor_index,
	openavb_aecp_AEMCommandResponse_t *pUnsolicited)
{
	openavb_aecp_AEMCommandResponse_t *pSavedCommand;

	if (!pUnsolicited || !openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return FALSE;
	}

	openavbAecpPrepareUnsolicitedBaseLocked(pUnsolicited,
		OPENAVB_AECP_MESSAGE_TYPE_AEM_COMMAND,
		OPENAVB_AEM_COMMAND_CODE_GET_STREAM_INFO);
	pUnsolicited->entityModelPdu.command_data.getStreamInfoCmd.descriptor_type = descriptor_type;
	pUnsolicited->entityModelPdu.command_data.getStreamInfoCmd.descriptor_index = descriptor_index;

	pSavedCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	openavbAecpSMEntityModelEntityVars.rcvdCommand = pUnsolicited;
	processCommand();
	openavbAecpSMEntityModelEntityVars.rcvdCommand = pSavedCommand;
	pUnsolicited->entityModelPdu.u = 1;

	return (pUnsolicited->headers.status == OPENAVB_AEM_COMMAND_STATUS_SUCCESS);
}

static bool openavbAecpBuildUnsolicitedStreamInputInfoExLocked(
	U16 descriptor_index,
	openavb_aecp_AEMCommandResponse_t *pUnsolicited)
{
	openavb_aecp_AEMCommandResponse_t *pSavedCommand;

	if (!pUnsolicited || !openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return FALSE;
	}

	openavbAecpPrepareUnsolicitedBaseLocked(pUnsolicited,
		OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND,
		OPENAVB_AECP_MVU_COMMAND_TYPE_GET_STREAM_INPUT_INFO_EX);
	openavbAecpCopyMvuProtocolId(pUnsolicited->protocol_id);
	pUnsolicited->entityModelPdu.command_data.getStreamInputInfoExCmd.descriptor_type =
		OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT;
	pUnsolicited->entityModelPdu.command_data.getStreamInputInfoExCmd.descriptor_index =
		descriptor_index;

	pSavedCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	openavbAecpSMEntityModelEntityVars.rcvdCommand = pUnsolicited;
	processVendorUniqueCommand();
	openavbAecpSMEntityModelEntityVars.rcvdCommand = pSavedCommand;
	pUnsolicited->entityModelPdu.u = 1;

	return (pUnsolicited->headers.status == OPENAVB_AECP_MVU_STATUS_SUCCESS);
}

static bool openavbAecpBuildUnsolicitedGetCountersLocked(
	U16 descriptor_type,
	U16 descriptor_index,
	openavb_aecp_AEMCommandResponse_t *pUnsolicited)
{
	openavb_aecp_AEMCommandResponse_t *pSavedCommand;

	if (!pUnsolicited || !openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return FALSE;
	}

	openavbAecpPrepareUnsolicitedBaseLocked(pUnsolicited,
		OPENAVB_AECP_MESSAGE_TYPE_AEM_COMMAND,
		OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS);
	pUnsolicited->entityModelPdu.command_data.getCountersCmd.descriptor_type = descriptor_type;
	pUnsolicited->entityModelPdu.command_data.getCountersCmd.descriptor_index = descriptor_index;

	pSavedCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	openavbAecpSMEntityModelEntityVars.rcvdCommand = pUnsolicited;
	processCommand();
	openavbAecpSMEntityModelEntityVars.rcvdCommand = pSavedCommand;
	pUnsolicited->entityModelPdu.u = 1;

	return (pUnsolicited->headers.status == OPENAVB_AEM_COMMAND_STATUS_SUCCESS);
}

static bool openavbAecpNotifyStreamStateChangedLocked(U16 descriptor_type, U16 descriptor_index)
{
	bool queued = FALSE;
	openavb_aecp_AEMCommandResponse_t unsolicited;

	if (!openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return FALSE;
	}

	if (openavbAecpBuildUnsolicitedStreamInfoLocked(descriptor_type, descriptor_index, &unsolicited)) {
		bool queuedThis = openavbAecpQueueChangedUnsolicitedResponseLocked(
			descriptor_type, descriptor_index, TRUE, &unsolicited);
		queued = queuedThis || queued;
	}

	if (descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT &&
			openavbAecpBuildUnsolicitedStreamInputInfoExLocked(descriptor_index, &unsolicited)) {
		bool queuedThis = openavbAecpQueueChangedUnsolicitedResponseLocked(
			descriptor_type, descriptor_index, TRUE, &unsolicited);
		queued = queuedThis || queued;
	}

	if ((descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT ||
			descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) &&
			openavbAecpBuildUnsolicitedGetCountersLocked(descriptor_type, descriptor_index, &unsolicited)) {
		bool queuedThis = openavbAecpQueueChangedUnsolicitedResponseLocked(
			descriptor_type, descriptor_index, TRUE, &unsolicited);
		queued = queuedThis || queued;
	}

	if (queued) {
		openavbAecpBumpAvailableIndexLocked();
	}

	return queued;
}

static bool openavbAecpNotifyCountersChangedLocked(U16 descriptor_type, U16 descriptor_index)
{
	bool queued = FALSE;
	openavb_aecp_AEMCommandResponse_t unsolicited;

	if (!openavbAecpSMEntityModelEntityVars.unsolicitedControllerRegistered) {
		return FALSE;
	}

	if ((descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT ||
			descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) &&
			openavbAecpBuildUnsolicitedGetCountersLocked(descriptor_type, descriptor_index, &unsolicited)) {
		bool queuedThis = openavbAecpQueueChangedUnsolicitedResponseLocked(
			descriptor_type, descriptor_index, FALSE, &unsolicited);
		queued = queuedThis || queued;
	}

	if (queued) {
		openavbAecpBumpAvailableIndexLocked();
	}

	return queued;
}

static void openavbAecpCopyMvuProtocolId(U8 protocol_id[OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH])
{
	if (!protocol_id) {
		return;
	}

	memcpy(protocol_id, s_openavbAecpMvuProtocolId, OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH);
}

static void openavbAecpCopyAudioMappingToResponse(
	openavb_aecp_audio_mapping_t *pDst,
	const openavb_aem_descriptor_audio_map_audio_mapping_format_t *pSrc)
{
	if (!pDst || !pSrc) {
		return;
	}

	pDst->mapping_stream_index = pSrc->mapping_stream_index;
	pDst->mapping_stream_channel = pSrc->mapping_stream_channel;
	pDst->mapping_cluster_offset = pSrc->mapping_cluster_offset;
	pDst->mapping_cluster_channel = pSrc->mapping_cluster_channel;
}

static void openavbAecpCopyAudioMappingFromCommand(
	openavb_aem_descriptor_audio_map_audio_mapping_format_t *pDst,
	const openavb_aecp_audio_mapping_t *pSrc)
{
	if (!pDst || !pSrc) {
		return;
	}

	pDst->mapping_stream_index = pSrc->mapping_stream_index;
	pDst->mapping_stream_channel = pSrc->mapping_stream_channel;
	pDst->mapping_cluster_offset = pSrc->mapping_cluster_offset;
	pDst->mapping_cluster_channel = pSrc->mapping_cluster_channel;
}

static bool openavbAecpAudioMappingEqualsAemAecp(
	const openavb_aem_descriptor_audio_map_audio_mapping_format_t *pAem,
	const openavb_aecp_audio_mapping_t *pAecp)
{
	return (pAem && pAecp &&
		pAem->mapping_stream_index == pAecp->mapping_stream_index &&
		pAem->mapping_stream_channel == pAecp->mapping_stream_channel &&
		pAem->mapping_cluster_offset == pAecp->mapping_cluster_offset &&
		pAem->mapping_cluster_channel == pAecp->mapping_cluster_channel);
}

static bool openavbAecpAudioMappingsConflictOnInput(
	const openavb_aecp_audio_mapping_t *pFirst,
	const openavb_aecp_audio_mapping_t *pSecond)
{
	return (pFirst && pSecond &&
		pFirst->mapping_cluster_offset == pSecond->mapping_cluster_offset &&
		pFirst->mapping_cluster_channel == pSecond->mapping_cluster_channel &&
		(pFirst->mapping_stream_index != pSecond->mapping_stream_index ||
		 pFirst->mapping_stream_channel != pSecond->mapping_stream_channel));
}

static bool openavbAecpAudioMappingsAreValidForInput(
	const openavb_aem_descriptor_stream_port_io_t *pStreamPort,
	const openavb_aecp_commandresponse_data_audio_mappings_t *pCmd)
{
	U16 i;
	U16 j;
	U16 projectedMappingCount;

	if (!pStreamPort || !pCmd) {
		return FALSE;
	}
	if (!pStreamPort->dynamic_mappings_supported) {
		return FALSE;
	}
	if (pCmd->number_of_mappings != pCmd->mappingsCount) {
		return FALSE;
	}

	projectedMappingCount = pStreamPort->dynamic_number_of_mappings;
	for (i = 0; i < pCmd->mappingsCount; i++) {
		const openavb_aecp_audio_mapping_t *pMapping = &pCmd->mappings[i];
		bool exists = FALSE;

		if (pMapping->mapping_stream_index != 0) {
			return FALSE;
		}
		if (pMapping->mapping_cluster_offset >= pStreamPort->number_of_clusters) {
			return FALSE;
		}
		if (pMapping->mapping_stream_channel >= pStreamPort->dynamic_stream_channels) {
			return FALSE;
		}
		if (pMapping->mapping_cluster_channel >= pStreamPort->dynamic_cluster_channels) {
			return FALSE;
		}

		for (j = i + 1; j < pCmd->mappingsCount; j++) {
			if (openavbAecpAudioMappingsConflictOnInput(pMapping, &pCmd->mappings[j])) {
				return FALSE;
			}
		}

		for (j = 0; j < pStreamPort->dynamic_number_of_mappings; j++) {
			const openavb_aem_descriptor_audio_map_audio_mapping_format_t *pExisting =
				&pStreamPort->dynamic_mappings[j];

			if (openavbAecpAudioMappingEqualsAemAecp(pExisting, pMapping)) {
				exists = TRUE;
			}

			if (pExisting->mapping_cluster_offset == pMapping->mapping_cluster_offset &&
					pExisting->mapping_cluster_channel == pMapping->mapping_cluster_channel &&
					!openavbAecpAudioMappingEqualsAemAecp(pExisting, pMapping)) {
				return FALSE;
			}
		}

		if (!exists) {
			projectedMappingCount++;
			if (projectedMappingCount > OPENAVB_DESCRIPTOR_AUDIO_MAP_MAX_MAPPINGS) {
				return FALSE;
			}
		}
	}

	return TRUE;
}

static bool openavbAecpGetDynamicAudioMap(
	const openavb_aem_descriptor_stream_port_io_t *pStreamPort,
	openavb_aecp_response_data_get_audio_map_t *pRsp)
{
	U16 i;

	if (!pStreamPort || !pRsp) {
		return FALSE;
	}
	if (!pStreamPort->dynamic_mappings_supported) {
		return FALSE;
	}
	if (pRsp->map_index >= pStreamPort->dynamic_number_of_maps) {
		return FALSE;
	}

	pRsp->number_of_maps = pStreamPort->dynamic_number_of_maps;
	pRsp->number_of_mappings = pStreamPort->dynamic_number_of_mappings;
	pRsp->reserved = 0;
	pRsp->mappingsCount = pRsp->number_of_mappings;
	for (i = 0; i < pRsp->mappingsCount; i++) {
		openavbAecpCopyAudioMappingToResponse(&pRsp->mappings[i], &pStreamPort->dynamic_mappings[i]);
	}

	return TRUE;
}

static U16 openavbAecpApplyAddAudioMappings(
	openavb_aem_descriptor_stream_port_io_t *pStreamPort,
	const openavb_aecp_commandresponse_data_audio_mappings_t *pCmd,
	openavb_aecp_commandresponse_data_audio_mappings_t *pRsp)
{
	openavb_aem_descriptor_audio_map_audio_mapping_format_t updated[OPENAVB_DESCRIPTOR_AUDIO_MAP_MAX_MAPPINGS];
	U16 updatedCount;
	U16 appliedCount = 0;
	U16 i;
	U16 j;

	if (!pStreamPort || !pCmd || !pRsp) {
		return 0;
	}

	memcpy(updated, pStreamPort->dynamic_mappings, sizeof(updated));
	updatedCount = pStreamPort->dynamic_number_of_mappings;

	for (i = 0; i < pCmd->mappingsCount; i++) {
		const openavb_aecp_audio_mapping_t *pMapping = &pCmd->mappings[i];
		bool exists = FALSE;

		for (j = 0; j < updatedCount; j++) {
			if (openavbAecpAudioMappingEqualsAemAecp(&updated[j], pMapping)) {
				exists = TRUE;
				break;
			}
		}

		if (exists) {
			continue;
		}
		if (updatedCount >= OPENAVB_DESCRIPTOR_AUDIO_MAP_MAX_MAPPINGS) {
			return 0;
		}

		openavbAecpCopyAudioMappingFromCommand(&updated[updatedCount], pMapping);
		openavbAecpCopyAudioMappingToResponse(&pRsp->mappings[appliedCount], &updated[updatedCount]);
		updatedCount++;
		appliedCount++;
	}

	memcpy(pStreamPort->dynamic_mappings, updated, sizeof(updated));
	pStreamPort->dynamic_number_of_mappings = updatedCount;
	pRsp->number_of_mappings = appliedCount;
	pRsp->mappingsCount = appliedCount;
	return appliedCount;
}

static U16 openavbAecpApplyRemoveAudioMappings(
	openavb_aem_descriptor_stream_port_io_t *pStreamPort,
	const openavb_aecp_commandresponse_data_audio_mappings_t *pCmd,
	openavb_aecp_commandresponse_data_audio_mappings_t *pRsp)
{
	U16 appliedCount = 0;
	U16 i;
	U16 j;

	if (!pStreamPort || !pCmd || !pRsp) {
		return 0;
	}

	for (i = 0; i < pCmd->mappingsCount; i++) {
		const openavb_aecp_audio_mapping_t *pMapping = &pCmd->mappings[i];

		for (j = 0; j < pStreamPort->dynamic_number_of_mappings; j++) {
			if (openavbAecpAudioMappingEqualsAemAecp(&pStreamPort->dynamic_mappings[j], pMapping)) {
				openavbAecpCopyAudioMappingToResponse(&pRsp->mappings[appliedCount], &pStreamPort->dynamic_mappings[j]);
				appliedCount++;
				if ((j + 1) < pStreamPort->dynamic_number_of_mappings) {
					memmove(&pStreamPort->dynamic_mappings[j],
						&pStreamPort->dynamic_mappings[j + 1],
						(pStreamPort->dynamic_number_of_mappings - (j + 1)) * sizeof(pStreamPort->dynamic_mappings[0]));
				}
				memset(&pStreamPort->dynamic_mappings[pStreamPort->dynamic_number_of_mappings - 1],
					0,
					sizeof(pStreamPort->dynamic_mappings[0]));
				pStreamPort->dynamic_number_of_mappings--;
				break;
			}
		}
	}

	pRsp->number_of_mappings = appliedCount;
	pRsp->mappingsCount = appliedCount;
	return appliedCount;
}

static bool openavbAecpIsMvuProtocolId(const U8 protocol_id[OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH])
{
	return (protocol_id != NULL &&
		memcmp(protocol_id, s_openavbAecpMvuProtocolId, OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH) == 0);
}

static void openavbAecpClearStreamInputRuntimeState(openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput)
{
	if (!pDescriptorStreamInput) {
		return;
	}

	memset(pDescriptorStreamInput->acmp_stream_id, 0, sizeof(pDescriptorStreamInput->acmp_stream_id));
	memset(pDescriptorStreamInput->acmp_dest_addr, 0, sizeof(pDescriptorStreamInput->acmp_dest_addr));
	pDescriptorStreamInput->acmp_stream_vlan_id = 0;
	pDescriptorStreamInput->acmp_flags = 0;
	pDescriptorStreamInput->msrp_failure_code = 0;
	memset(pDescriptorStreamInput->msrp_failure_bridge_id, 0, sizeof(pDescriptorStreamInput->msrp_failure_bridge_id));
	pDescriptorStreamInput->stream_info_flags_ex &= ~OPENAVB_STREAM_INFO_FLAGS_EX_REGISTERING;
	pDescriptorStreamInput->mvu_acmp_status = 0;
}

static openavb_mvu_probing_status_t openavbAecpGetStreamInputProbingStatus(
	const openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput)
{
	if (!pDescriptorStreamInput || !pDescriptorStreamInput->mvu_bound) {
		return OPENAVB_MVU_PROBING_STATUS_DISABLED;
	}

	if (memcmp(pDescriptorStreamInput->acmp_stream_id, "\0\0\0\0\0\0\0\0",
			sizeof(pDescriptorStreamInput->acmp_stream_id)) != 0) {
		return OPENAVB_MVU_PROBING_STATUS_COMPLETED;
	}

	if (pDescriptorStreamInput->fast_connect_status == OPENAVB_FAST_CONNECT_STATUS_IN_PROGRESS) {
		return OPENAVB_MVU_PROBING_STATUS_ACTIVE;
	}

	return OPENAVB_MVU_PROBING_STATUS_PASSIVE;
}


void acquireEntity()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	if (!pCommand) {
		AVB_LOG_ERROR("acquireEntity called without command");
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return;
	}

	pCommand->headers.message_type = OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE;
	pCommand->entityModelPdu.u = 0;
	pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
	pCommand->entityModelPdu.command_data.acquireEntityRsp.flags =
		pCommand->entityModelPdu.command_data.acquireEntityCmd.flags;
	pCommand->entityModelPdu.command_data.acquireEntityRsp.descriptor_type =
		pCommand->entityModelPdu.command_data.acquireEntityCmd.descriptor_type;
	pCommand->entityModelPdu.command_data.acquireEntityRsp.descriptor_index =
		pCommand->entityModelPdu.command_data.acquireEntityCmd.descriptor_index;
	memset(pCommand->entityModelPdu.command_data.acquireEntityRsp.owner_id, 0,
		sizeof(pCommand->entityModelPdu.command_data.acquireEntityRsp.owner_id));

	// Milan 1.3 devices must not implement ACQUIRE_ENTITY.
	pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void lockEntity()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	if (!pCommand) {
		AVB_LOG_ERROR("lockEntity called without command");
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return;
	}

	pCommand->headers.message_type = OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE;
	pCommand->entityModelPdu.u = 0;
	pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
	pCommand->entityModelPdu.command_data.lockEntityRsp.flags =
		pCommand->entityModelPdu.command_data.lockEntityCmd.flags;
	pCommand->entityModelPdu.command_data.lockEntityRsp.descriptor_type =
		pCommand->entityModelPdu.command_data.lockEntityCmd.descriptor_type;
	pCommand->entityModelPdu.command_data.lockEntityRsp.descriptor_index =
		pCommand->entityModelPdu.command_data.lockEntityCmd.descriptor_index;
	memset(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id, 0,
		sizeof(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id));

	openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();
	if (!pAem) {
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return;
	}

	AEM_LOCK();
	openavbAecpExpireOwnershipLocked(pAem);
	if (pCommand->entityModelPdu.command_data.lockEntityCmd.descriptor_type != OPENAVB_AEM_DESCRIPTOR_ENTITY) {
		pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
	}
	else if (pCommand->entityModelPdu.command_data.lockEntityCmd.flags & OPENAVB_AEM_LOCK_ENTITY_COMMAND_FLAG_UNLOCK) {
		if (!pAem->entityLocked) {
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
		}
		else if (memcmp(pAem->lockedControllerId,
				pCommand->commonPdu.controller_entity_id,
				sizeof(pAem->lockedControllerId)) != 0) {
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_LOCKED;
			memcpy(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id,
				pAem->lockedControllerId,
				sizeof(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id));
		}
		else {
			openavbAecpClearEntityLockedLocked(pAem, NULL);
		}
	}
	else if (pAem->entityAcquired &&
			memcmp(pAem->acquiredControllerId,
				pCommand->commonPdu.controller_entity_id,
				sizeof(pAem->acquiredControllerId)) != 0) {
		pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
	}
	else if (pAem->entityLocked) {
		if (memcmp(pAem->lockedControllerId,
				pCommand->commonPdu.controller_entity_id,
				sizeof(pAem->lockedControllerId)) != 0) {
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_LOCKED;
			memcpy(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id,
				pAem->lockedControllerId,
				sizeof(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id));
		}
		else {
			openavbAecpMarkLockedOwnerActivityLocked();
			memcpy(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id,
				pAem->lockedControllerId,
				sizeof(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id));
		}
	}
	else {
		pAem->entityLocked = TRUE;
		memcpy(pAem->lockedControllerId,
			pCommand->commonPdu.controller_entity_id,
			sizeof(pAem->lockedControllerId));
		openavbAecpMarkLockedOwnerActivityLocked();
		memcpy(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id,
			pAem->lockedControllerId,
			sizeof(pCommand->entityModelPdu.command_data.lockEntityRsp.locked_id));
		AVB_LOGF_DEBUG("Entity Locked by " ENTITYID_FORMAT, pAem->lockedControllerId);
	}
	AEM_UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

// Returns TRUE is current rcvdCommand controlID matches the AEM acquire or lock controller id
bool processCommandCheckRestriction_CorrectController()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	bool bResult = TRUE;

	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	if (!pCommand) {
		AVB_LOG_ERROR("processCommandCheckRestriction_CorrectController called without command");
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return bResult;
	}

	openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();
	if (pAem) {
		AEM_LOCK();
		openavbAecpExpireOwnershipLocked(pAem);
		if (pAem->entityAcquired) {
			if (memcmp(pAem->acquiredControllerId, pCommand->commonPdu.controller_entity_id, sizeof(pAem->acquiredControllerId)) == 0) {
				bResult = TRUE;
				openavbAecpMarkAcquiredOwnerActivityLocked();
			}
			else {
				AVB_LOGF_DEBUG("Access denied to " ENTITYID_FORMAT " as " ENTITYID_FORMAT " already acquired it", pCommand->commonPdu.controller_entity_id, pAem->acquiredControllerId);
				bResult = FALSE;
			}
		}
		else if (pAem->entityLocked) {
			if (memcmp(pAem->lockedControllerId, pCommand->commonPdu.controller_entity_id, sizeof(pAem->lockedControllerId)) == 0) {
				bResult = TRUE;
				openavbAecpMarkLockedOwnerActivityLocked();
			}
			else {
				AVB_LOGF_DEBUG("Access denied to " ENTITYID_FORMAT " as " ENTITYID_FORMAT " already locked it", pCommand->commonPdu.controller_entity_id, pAem->lockedControllerId);
				bResult = FALSE;
			}
		}
		AEM_UNLOCK();
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
	return bResult;
}

// Returns TRUE if current rcvdCommand controller matches the current entity lock owner, or if the entity is unlocked.
bool processCommandCheckRestriction_LockingController()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	bool bResult = TRUE;

	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	if (!pCommand) {
		AVB_LOG_ERROR("processCommandCheckRestriction_LockingController called without command");
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return bResult;
	}

	openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();
	if (pAem) {
		AEM_LOCK();
		openavbAecpExpireOwnershipLocked(pAem);
		if (pAem->entityLocked) {
			if (memcmp(pAem->lockedControllerId,
					pCommand->commonPdu.controller_entity_id,
					sizeof(pAem->lockedControllerId)) == 0) {
				openavbAecpMarkLockedOwnerActivityLocked();
			}
			else {
				AVB_LOGF_DEBUG("Lock access denied to " ENTITYID_FORMAT " as " ENTITYID_FORMAT " already locked it",
					pCommand->commonPdu.controller_entity_id,
					pAem->lockedControllerId);
				bResult = FALSE;
			}
		}
		AEM_UNLOCK();
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
	return bResult;
}

// Returns TRUE the stream_input or stream_output descriptor is currently not running.
// NOTE:  This function is using the last reported state from the client, not the state AVDECC last told the client to be in.
bool processCommandCheckRestriction_StreamNotRunning(U16 descriptor_type, U16 descriptor_index)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	bool bResult = TRUE;

	if (descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT ||
			descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		U16 configIdx = openavbAemGetConfigIdx();
		openavb_aem_descriptor_stream_io_t *pDescriptorStreamIO = openavbAemGetDescriptor(configIdx, descriptor_type, descriptor_index);
		if (pDescriptorStreamIO) {
			openavbAvdeccMsgStateType_t state = openavbAVDECCGetStreamingState(pDescriptorStreamIO, configIdx);
			if (state >= OPENAVB_AVDECC_MSG_RUNNING) {
				bResult = FALSE;
			}
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
	return bResult;
}

static bool openavbAecpParseEmbeddedAemCommand(
	openavb_aecp_AEMCommandResponse_t *pSubCommand,
	U16 commandType,
	const U8 *pPayload,
	U16 payloadLength)
{
	U8 *pSrc = (U8 *)pPayload;

	if (!pSubCommand) {
		return FALSE;
	}

	switch (commandType) {
		case OPENAVB_AEM_COMMAND_CODE_GET_CONFIGURATION:
		case OPENAVB_AEM_COMMAND_CODE_GET_ASSOCIATION_ID:
			return (payloadLength == 0);

		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_FORMAT:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getStreamFormatCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getStreamFormatCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_INFO:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getStreamInfoCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getStreamInfoCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_NAME:
			if (payloadLength != 8) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getNameCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getNameCmd.descriptor_index, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getNameCmd.name_index, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getNameCmd.configuration_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_SAMPLING_RATE:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getSamplingRateCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getSamplingRateCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_CLOCK_SOURCE:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getClockSourceCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getClockSourceCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_AVB_INFO:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAvbInfoCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAvbInfoCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_AS_PATH:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAsPathCmd.descriptor_index, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAsPathCmd.reserved, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getCountersCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getCountersCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_MEMORY_OBJECT_LENGTH:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getMemoryObjectLengthCmd.configuration_index, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getMemoryObjectLengthCmd.memory_object_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME:
			if (payloadLength != 4) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getMaxTransitTimeCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getMaxTransitTimeCmd.descriptor_index, pSrc);
			return TRUE;

		case OPENAVB_AEM_COMMAND_CODE_GET_AUDIO_MAP:
			if (payloadLength != 8) return FALSE;
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAudioMapCmd.descriptor_type, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAudioMapCmd.descriptor_index, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAudioMapCmd.map_index, pSrc);
			OCT_B2DNTOHS(pSubCommand->entityModelPdu.command_data.getAudioMapCmd.reserved, pSrc);
			return TRUE;

		default:
			return TRUE;
	}
}

static void openavbAecpSerializeEmbeddedAudioMappings(
	U8 **ppDst,
	const openavb_aecp_audio_mapping_t *pMappings,
	U16 number_of_mappings)
{
	U16 i;
	U8 *pDst;

	if (!ppDst || !*ppDst || !pMappings) {
		return;
	}

	pDst = *ppDst;
	for (i = 0; i < number_of_mappings; i++) {
		OCT_D2BHTONS(pDst, pMappings[i].mapping_stream_index);
		OCT_D2BHTONS(pDst, pMappings[i].mapping_stream_channel);
		OCT_D2BHTONS(pDst, pMappings[i].mapping_cluster_offset);
		OCT_D2BHTONS(pDst, pMappings[i].mapping_cluster_channel);
	}
	*ppDst = pDst;
}

static bool openavbAecpSerializeEmbeddedAemResponse(
	const openavb_aecp_AEMCommandResponse_t *pSubCommand,
	U8 *pBuf,
	U16 bufLength,
	U16 *pPayloadLength)
{
	U8 *pDst = pBuf;

	if (!pSubCommand || !pBuf || !pPayloadLength) {
		return FALSE;
	}

	switch (pSubCommand->entityModelPdu.command_type) {
		case OPENAVB_AEM_COMMAND_CODE_GET_CONFIGURATION:
			if (bufLength < 4) return FALSE;
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getConfigurationRsp.reserved);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getConfigurationRsp.configuration_index);
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_FORMAT:
			if (bufLength < 12) return FALSE;
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getStreamFormatRsp.descriptor_type);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getStreamFormatRsp.descriptor_index);
			OCT_D2BMEMCP(pDst, pSubCommand->entityModelPdu.command_data.getStreamFormatRsp.stream_format);
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_INFO:
			{
				bool emitMilanExtension;
				U32 streamInfoFlagsEx;
				U8 probingAcmpStatus;
				U8 reserved3 = 0;
				U16 reserved4 = 0;
				const openavb_aecp_response_data_get_stream_info_t *pRsp =
					&pSubCommand->entityModelPdu.command_data.getStreamInfoRsp;

				emitMilanExtension = TRUE;
				if (bufLength < (emitMilanExtension ? 56 : 48)) return FALSE;
				streamInfoFlagsEx = pRsp->flags_ex;
				probingAcmpStatus = 0;
				if (emitMilanExtension &&
						(pRsp->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_CONNECTED)) {
					probingAcmpStatus = (U8)(OPENAVB_MVU_PROBING_STATUS_COMPLETED << 5);
				}

				OCT_D2BHTONS(pDst, pRsp->descriptor_type);
				OCT_D2BHTONS(pDst, pRsp->descriptor_index);
				OCT_D2BHTONL(pDst, pRsp->flags);
				OCT_D2BMEMCP(pDst, pRsp->stream_format);
				OCT_D2BMEMCP(pDst, pRsp->stream_id);
				OCT_D2BHTONL(pDst, pRsp->msrp_accumulated_latency);
				OCT_D2BMEMCP(pDst, pRsp->stream_dest_mac);
				OCT_D2BHTONB(pDst, pRsp->msrp_failure_code);
				OCT_D2BHTONB(pDst, pRsp->reserved_1);
				OCT_D2BMEMCP(pDst, pRsp->msrp_failure_bridge_id);
				OCT_D2BHTONS(pDst, pRsp->stream_vlan_id);
				OCT_D2BHTONS(pDst, pRsp->reserved_2);
				if (emitMilanExtension) {
					OCT_D2BHTONL(pDst, streamInfoFlagsEx);
					OCT_D2BHTONB(pDst, probingAcmpStatus);
					OCT_D2BHTONB(pDst, reserved3);
					OCT_D2BHTONS(pDst, reserved4);
				}
			}
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_ASSOCIATION_ID:
			if (bufLength < 8) return FALSE;
			OCT_D2BMEMCP(pDst, pSubCommand->entityModelPdu.command_data.getAssociationIDRsp.association_id);
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_SAMPLING_RATE:
			if (bufLength < 8) return FALSE;
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getSamplingRateRsp.descriptor_type);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getSamplingRateRsp.descriptor_index);
			OCT_D2BMEMCP(pDst, pSubCommand->entityModelPdu.command_data.getSamplingRateRsp.sampling_rate);
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_CLOCK_SOURCE:
			if (bufLength < 8) return FALSE;
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getClockSourceRsp.descriptor_type);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getClockSourceRsp.descriptor_index);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getClockSourceRsp.clock_source_index);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getClockSourceRsp.reserved);
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_AVB_INFO:
			{
				const openavb_aecp_response_data_get_avb_info_t *pRsp =
					&pSubCommand->entityModelPdu.command_data.getAvbInfoRsp;
				U16 mappingsBytes = pRsp->msrp_mappings_count * 8;
				if (bufLength < (U16)(20 + mappingsBytes)) return FALSE;
				OCT_D2BHTONS(pDst, pRsp->descriptor_type);
				OCT_D2BHTONS(pDst, pRsp->descriptor_index);
				OCT_D2BBUFCP(pDst, pRsp->as_grandmaster_id, sizeof(pRsp->as_grandmaster_id));
				OCT_D2BHTONL(pDst, pRsp->propagation_delay);
				OCT_D2BHTONB(pDst, pRsp->as_domain_number);
				OCT_D2BHTONB(pDst, pRsp->flags);
				OCT_D2BHTONS(pDst, pRsp->msrp_mappings_count);
				if (mappingsBytes > 0) {
					OCT_D2BBUFCP(pDst, pRsp->msrp_mappings, mappingsBytes);
				}
			}
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_AS_PATH:
			{
				const openavb_aecp_response_data_get_as_path_t *pRsp =
					&pSubCommand->entityModelPdu.command_data.getAsPathRsp;
				U16 pathBytes = pRsp->as_path_count * 8;
				if (bufLength < (U16)(8 + pathBytes)) return FALSE;
				OCT_D2BHTONS(pDst, pRsp->descriptor_index);
				OCT_D2BHTONS(pDst, pRsp->as_path_count);
				OCT_D2BHTONL(pDst, pRsp->path_latency);
				if (pathBytes > 0) {
					OCT_D2BBUFCP(pDst, pRsp->as_path, pathBytes);
				}
			}
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS:
			{
				const openavb_aecp_response_data_get_counters_t *pRsp =
					&pSubCommand->entityModelPdu.command_data.getCountersRsp;
				if (bufLength < (U16)(8 + pRsp->counters_block_length)) return FALSE;
				OCT_D2BHTONS(pDst, pRsp->descriptor_type);
				OCT_D2BHTONS(pDst, pRsp->descriptor_index);
				OCT_D2BHTONL(pDst, pRsp->counters_valid);
				OCT_D2BBUFCP(pDst, pRsp->counters_block, pRsp->counters_block_length);
			}
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME:
			if (bufLength < 12) return FALSE;
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getMaxTransitTimeRsp.descriptor_type);
			OCT_D2BHTONS(pDst, pSubCommand->entityModelPdu.command_data.getMaxTransitTimeRsp.descriptor_index);
			OCT_D2BHTONL(pDst, (U32)(pSubCommand->entityModelPdu.command_data.getMaxTransitTimeRsp.max_transit_time >> 32));
			OCT_D2BHTONL(pDst, (U32)(pSubCommand->entityModelPdu.command_data.getMaxTransitTimeRsp.max_transit_time & 0xffffffffu));
			break;

		case OPENAVB_AEM_COMMAND_CODE_GET_AUDIO_MAP:
			{
				const openavb_aecp_response_data_get_audio_map_t *pRsp =
					&pSubCommand->entityModelPdu.command_data.getAudioMapRsp;
				U16 mappingsBytes = pRsp->number_of_mappings * sizeof(openavb_aecp_audio_mapping_t);
				if (bufLength < (U16)(12 + mappingsBytes)) return FALSE;
				OCT_D2BHTONS(pDst, pRsp->descriptor_type);
				OCT_D2BHTONS(pDst, pRsp->descriptor_index);
				OCT_D2BHTONS(pDst, pRsp->map_index);
				OCT_D2BHTONS(pDst, pRsp->number_of_maps);
				OCT_D2BHTONS(pDst, pRsp->number_of_mappings);
				OCT_D2BHTONS(pDst, pRsp->reserved);
				if (mappingsBytes > 0) {
					openavbAecpSerializeEmbeddedAudioMappings(&pDst, pRsp->mappings, pRsp->number_of_mappings);
				}
			}
			break;

		default:
			return FALSE;
	}

	*pPayloadLength = (U16)(pDst - pBuf);
	return TRUE;
}

static bool openavbAecpPackedStreamFormatsMatch(
	const openavb_aem_stream_format_t *pCurrentFormat,
	const U8 *pPackedFormat)
{
	U8 packedCurrentFormat[8];

	if (!pCurrentFormat || !pPackedFormat) {
		return FALSE;
	}

	openavbAemStreamFormatToBuf((openavb_aem_stream_format_t *)pCurrentFormat, packedCurrentFormat);
	return (memcmp(packedCurrentFormat, pPackedFormat, sizeof(packedCurrentFormat)) == 0);
}

static void openavbAecpCopyPackedStreamFormat(
	U8 *pDstPackedFormat,
	const openavb_aem_stream_format_t *pSrcCurrentFormat)
{
	if (!pDstPackedFormat) {
		return;
	}

	memset(pDstPackedFormat, 0, 8);
	if (pSrcCurrentFormat) {
		openavbAemStreamFormatToBuf((openavb_aem_stream_format_t *)pSrcCurrentFormat, pDstPackedFormat);
	}
}

static void openavbAecpHandleGetDynamicInfo(void)
{
	U16 offset = 0;
	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	openavb_aecp_commandresponse_data_get_dynamic_info_t *pCmd;
	openavb_aecp_commandresponse_data_get_dynamic_info_t *pRsp;

	if (!pCommand) {
		return;
	}

	pCmd = &pCommand->entityModelPdu.command_data.getDynamicInfoCmd;
	pRsp = &pCommand->entityModelPdu.command_data.getDynamicInfoRsp;
	pRsp->payload_length = 0;
	pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;

	while (offset < pCmd->payload_length) {
		U16 specificCommandLength;
		U16 reserved1;
		U8 requestedStatus;
		U8 reserved2;
		U16 embeddedCommandType;
		U16 responseLength = 0;
		U8 *pSrc = pCmd->payload + offset;
		openavb_aecp_AEMCommandResponse_t subCommand;
		openavb_aecp_AEMCommandResponse_t *pSavedCommand;
		bool parsed;
		bool serialized;
		U16 responseHeaderOffset;

		if ((U16)(pCmd->payload_length - offset) < 8) {
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
			pRsp->payload_length = 0;
			return;
		}

		OCT_B2DNTOHS(specificCommandLength, pSrc);
		OCT_B2DNTOHS(reserved1, pSrc);
		OCT_B2DNTOHB(requestedStatus, pSrc);
		OCT_B2DNTOHB(reserved2, pSrc);
		OCT_B2DNTOHS(embeddedCommandType, pSrc);
		(void)reserved1;
		(void)requestedStatus;
		(void)reserved2;

		if ((U16)(pCmd->payload_length - offset - 8) < specificCommandLength) {
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
			pRsp->payload_length = 0;
			return;
		}

		memset(&subCommand, 0, sizeof(subCommand));
		memcpy(subCommand.host, pCommand->host, sizeof(subCommand.host));
		memcpy(subCommand.headers.target_entity_id,
			pCommand->headers.target_entity_id,
			sizeof(subCommand.headers.target_entity_id));
		memcpy(subCommand.commonPdu.controller_entity_id,
			pCommand->commonPdu.controller_entity_id,
			sizeof(subCommand.commonPdu.controller_entity_id));
		subCommand.commonPdu.sequence_id = pCommand->commonPdu.sequence_id;
		subCommand.entityModelPdu.command_type = embeddedCommandType;

		parsed = openavbAecpParseEmbeddedAemCommand(&subCommand,
			embeddedCommandType,
			pSrc,
			specificCommandLength);
		if (!parsed) {
			subCommand.headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
		}
		else if (embeddedCommandType == OPENAVB_AEM_COMMAND_CODE_GET_DYNAMIC_INFO) {
			subCommand.headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;
		}
		else {
			pSavedCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
			openavbAecpSMEntityModelEntityVars.rcvdCommand = &subCommand;
			processCommand();
			openavbAecpSMEntityModelEntityVars.rcvdCommand = pSavedCommand;
		}

		responseHeaderOffset = pRsp->payload_length;
		if ((U16)(sizeof(pRsp->payload) - pRsp->payload_length) < 8) {
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_RESOURCES;
			pRsp->payload_length = 0;
			return;
		}

		pRsp->payload_length += 8;
		serialized = openavbAecpSerializeEmbeddedAemResponse(&subCommand,
			pRsp->payload + pRsp->payload_length,
			(U16)(sizeof(pRsp->payload) - pRsp->payload_length),
			&responseLength);
		if (!serialized) {
			if ((U16)(sizeof(pRsp->payload) - pRsp->payload_length) < specificCommandLength) {
				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_RESOURCES;
				pRsp->payload_length = 0;
				return;
			}
			memcpy(pRsp->payload + pRsp->payload_length, pSrc, specificCommandLength);
			responseLength = specificCommandLength;
		}

		{
			U8 *pHdrDst = pRsp->payload + responseHeaderOffset;
			OCT_D2BHTONS(pHdrDst, responseLength);
			OCT_D2BHTONS(pHdrDst, 0);
			OCT_D2BHTONB(pHdrDst, subCommand.headers.status);
			OCT_D2BHTONB(pHdrDst, 0);
			OCT_D2BHTONS(pHdrDst, embeddedCommandType);
		}

		pRsp->payload_length += responseLength;
		offset = (U16)(offset + 8 + specificCommandLength);
	}
}

static void openavbAecpPrepareMvuResponse(openavb_aecp_AEMCommandResponse_t *pCommand, U8 status)
{
	if (!pCommand) {
		return;
	}

	pCommand->headers.message_type = OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE;
	pCommand->headers.status = status;
	pCommand->entityModelPdu.u = 0;
	openavbAecpCopyMvuProtocolId(pCommand->protocol_id);
}

static openavb_aem_descriptor_stream_io_t *openavbAecpGetMvuStreamInputDescriptor(
	U16 descriptor_type,
	U16 descriptor_index,
	U8 *pStatus)
{
	openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput;

	if (pStatus) {
		*pStatus = OPENAVB_AECP_MVU_STATUS_SUCCESS;
	}

	if (descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
		if (pStatus) {
			*pStatus = OPENAVB_AECP_MVU_STATUS_BAD_ARGUMENTS;
		}
		return NULL;
	}

	pDescriptorStreamInput = openavbAemGetDescriptor(openavbAemGetConfigIdx(),
		descriptor_type,
		descriptor_index);
	if (!pDescriptorStreamInput && pStatus) {
		*pStatus = OPENAVB_AECP_MVU_STATUS_NO_SUCH_DESCRIPTOR;
	}

	return pDescriptorStreamInput;
}

static void processVendorUniqueCommand(void)
{
	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;

	if (!pCommand) {
		AVB_LOG_ERROR("processVendorUniqueCommand called without command");
		return;
	}

	openavbAecpNoteControllerActivityLocked(pCommand);
	openavbAecpPrepareMvuResponse(pCommand, OPENAVB_AECP_MVU_STATUS_NOT_IMPLEMENTED);

	if (!openavbAecpIsMvuProtocolId(pCommand->protocol_id)) {
		return;
	}

	switch (pCommand->entityModelPdu.command_type) {
		case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MILAN_INFO:
			{
				openavb_aecp_mvu_response_data_get_milan_info_t *pRsp =
					&pCommand->entityModelPdu.command_data.getMilanInfoRsp;

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->protocol_version = OPENAVB_AECP_MILAN_PROTOCOL_VERSION;
				pRsp->features_flags = s_openavbAecpMilanFeaturesFlags;
				pRsp->certification_version = s_openavbAecpMilanCertificationVersion;
				pRsp->specification_version = s_openavbAecpMilanSpecificationVersion;
				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		case OPENAVB_AECP_MVU_COMMAND_TYPE_SET_SYSTEM_UNIQUE_ID:
			{
				openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t *pCmd =
					&pCommand->entityModelPdu.command_data.setSystemUniqueIdCmd;
				openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t *pRsp =
					&pCommand->entityModelPdu.command_data.setSystemUniqueIdRsp;

				openavbAecpEnsureSystemUniqueId();
				s_openavbSystemUniqueId = pCmd->system_unique_id;
				memcpy(s_openavbSystemName, pCmd->system_name, sizeof(s_openavbSystemName));
				s_openavbSystemName[sizeof(s_openavbSystemName) - 1] = '\0';

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->system_unique_id = s_openavbSystemUniqueId;
				memcpy(pRsp->system_name, s_openavbSystemName, sizeof(pRsp->system_name));
				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_SYSTEM_UNIQUE_ID:
			{
				openavb_aecp_mvu_response_data_get_system_unique_id_t *pRsp =
					&pCommand->entityModelPdu.command_data.getSystemUniqueIdRsp;

				openavbAecpEnsureSystemUniqueId();
				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->system_unique_id = s_openavbSystemUniqueId;
				memcpy(pRsp->system_name, s_openavbSystemName, sizeof(pRsp->system_name));
				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MEDIA_CLOCK_REFERENCE_INFO:
			{
				openavb_aecp_mvu_commandresponse_data_get_media_clock_reference_info_t *pCmd =
					&pCommand->entityModelPdu.command_data.getMediaClockReferenceInfoCmd;
				openavb_aecp_mvu_response_data_get_media_clock_reference_info_t *pRsp =
					&pCommand->entityModelPdu.command_data.getMediaClockReferenceInfoRsp;
				openavb_aem_descriptor_clock_domain_t *pDescriptorClockDomain = NULL;
				U16 clock_domain_index = pCmd->clock_domain_index;

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->clock_domain_index = clock_domain_index;
				pRsp->default_media_clock_priority = OPENAVB_AECP_MILAN_DEFAULT_MEDIA_CLOCK_PRIORITY;

				pDescriptorClockDomain = openavbAemGetDescriptor(
					openavbAemGetConfigIdx(),
					OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN,
					clock_domain_index);
				if (!pDescriptorClockDomain) {
					pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		case OPENAVB_AECP_MVU_COMMAND_TYPE_BIND_STREAM:
			{
				openavb_aecp_mvu_commandresponse_data_bind_stream_t *pCmd =
					&pCommand->entityModelPdu.command_data.bindStreamCmd;
				openavb_aecp_mvu_commandresponse_data_bind_stream_t *pRsp =
					&pCommand->entityModelPdu.command_data.bindStreamRsp;
				openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput;
				U8 status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
				U16 acmpConnectFlags = 0;
				bool queuedConnect = FALSE;

				memcpy(pRsp, pCmd, sizeof(*pRsp));
				pRsp->reserved = 0;

				if (!processCommandCheckRestriction_LockingController()) {
					pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_ENTITY_LOCKED;
					break;
				}

				pDescriptorStreamInput = openavbAecpGetMvuStreamInputDescriptor(
					pCmd->descriptor_type,
					pCmd->descriptor_index,
					&status);
				if (!pDescriptorStreamInput) {
					pCommand->headers.status = status;
					break;
				}

				if ((pCmd->flags & ~OPENAVB_MVU_BIND_STREAM_FLAG_STREAMING_WAIT) != 0) {
					pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_BAD_ARGUMENTS;
					break;
				}

				AEM_LOCK();
				openavbAecpClearStreamInputRuntimeState(pDescriptorStreamInput);
				pDescriptorStreamInput->mvu_bound = TRUE;
				pDescriptorStreamInput->mvu_bind_flags = pCmd->flags;
				memcpy(pDescriptorStreamInput->mvu_talker_entity_id,
					pCmd->talker_entity_id,
					sizeof(pDescriptorStreamInput->mvu_talker_entity_id));
				pDescriptorStreamInput->mvu_talker_stream_index = pCmd->talker_stream_index;
				pDescriptorStreamInput->fast_connect_status = OPENAVB_FAST_CONNECT_STATUS_UNKNOWN;
				AEM_UNLOCK();

				if ((pCmd->flags & OPENAVB_MVU_BIND_STREAM_FLAG_STREAMING_WAIT) != 0) {
					acmpConnectFlags |= OPENAVB_ACMP_FLAG_STREAMING_WAIT;
				}

				if (pDescriptorStreamInput->stream) {
					queuedConnect = openavbAcmpSMListenerSet_doConnect(
						pDescriptorStreamInput->stream,
						acmpConnectFlags,
						pCmd->talker_stream_index,
						pCmd->talker_entity_id,
						pCommand->commonPdu.controller_entity_id);
				}
				if (!queuedConnect) {
					pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_ENTITY_MISBEHAVING;
					break;
				}

				(void)openavbAecpNotifyStreamStateChangedLocked(
					pCmd->descriptor_type,
					pCmd->descriptor_index);
				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		case OPENAVB_AECP_MVU_COMMAND_TYPE_UNBIND_STREAM:
			{
				openavb_aecp_mvu_commandresponse_data_unbind_stream_t *pCmd =
					&pCommand->entityModelPdu.command_data.unbindStreamCmd;
				openavb_aecp_mvu_commandresponse_data_unbind_stream_t *pRsp =
					&pCommand->entityModelPdu.command_data.unbindStreamRsp;
				openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput;
				U8 status = OPENAVB_AECP_MVU_STATUS_SUCCESS;

				memcpy(pRsp, pCmd, sizeof(*pRsp));
				pRsp->reserved_0 = 0;

				if (!processCommandCheckRestriction_LockingController()) {
					pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_ENTITY_LOCKED;
					break;
				}

				pDescriptorStreamInput = openavbAecpGetMvuStreamInputDescriptor(
					pCmd->descriptor_type,
					pCmd->descriptor_index,
					&status);
				if (!pDescriptorStreamInput) {
					pCommand->headers.status = status;
					break;
				}

				if (pDescriptorStreamInput->stream &&
						!openavbAcmpSMListenerSet_doDisconnect(
							pDescriptorStreamInput->stream,
							pCommand->commonPdu.controller_entity_id)) {
					openavb_acmp_ListenerStreamInfo_t listenerInfo;
					memset(&listenerInfo, 0, sizeof(listenerInfo));
					(void)openavbAVDECCStopListener(
						pDescriptorStreamInput,
						openavbAemGetConfigIdx(),
						&listenerInfo);
					(void)openavbAvdeccClearSavedState(pDescriptorStreamInput->stream);
				}

				AEM_LOCK();
				openavbAecpClearStreamInputRuntimeState(pDescriptorStreamInput);
				pDescriptorStreamInput->mvu_bound = FALSE;
				pDescriptorStreamInput->mvu_bind_flags = 0;
				memset(pDescriptorStreamInput->mvu_talker_entity_id, 0,
					sizeof(pDescriptorStreamInput->mvu_talker_entity_id));
				pDescriptorStreamInput->mvu_talker_stream_index = 0;
				pDescriptorStreamInput->fast_connect_status = OPENAVB_FAST_CONNECT_STATUS_NOT_AVAILABLE;
				AEM_UNLOCK();

				(void)openavbAecpNotifyStreamStateChangedLocked(
					pCmd->descriptor_type,
					pCmd->descriptor_index);
				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_STREAM_INPUT_INFO_EX:
			{
				openavb_aecp_mvu_commandresponse_data_unbind_stream_t *pCmd =
					&pCommand->entityModelPdu.command_data.getStreamInputInfoExCmd;
				openavb_aecp_mvu_response_data_get_stream_input_info_ex_t *pRsp =
					&pCommand->entityModelPdu.command_data.getStreamInputInfoExRsp;
				openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput;
				U8 status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
				openavb_mvu_probing_status_t probingStatus = OPENAVB_MVU_PROBING_STATUS_DISABLED;
				U8 acmpStatus = OPENAVB_ACMP_STATUS_SUCCESS;
				U16 descriptor_type = pCmd->descriptor_type;
				U16 descriptor_index = pCmd->descriptor_index;

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->reserved_0 = 0;
				pRsp->descriptor_type = descriptor_type;
				pRsp->descriptor_index = descriptor_index;
				pRsp->reserved_1 = 0;

				pDescriptorStreamInput = openavbAecpGetMvuStreamInputDescriptor(
					descriptor_type,
					descriptor_index,
					&status);
				if (!pDescriptorStreamInput) {
					pCommand->headers.status = status;
					break;
				}

				AEM_LOCK();
				if (pDescriptorStreamInput->mvu_bound) {
					memcpy(pRsp->talker_entity_id,
						pDescriptorStreamInput->mvu_talker_entity_id,
						sizeof(pRsp->talker_entity_id));
					pRsp->talker_unique_id = pDescriptorStreamInput->mvu_talker_stream_index;
					acmpStatus = pDescriptorStreamInput->mvu_acmp_status;
				}
				probingStatus = openavbAecpGetStreamInputProbingStatus(pDescriptorStreamInput);
				AEM_UNLOCK();

				pRsp->probing_acmp_status =
					(U8)(((probingStatus << 5) & 0xe0) | (acmpStatus & 0x1f));
				pCommand->headers.status = OPENAVB_AECP_MVU_STATUS_SUCCESS;
			}
			break;

		default:
			break;
	}
}

// Process an incoming command and make it the response data on return.
void processCommand()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	openavb_aecp_AEMCommandResponse_t *pCommand = openavbAecpSMEntityModelEntityVars.rcvdCommand;
	if (!pCommand) {
		AVB_LOG_ERROR("processCommand called without command");
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return;
	}

	openavbAecpNoteControllerActivityLocked(pCommand);

	// Set message type as response
	pCommand->headers.message_type = OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE;
	pCommand->entityModelPdu.u = 0;

	// Set to Not Implemented. Will be overridden by commands that are implemented.
	pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;

	switch (pCommand->entityModelPdu.command_type) {
		case OPENAVB_AEM_COMMAND_CODE_ACQUIRE_ENTITY:
			// Implemented in the acquireEntity() function. Should never get here.
			break;
		case OPENAVB_AEM_COMMAND_CODE_LOCK_ENTITY:
			// Implemented in the lockEntity() function. Should never get here.
			break;
		case OPENAVB_AEM_COMMAND_CODE_ENTITY_AVAILABLE:
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			break;
		case OPENAVB_AEM_COMMAND_CODE_CONTROLLER_AVAILABLE:
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			break;
		case OPENAVB_AEM_COMMAND_CODE_READ_DESCRIPTOR:
			{
				openavb_aecp_command_data_read_descriptor_t *pCmd = &pCommand->entityModelPdu.command_data.readDescriptorCmd;
				openavb_aecp_response_data_read_descriptor_t *pRsp = &pCommand->entityModelPdu.command_data.readDescriptorRsp;
				U16 configuration_index = pCmd->configuration_index;
				U16 descriptor_type = pCmd->descriptor_type;
				U16 descriptor_index = pCmd->descriptor_index;

				if (descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONFIGURATION ||
						descriptor_type == OPENAVB_AEM_DESCRIPTOR_MEMORY_OBJECT) {
					AVB_LOGF_INFO("READ_DESCRIPTOR requested: config=%u type=0x%04x index=%u",
						configuration_index,
						descriptor_type,
						descriptor_index);
				}

				openavbRC rc = openavbAemSerializeDescriptor(configuration_index, descriptor_type, descriptor_index,
					sizeof(pRsp->descriptor_data), pRsp->descriptor_data, &pRsp->descriptor_length);
				if (IS_OPENAVB_FAILURE(rc)) {
					U8 *pDst = pRsp->descriptor_data;

					// Send a basic response back to the controller.
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					pRsp->configuration_index = configuration_index;
					pRsp->reserved = 0;
					OCT_D2BHTONS(pDst, descriptor_type);
					OCT_D2BHTONS(pDst, descriptor_index);
					pRsp->descriptor_length = pDst - pRsp->descriptor_data;
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					if (descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONFIGURATION ||
							descriptor_type == OPENAVB_AEM_DESCRIPTOR_MEMORY_OBJECT) {
						AVB_LOGF_INFO("READ_DESCRIPTOR served: config=%u type=0x%04x index=%u length=%u",
							configuration_index,
							descriptor_type,
							descriptor_index,
							pRsp->descriptor_length);
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_WRITE_DESCRIPTOR:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_CONFIGURATION:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_CONFIGURATION:
			{
				openavb_aecp_response_data_get_configuration_t *pRsp =
					&pCommand->entityModelPdu.command_data.getConfigurationRsp;
				pRsp->reserved = 0;
				pRsp->configuration_index = openavbAemGetConfigIdx();
				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_DYNAMIC_INFO:
			openavbAecpHandleGetDynamicInfo();
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_STREAM_FORMAT:
			{
				openavb_aecp_commandresponse_data_set_stream_format_t *pCmd = &pCommand->entityModelPdu.command_data.setStreamFormatCmd;
				openavb_aecp_commandresponse_data_set_stream_format_t *pRsp = &pCommand->entityModelPdu.command_data.setStreamFormatRsp;
				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;

				if (processCommandCheckRestriction_CorrectController()) {
					if (processCommandCheckRestriction_StreamNotRunning(pCmd->descriptor_type, pCmd->descriptor_index)) {
						if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
							openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
							if (pDescriptorStreamInput) {
								if (openavbAecpPackedStreamFormatsMatch(&pDescriptorStreamInput->current_format, pCmd->stream_format)) {
									// No change needed.
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
								}
								else {
									// AVDECC_TODO:  Verify that the stream format is supported, and notify the Listener of the change.
									//memcpy(&pDescriptorStreamInput->current_format, pCmd->stream_format, sizeof(pDescriptorStreamInput->current_format));
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
								}
							}
							else {
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
							}
						}
						else if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
							openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
							if (pDescriptorStreamOutput) {
								if (openavbAecpPackedStreamFormatsMatch(&pDescriptorStreamOutput->current_format, pCmd->stream_format)) {
									// No change needed.
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
								}
								else {
									// AVDECC_TODO:  Verify that the stream format is supported, and notify the Talker of the change.
									//memcpy(&pDescriptorStreamOutput->current_format, pCmd->stream_format, sizeof(pDescriptorStreamOutput->current_format));
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
								}
							}
							else {
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
							}
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_STREAM_IS_RUNNING;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
					openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorStreamInput) {
						openavbAecpCopyPackedStreamFormat(pRsp->stream_format, &pDescriptorStreamInput->current_format);
					}
				}
				else if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
					openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorStreamOutput) {
						openavbAecpCopyPackedStreamFormat(pRsp->stream_format, &pDescriptorStreamOutput->current_format);
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_FORMAT:
			{
				openavb_aecp_command_data_get_stream_format_t *pCmd = &pCommand->entityModelPdu.command_data.getStreamFormatCmd;
				openavb_aecp_response_data_get_stream_format_t *pRsp = &pCommand->entityModelPdu.command_data.getStreamFormatRsp;
				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;

				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
					openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorStreamInput) {
						openavbAecpCopyPackedStreamFormat(pRsp->stream_format, &pDescriptorStreamInput->current_format);
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
				}
				else if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
					openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorStreamOutput) {
						openavbAecpCopyPackedStreamFormat(pRsp->stream_format, &pDescriptorStreamOutput->current_format);
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_VIDEO_FORMAT:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_VIDEO_FORMAT:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_SENSOR_FORMAT:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_SENSOR_FORMAT:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_STREAM_INFO:
			{
				openavb_aecp_commandresponse_data_set_stream_info_t *pCmd = &pCommand->entityModelPdu.command_data.setStreamInfoCmd;
				openavb_aecp_commandresponse_data_set_stream_info_t *pRsp = &pCommand->entityModelPdu.command_data.setStreamInfoRsp;

				if (processCommandCheckRestriction_CorrectController()) {
					if (processCommandCheckRestriction_StreamNotRunning(pCmd->descriptor_type, pCmd->descriptor_index)) {
						if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT ||
								pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
							openavb_aem_descriptor_stream_io_t *pDescriptorStreamIO = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
							if (pDescriptorStreamIO) {
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;

								if ((pCmd->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_FORMAT_VALID) != 0) {
									if (!openavbAecpPackedStreamFormatsMatch(&pDescriptorStreamIO->current_format, pCmd->stream_format)) {
										// AVDECC_TODO:  Verify that the stream format is supported, and notify the Listener of the change.
										//memcpy(&pDescriptorStreamInput->current_format, pCmd->stream_format, sizeof(pDescriptorStreamInput->current_format));
										pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
									}
								}

								// AVDECC_TODO:  Add support for ENCRYPTED_PDU.
								if ((pCmd->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_ENCRYPTED_PDU) != 0) {
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
								}

									// MSRP accumulated latency is used by some controllers to expose
									// per-stream presentation offset. Keep command-provided value/flag.
									// If not provided in the command, clear it in the response.
									if ((pCmd->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_ACC_LAT_VALID) == 0) {
										pRsp->flags &= ~OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_ACC_LAT_VALID;
										pRsp->msrp_accumulated_latency = 0;
									}
									pRsp->flags &= ~OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_FAILURE_VALID;
									pRsp->msrp_failure_code = 0;
									memset(pRsp->msrp_failure_bridge_id, 0, sizeof(pRsp->msrp_failure_bridge_id));

								if (pCommand->headers.status != OPENAVB_AEM_COMMAND_STATUS_SUCCESS) {
									// As there are already issues, don't bother trying anything following this.
								}
								else if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
									// If the controller is trying to set the streaming information for the Listener, this is a problem.
									// The Listener gets this information from the Talker when the connection starts, so setting it here makes no sense.
									// We also ignore the CLASS_A flag (or absence of it), as the Talker will indicate that value as well.
									if ((pCmd->flags &
											(OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID |
											 OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID |
											 OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_VLAN_ID_VALID)) != 0) {
										AVB_LOG_ERROR("SET_STREAM_INFO cannot set stream parameters for Listener");
										pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
									}
								}
								else {
									// Get the streaming values to send to the Talker.
									// Invalid values used to indicate those that should not be changed.
									U8 sr_class = ((pCmd->flags & OPENAVB_ACMP_FLAG_CLASS_B) != 0 ? SR_CLASS_B : SR_CLASS_A);
									U8 class_update_valid = ((pCmd->flags & OPENAVB_ACMP_FLAG_CLASS_B) != 0);
									U8 stream_id_valid = FALSE;
									U8 stream_src_mac[6] = {0, 0, 0, 0, 0, 0};
									U16 stream_uid = 0;
									U8 stream_dest_valid = FALSE;
									U8 stream_dest_mac[6] = {0, 0, 0, 0, 0, 0};
									U8 stream_vlan_id_valid = FALSE;
									U16 stream_vlan_id = 0;
									if ((pCmd->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID) != 0) {
										stream_id_valid = TRUE;
										memcpy(stream_src_mac, pCmd->stream_id, 6);
										stream_uid = ntohs(*(U16*) (pCmd->stream_id + 6));
										AVB_LOGF_INFO("AVDECC-specified Stream ID " ETH_FORMAT "/%u for Talker", ETH_OCTETS(stream_src_mac), stream_uid);
									}
									if ((pCmd->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID) != 0) {
										stream_dest_valid = TRUE;
										memcpy(stream_dest_mac, pCmd->stream_dest_mac, 6);
										AVB_LOGF_INFO("AVDECC-specified Stream Dest Addr " ETH_FORMAT " for Talker", ETH_OCTETS(stream_dest_mac));
									}
									if ((pCmd->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_VLAN_ID_VALID) != 0) {
										stream_vlan_id_valid = TRUE;
										stream_vlan_id = pCmd->stream_vlan_id;
										AVB_LOGF_INFO("AVDECC-specified Stream VLAN ID %u for Talker", stream_vlan_id);
									}

									// Only forward updates that affect stream reservation/identity to the Talker.
									// Controllers may send SET_STREAM_INFO for readback-oriented fields (e.g. MSRP
									// accumulated latency) which should not require a Talker client round-trip.
									if (class_update_valid || stream_id_valid || stream_dest_valid || stream_vlan_id_valid) {
										if (!openavbAVDECCSetTalkerStreamInfo(
												pDescriptorStreamIO, sr_class,
												stream_id_valid, stream_src_mac, stream_uid,
												stream_dest_valid, stream_dest_mac,
												stream_vlan_id_valid, stream_vlan_id)) {
											if (!pDescriptorStreamIO->stream || !pDescriptorStreamIO->stream->client) {
												pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_IN_PROGRESS;
											}
											else {
												AVB_LOG_ERROR("SET_STREAM_INFO error setting stream parameters for Talker");
												pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
											}
										}
									}
								}
							}
							else {
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
							}
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_STREAM_IS_RUNNING;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_INFO:
			{
				openavb_aecp_command_data_get_stream_info_t *pCmd = &pCommand->entityModelPdu.command_data.getStreamInfoCmd;
				openavb_aecp_response_data_get_stream_info_t *pRsp = &pCommand->entityModelPdu.command_data.getStreamInfoRsp;
				U16 streamUidNbo = 0;
				// Mirror selectors into response fields explicitly for interop diagnostics.
				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;
				pRsp->flags = 0;
				memset(pRsp->stream_format, 0, sizeof(pRsp->stream_format));
				memset(pRsp->stream_id, 0, sizeof(pRsp->stream_id));
				pRsp->msrp_accumulated_latency = 0;
				memset(pRsp->stream_dest_mac, 0, sizeof(pRsp->stream_dest_mac));
				pRsp->msrp_failure_code = 0;
				pRsp->reserved_1 = 0;
				memset(pRsp->msrp_failure_bridge_id, 0, sizeof(pRsp->msrp_failure_bridge_id));
				pRsp->stream_vlan_id = 0;
				pRsp->reserved_2 = 0;
				pRsp->flags_ex = 0;

				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT ||
						pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
					U16 configIdx = openavbAemGetConfigIdx();
					openavb_aem_descriptor_stream_io_t *pDescriptorStreamIO = openavbAemGetDescriptor(configIdx, pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorStreamIO && pDescriptorStreamIO->stream) {
						openavbAvdeccMsgStateType_t clientState = openavbAVDECCGetStreamingState(pDescriptorStreamIO, configIdx);

						// Get the flags for the current status.
						pRsp->flags = 0;
						if (pDescriptorStreamIO->fast_connect_status >= OPENAVB_FAST_CONNECT_STATUS_IN_PROGRESS) {
							pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_FAST_CONNECT;
						}
						if (openavbAvdeccGetSaveStateInfo(pDescriptorStreamIO->stream, NULL, NULL, NULL, NULL)) {
							pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_SAVED_STATE;
						}
						if (clientState >= OPENAVB_AVDECC_MSG_RUNNING) {
							pRsp->flags |= (pDescriptorStreamIO->acmp_flags &
									(OPENAVB_ACMP_FLAG_CLASS_B |
									 OPENAVB_ACMP_FLAG_FAST_CONNECT |
									 OPENAVB_ACMP_FLAG_SUPPORTS_ENCRYPTED |
									 OPENAVB_ACMP_FLAG_ENCRYPTED_PDU));
							pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_CONNECTED;
							if (clientState == OPENAVB_AVDECC_MSG_PAUSED) {
								pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAMING_WAIT;
							}

							// For the Listener, use the streaming values we received from the current Talker.
							if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
								// Get the Stream ID.
								memcpy(pRsp->stream_id, pDescriptorStreamIO->acmp_stream_id, 8);
								if (memcmp(pRsp->stream_id, "\x00\x00\x00\x00\x00\x00\x00\x00", 8) != 0) {
									pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID;
								}
								else if (pDescriptorStreamIO->mvu_bound) {
									memcpy(pRsp->stream_id,
										pDescriptorStreamIO->mvu_talker_entity_id,
										sizeof(pDescriptorStreamIO->mvu_talker_entity_id));
									streamUidNbo = htons(pDescriptorStreamIO->mvu_talker_stream_index);
									memcpy(pRsp->stream_id + 6, &streamUidNbo, sizeof(streamUidNbo));
									if (memcmp(pRsp->stream_id, "\x00\x00\x00\x00\x00\x00\x00\x00", 8) != 0) {
										pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID;
									}
								}

								// Get the Destination MAC Address.
								memcpy(pRsp->stream_dest_mac, pDescriptorStreamIO->acmp_dest_addr, 6);
								if (memcmp(pRsp->stream_dest_mac, "\x00\x00\x00\x00\x00\x00", 6) != 0) {
									pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID;
								}

								// Get the Stream VLAN ID if the other stream identifiers are valid.
								if ((pRsp->flags & (OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID | OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID)) ==
										(OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID | OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID)) {
									pRsp->stream_vlan_id = pDescriptorStreamIO->acmp_stream_vlan_id;
									pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_VLAN_ID_VALID;
								}

								if ((pRsp->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_CONNECTED) &&
										(pRsp->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID) == 0) {
									pRsp->flags &= ~OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_CONNECTED;
									pRsp->flags &= ~OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAMING_WAIT;
								}
							}
						}

						// For the Talker, use the values we are or will use for a connection.
						if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
							openavb_acmp_TalkerStreamInfo_t talkerStreamInfo;
							bool gotTalkerInfo = FALSE;
							memset(&talkerStreamInfo, 0, sizeof(talkerStreamInfo));
							gotTalkerInfo = openavbAVDECCGetTalkerStreamInfo(pDescriptorStreamIO, configIdx, &talkerStreamInfo);

							// Get the Stream ID.
							if (gotTalkerInfo && memcmp(talkerStreamInfo.stream_id, "\x00\x00\x00\x00\x00\x00\x00\x00", 8) != 0) {
								memcpy(pRsp->stream_id, talkerStreamInfo.stream_id, 8);
								pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID;
							}
							else if (pDescriptorStreamIO->stream->stream_addr.mac != NULL) {
								memcpy(pRsp->stream_id, pDescriptorStreamIO->stream->stream_addr.buffer.ether_addr_octet, 6);
								streamUidNbo = htons(pDescriptorStreamIO->stream->stream_uid);
								memcpy(pRsp->stream_id + 6, &streamUidNbo, sizeof(streamUidNbo));
								if (memcmp(pRsp->stream_id, "\x00\x00\x00\x00\x00\x00\x00\x00", 8) != 0) {
									pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_ID_VALID;
								}
							}

							// Get the Destination MAC Address.
							if (gotTalkerInfo && memcmp(talkerStreamInfo.stream_dest_mac, "\x00\x00\x00\x00\x00\x00", 6) != 0) {
								memcpy(pRsp->stream_dest_mac, talkerStreamInfo.stream_dest_mac, 6);
								pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID;
							}
							else if (pDescriptorStreamIO->stream->dest_addr.mac != NULL) {
								memcpy(pRsp->stream_dest_mac, pDescriptorStreamIO->stream->dest_addr.buffer.ether_addr_octet, 6);
								if (memcmp(pRsp->stream_dest_mac, "\x00\x00\x00\x00\x00\x00", 6) != 0) {
									pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_DEST_MAC_VALID;
								}
							}

							// Get the Stream VLAN ID.
							if (gotTalkerInfo && talkerStreamInfo.stream_vlan_id != 0) {
								pRsp->stream_vlan_id = talkerStreamInfo.stream_vlan_id;
								pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_VLAN_ID_VALID;
							}
							else if (pDescriptorStreamIO->stream->vlan_id != 0) {
								pRsp->stream_vlan_id = pDescriptorStreamIO->stream->vlan_id;
								pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_VLAN_ID_VALID;
							}
						}

						// Publish Stream Info extension flags (Milan / IEEE 1722.1-2021).
						pRsp->flags_ex = pDescriptorStreamIO->stream_info_flags_ex;

						// Add TALKER_FAILED / MSRP failure details if present.
						if ((pDescriptorStreamIO->acmp_flags & OPENAVB_ACMP_FLAG_TALKER_FAILED) != 0) {
							pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_FAILURE_VALID;
							pRsp->msrp_failure_code = pDescriptorStreamIO->msrp_failure_code;
							memcpy(pRsp->msrp_failure_bridge_id,
								pDescriptorStreamIO->msrp_failure_bridge_id,
								sizeof(pRsp->msrp_failure_bridge_id));
						}
						else {
							pRsp->flags &= ~OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_FAILURE_VALID;
							pRsp->msrp_failure_code = 0;
							memset(pRsp->msrp_failure_bridge_id, 0, sizeof(pRsp->msrp_failure_bridge_id));
						}

						// Get the stream format.
						openavbAemStreamFormatToBuf(&pDescriptorStreamIO->current_format, pRsp->stream_format);
						pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_STREAM_FORMAT_VALID;

							// For talkers, max_transit_usec is the advertised presentation offset.
							// For listeners, the same INI field is only a local receive tolerance and
							// does not represent MSRP accumulated latency. Advertising it as such makes
							// controllers report false over-latency diagnostics.
							if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT &&
									pDescriptorStreamIO->stream->max_transit_usec > 0) {
								U64 latencyNs = (U64)pDescriptorStreamIO->stream->max_transit_usec * 1000ULL;
								if (latencyNs > 0xffffffffULL) {
									latencyNs = 0xffffffffULL;
								}
								pRsp->msrp_accumulated_latency = (U32)latencyNs;
								pRsp->flags |= OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_ACC_LAT_VALID;
							}
							else {
								pRsp->msrp_accumulated_latency = 0;
								pRsp->flags &= ~OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_MSRP_ACC_LAT_VALID;
							}
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
						}
					}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_NAME:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_NAME:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_ASSOCIATION_ID:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_ASSOCIATION_ID:
			{
				openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();
				openavb_aecp_response_data_get_association_id_t *pRsp =
					&pCommand->entityModelPdu.command_data.getAssociationIDRsp;
				memset(pRsp->association_id, 0, sizeof(pRsp->association_id));

				if (pAem && pAem->pDescriptorEntity) {
					memcpy(pRsp->association_id,
						pAem->pDescriptorEntity->association_id,
						sizeof(pRsp->association_id));
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_MISBEHAVING;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_SAMPLING_RATE:
			{
				openavb_aecp_commandresponse_data_set_sampling_rate_t *pCmd = &pCommand->entityModelPdu.command_data.setSamplingRateCmd;
				openavb_aecp_commandresponse_data_set_sampling_rate_t *pRsp = &pCommand->entityModelPdu.command_data.setSamplingRateRsp;

				if (processCommandCheckRestriction_CorrectController()) {
					if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_AUDIO_UNIT) {
						openavb_aem_descriptor_audio_unit_t *pDescriptorAudioUnit = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
						if (pDescriptorAudioUnit) {
							if (memcmp(&pDescriptorAudioUnit->current_sampling_rate, pCmd->sampling_rate, sizeof(pDescriptorAudioUnit->current_sampling_rate)) == 0) {
								// No change needed.
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
							}
							else {
								// AVDECC_TODO:  Verify that the sample rate is supported, and notify the Talker/Listener of the change.
								//memcpy(&pDescriptorAudioUnit->current_sampling_rate, pCmd->sampling_rate, sizeof(pDescriptorAudioUnit->current_sampling_rate));
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
							}
						}
						else {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_AUDIO_UNIT) {
					openavb_aem_descriptor_audio_unit_t *pDescriptorAudioUnit = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorAudioUnit) {
						memcpy(pRsp->sampling_rate, &pDescriptorAudioUnit->current_sampling_rate, sizeof(pRsp->sampling_rate));
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_SAMPLING_RATE:
			{
				openavb_aecp_command_data_get_sampling_rate_t *pCmd = &pCommand->entityModelPdu.command_data.getSamplingRateCmd;
				openavb_aecp_response_data_get_sampling_rate_t *pRsp = &pCommand->entityModelPdu.command_data.getSamplingRateRsp;

				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_AUDIO_UNIT) {
					openavb_aem_descriptor_audio_unit_t *pDescriptorAudioUnit = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorAudioUnit) {
						U8 *pSrDst = (U8*)(pRsp->sampling_rate);
						memset(pRsp->sampling_rate, 0, sizeof(pRsp->sampling_rate));
						BIT_D2BHTONL(pSrDst, pDescriptorAudioUnit->current_sampling_rate.pull, 29, 0);
						BIT_D2BHTONL(pSrDst, pDescriptorAudioUnit->current_sampling_rate.base, 0, 0);
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_CLOCK_SOURCE:
			{
				openavb_aecp_commandresponse_data_set_clock_source_t *pCmd = &pCommand->entityModelPdu.command_data.setClockSourceCmd;
				openavb_aecp_commandresponse_data_set_clock_source_t *pRsp = &pCommand->entityModelPdu.command_data.setClockSourceRsp;
				U16 configIdx = openavbAemGetConfigIdx();
				openavb_aem_descriptor_clock_domain_t *pDescriptorClockDomain = NULL;
				openavb_aem_descriptor_clock_source_t *pDescriptorClockSource = NULL;
				bool sourceFound = FALSE;
				U16 sourceIdx = 0;

				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;
				pRsp->clock_source_index = pCmd->clock_source_index;
				pRsp->reserved = 0;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN) {
					pDescriptorClockDomain = openavbAemGetDescriptor(configIdx, pCmd->descriptor_type, pCmd->descriptor_index);
				}

				if (processCommandCheckRestriction_CorrectController()) {
					if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN) {
						if (!pDescriptorClockDomain) {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
						else {
							for (sourceIdx = 0; sourceIdx < pDescriptorClockDomain->clock_sources_count; sourceIdx++) {
								if (pDescriptorClockDomain->clock_sources[sourceIdx] == pCmd->clock_source_index) {
									sourceFound = TRUE;
									break;
								}
							}

							if (!sourceFound) {
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
							}
							else {
								pDescriptorClockSource = openavbAemGetDescriptor(configIdx, OPENAVB_AEM_DESCRIPTOR_CLOCK_SOURCE, pCmd->clock_source_index);
								if (!pDescriptorClockSource) {
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
								}
								else if (pDescriptorClockDomain->clock_source_index == pCmd->clock_source_index) {
									// No change needed.
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
								}
								else if (!openavbAVDECCSetClockSource(pDescriptorClockDomain, pDescriptorClockSource, configIdx)) {
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
								}
								else {
									pDescriptorClockDomain->clock_source_index = pCmd->clock_source_index;
									pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
								}
							}
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}

				if (pDescriptorClockDomain) {
					pRsp->clock_source_index = pDescriptorClockDomain->clock_source_index;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_CLOCK_SOURCE:
			{
				openavb_aecp_command_data_get_clock_source_t *pCmd = &pCommand->entityModelPdu.command_data.getClockSourceCmd;
				openavb_aecp_response_data_get_clock_source_t *pRsp = &pCommand->entityModelPdu.command_data.getClockSourceRsp;

				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN) {
					openavb_aem_descriptor_clock_domain_t *pDescriptorClockDomain = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorClockDomain) {
						pRsp->clock_source_index = pDescriptorClockDomain->clock_source_index;
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_CONTROL:
			{
				openavb_aecp_commandresponse_data_set_control_t *pCmd = &pCommand->entityModelPdu.command_data.setControlCmd;
				openavb_aecp_commandresponse_data_set_control_t *pRsp = &pCommand->entityModelPdu.command_data.setControlRsp;
				openavb_aem_descriptor_control_t *pDescriptorControl = NULL;
				bool accessGranted = FALSE;
				bool bypassRestriction = FALSE;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONTROL) {
					pDescriptorControl = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					bypassRestriction = openavbAecpCanBypassControllerRestrictionForControl(pDescriptorControl);
				}

				accessGranted = processCommandCheckRestriction_CorrectController();
				if (accessGranted || bypassRestriction) {
					if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONTROL) {
						if (pDescriptorControl) {
							bool identifyChanged = FALSE;

							if (!accessGranted && bypassRestriction) {
								AVB_LOGF_INFO("Allowing IDENTIFY SET_CONTROL from non-owning controller " ENTITYID_FORMAT,
									pCommand->commonPdu.controller_entity_id);
							}

							if (!openavbAecpSetDescriptorControlValues(pDescriptorControl, pCmd, &identifyChanged)) {
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
							}
							else {
								if (openavbAecpIsIdentifyControl(pDescriptorControl)) {
									openavbAecpApplyIdentifyActionLocked(
										(pDescriptorControl->value_details.linear_uint8[0].current != 0),
										identifyChanged);
								}
								pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
								if (identifyChanged) {
									openavbAecpQueueIdentifyNotificationLocked();
								}
							}
						}
						else {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}

				// Populate response
				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONTROL) {
					if (pDescriptorControl) {
						openavbAecpPopulateSetControlResponse(pRsp, pDescriptorControl);
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_CONTROL:
			{
				openavb_aecp_command_data_get_control_t *pCmd = &pCommand->entityModelPdu.command_data.getControlCmd;
				openavb_aecp_response_data_get_control_t *pRsp = &pCommand->entityModelPdu.command_data.getControlRsp;

				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;

				if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONTROL) {
					openavb_aem_descriptor_control_t *pDescriptorControl = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
					if (pDescriptorControl) {
						openavbAecpPopulateGetControlResponse(pRsp, pDescriptorControl);
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_INCREMENT_CONTROL:
			break;
		case OPENAVB_AEM_COMMAND_CODE_DECREMENT_CONTROL:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_SIGNAL_SELECTOR:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_SIGNAL_SELECTOR:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_MIXER:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_MIXER:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_MATRIX:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_MATRIX:
			break;
		case OPENAVB_AEM_COMMAND_CODE_START_STREAMING:
			{
				openavb_aecp_commandresponse_data_start_streaming_t *pCmd = &pCommand->entityModelPdu.command_data.startStreamingCmd;
				//openavb_aecp_commandresponse_data_start_streaming_t *pRsp = &pCommand->entityModelPdu.command_data.startStreamingRsp;

				if (processCommandCheckRestriction_CorrectController()) {
					if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
						openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
						if (pDescriptorStreamInput) {
							openavbAVDECCPauseStream(pDescriptorStreamInput, FALSE);
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
						}
						else {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
					}
					else if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
						openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
						if (pDescriptorStreamOutput) {
							openavbAVDECCPauseStream(pDescriptorStreamOutput, FALSE);
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
						}
						else {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_STOP_STREAMING:
			{
				openavb_aecp_commandresponse_data_start_streaming_t *pCmd = &pCommand->entityModelPdu.command_data.startStreamingCmd;
				//openavb_aecp_commandresponse_data_start_streaming_t *pRsp = &pCommand->entityModelPdu.command_data.startStreamingRsp;

				if (processCommandCheckRestriction_CorrectController()) {
					if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {
						openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
						if (pDescriptorStreamInput) {
							openavbAVDECCPauseStream(pDescriptorStreamInput, TRUE);
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
						}
						else {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
					}
					else if (pCmd->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
						openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pCmd->descriptor_type, pCmd->descriptor_index);
						if (pDescriptorStreamOutput) {
							openavbAVDECCPauseStream(pDescriptorStreamOutput, TRUE);
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
						}
						else {
							pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						}
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_IMPLEMENTED;
					}
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_ACQUIRED;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_REGISTER_UNSOLICITED_NOTIFICATION:
			openavbAecpRegisterUnsolicitedController(pCommand);
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			break;
		case OPENAVB_AEM_COMMAND_CODE_DEREGISTER_UNSOLICITED_NOTIFICATION:
			if (openavbAecpIsRegisteredUnsolicitedController(pCommand)) {
				openavbAecpClearUnsolicitedController();
			}
			pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			break;
		case OPENAVB_AEM_COMMAND_CODE_IDENTIFY_NOTIFICATION:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_AVB_INFO:
			{
				openavb_aecp_command_data_get_avb_info_t *pCmd = &pCommand->entityModelPdu.command_data.getAvbInfoCmd;
				openavb_aecp_response_data_get_avb_info_t *pRsp = &pCommand->entityModelPdu.command_data.getAvbInfoRsp;
				U16 descriptor_type = pCmd->descriptor_type;
				U16 descriptor_index = pCmd->descriptor_index;
				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->descriptor_type = descriptor_type;
				pRsp->descriptor_index = descriptor_index;
				pRsp->msrp_mappings_count = 0;
				pRsp->msrpMappingsCount = 0;

				openavb_aem_descriptor_avb_interface_t *pDescriptorAvbInterface =
					openavbAemGetDescriptor(openavbAemGetConfigIdx(), descriptor_type, descriptor_index);
				if (pDescriptorAvbInterface) {
					uint8_t domain_number = 0;
					if (osalAVBGrandmasterGetCurrent(pRsp->as_grandmaster_id, &domain_number)) {
						pRsp->as_domain_number = domain_number;
						pRsp->flags |= OPENAVB_AEM_GET_AVB_INFO_COMMAND_GPTP_ENABLED;
					}
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_AS_PATH:
			{
				openavb_aecp_command_data_get_as_path_t *pCmd = &pCommand->entityModelPdu.command_data.getAsPathCmd;
				openavb_aecp_response_data_get_as_path_t *pRsp = &pCommand->entityModelPdu.command_data.getAsPathRsp;
				U16 descriptor_index = pCmd->descriptor_index;
				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->descriptor_index = descriptor_index;
				pRsp->as_path_count = 0;
				pRsp->path_latency = 0;
				pRsp->asPathCount = 0;

				openavb_aem_descriptor_avb_interface_t *pDescriptorAvbInterface =
					openavbAemGetDescriptor(openavbAemGetConfigIdx(), OPENAVB_AEM_DESCRIPTOR_AVB_INTERFACE, descriptor_index);
				if (pDescriptorAvbInterface) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
				}
				else {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS:
			{
				openavb_aecp_command_data_get_counters_t *pCmd = &pCommand->entityModelPdu.command_data.getCountersCmd;
				openavb_aecp_response_data_get_counters_t *pRsp = &pCommand->entityModelPdu.command_data.getCountersRsp;
				pCommand->headers.status = openavbAecpCommandGetCountersHandler(pCmd, pRsp);
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_MEMORY_OBJECT_LENGTH:
			{
				openavb_aecp_command_data_get_memory_object_length_t *pCmd =
					&pCommand->entityModelPdu.command_data.getMemoryObjectLengthCmd;
				openavb_aecp_response_data_get_memory_object_length_t *pRsp =
					&pCommand->entityModelPdu.command_data.getMemoryObjectLengthRsp;
				U16 configIdx = pCmd->configuration_index;
				openavb_aem_descriptor_memory_object_t *pDescriptorMemoryObject;

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->configuration_index = pCmd->configuration_index;
				pRsp->memory_object_index = pCmd->memory_object_index;

				if (configIdx == OPENAVB_AEM_DESCRIPTOR_INVALID) {
					configIdx = openavbAemGetConfigIdx();
					pRsp->configuration_index = configIdx;
				}

				pDescriptorMemoryObject = openavbAemGetDescriptor(
					configIdx,
					OPENAVB_AEM_DESCRIPTOR_MEMORY_OBJECT,
					pCmd->memory_object_index);
				if (!pDescriptorMemoryObject) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				pRsp->length = pDescriptorMemoryObject->length;
				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME:
			{
				openavb_aecp_command_data_get_max_transit_time_t *pCmd = &pCommand->entityModelPdu.command_data.getMaxTransitTimeCmd;
				openavb_aecp_response_data_get_max_transit_time_t *pRsp = &pCommand->entityModelPdu.command_data.getMaxTransitTimeRsp;
				U16 descriptor_type = pCmd->descriptor_type;
				U16 descriptor_index = pCmd->descriptor_index;
				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->descriptor_type = descriptor_type;
				pRsp->descriptor_index = descriptor_index;
				{
					openavb_tl_data_cfg_t *pStreamCfg = openavbAvdeccGetStreamCfg(descriptor_index);
					if (pStreamCfg) {
						pRsp->max_transit_time = (U64)pStreamCfg->max_transit_usec * 1000ULL;
					}
					else {
						pRsp->max_transit_time = 0;
					}
				}

				(void)descriptor_type;
				pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_AUDIO_MAP:
			{
				openavb_aecp_command_data_get_audio_map_t *pCmd = &pCommand->entityModelPdu.command_data.getAudioMapCmd;
				openavb_aecp_response_data_get_audio_map_t *pRsp = &pCommand->entityModelPdu.command_data.getAudioMapRsp;
				U16 descriptor_type = pCmd->descriptor_type;
				U16 descriptor_index = pCmd->descriptor_index;
				U16 map_index = pCmd->map_index;
				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->descriptor_type = descriptor_type;
				pRsp->descriptor_index = descriptor_index;
				pRsp->map_index = map_index;
				pRsp->number_of_maps = 0;
				pRsp->number_of_mappings = 0;
				pRsp->reserved = 0;
				pRsp->mappingsCount = 0;

				if (descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_INPUT &&
						descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_OUTPUT) {
					AVB_LOGF_WARNING("GET_AUDIO_MAP invalid descriptor type=0x%04x index=%u",
						descriptor_type,
						descriptor_index);
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				{
					openavb_aem_descriptor_stream_port_io_t *pDescriptorStreamPort =
						openavbAemGetDescriptor(openavbAemGetConfigIdx(), descriptor_type, descriptor_index);
					if (!pDescriptorStreamPort) {
						AVB_LOGF_WARNING("GET_AUDIO_MAP missing descriptor type=0x%04x index=%u",
							descriptor_type,
							descriptor_index);
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
						break;
					}

					AEM_LOCK();
					if (!pDescriptorStreamPort->dynamic_mappings_supported) {
						// Some profiles expose a valid stream port without any editable audio mappings
						// (for example CRF-only listener inputs in the tonegen/CRF profile). Milan
						// controllers still expect GET_AUDIO_MAP to succeed for the descriptor and
						// report an empty map set rather than "not supported".
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
					else if (!openavbAecpGetDynamicAudioMap(pDescriptorStreamPort, pRsp)) {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
					}
					else {
						pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
					}
					AEM_UNLOCK();
				}
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_ADD_AUDIO_MAPPINGS:
			{
				openavb_aecp_commandresponse_data_audio_mappings_t cmd =
					pCommand->entityModelPdu.command_data.addAudioMappingsCmd;
				openavb_aecp_commandresponse_data_audio_mappings_t *pCmd = &cmd;
				openavb_aecp_commandresponse_data_audio_mappings_t *pRsp =
					&pCommand->entityModelPdu.command_data.addAudioMappingsRsp;
				openavb_aem_descriptor_stream_port_io_t *pDescriptorStreamPort = NULL;
				openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();
				U16 appliedCount = 0;

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;
				pRsp->reserved = 0;

				if (!processCommandCheckRestriction_LockingController()) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_LOCKED;
					break;
				}

				if (pCmd->descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_INPUT &&
						pCmd->descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_OUTPUT) {
					AVB_LOGF_WARNING("ADD_AUDIO_MAPPINGS invalid descriptor type=0x%04x index=%u",
						pCmd->descriptor_type,
						pCmd->descriptor_index);
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				pDescriptorStreamPort = openavbAemGetDescriptor(openavbAemGetConfigIdx(),
					pCmd->descriptor_type,
					pCmd->descriptor_index);
				if (!pDescriptorStreamPort) {
					AVB_LOGF_WARNING("ADD_AUDIO_MAPPINGS missing descriptor type=0x%04x index=%u",
						pCmd->descriptor_type,
						pCmd->descriptor_index);
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				if (pCmd->descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_INPUT) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
					break;
				}

				AEM_LOCK();
				if (!pDescriptorStreamPort->dynamic_mappings_supported) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
				}
				else if (!openavbAecpAudioMappingsAreValidForInput(pDescriptorStreamPort, pCmd)) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_BAD_ARGUMENTS;
				}
				else {
					appliedCount = openavbAecpApplyAddAudioMappings(pDescriptorStreamPort, pCmd, pRsp);
					if (appliedCount > 0 && pAem && pAem->pDescriptorEntity) {
						pAem->pDescriptorEntity->available_index++;
					}
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
				}
				AEM_UNLOCK();
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_REMOVE_AUDIO_MAPPINGS:
			{
				openavb_aecp_commandresponse_data_audio_mappings_t cmd =
					pCommand->entityModelPdu.command_data.removeAudioMappingsCmd;
				openavb_aecp_commandresponse_data_audio_mappings_t *pCmd = &cmd;
				openavb_aecp_commandresponse_data_audio_mappings_t *pRsp =
					&pCommand->entityModelPdu.command_data.removeAudioMappingsRsp;
				openavb_aem_descriptor_stream_port_io_t *pDescriptorStreamPort = NULL;
				openavb_avdecc_entity_model_t *pAem = openavbAemGetModel();
				U16 appliedCount = 0;

				memset(pRsp, 0, sizeof(*pRsp));
				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;
				pRsp->reserved = 0;

				if (!processCommandCheckRestriction_LockingController()) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_ENTITY_LOCKED;
					break;
				}

				if (pCmd->descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_INPUT &&
						pCmd->descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_OUTPUT) {
					AVB_LOGF_WARNING("REMOVE_AUDIO_MAPPINGS invalid descriptor type=0x%04x index=%u",
						pCmd->descriptor_type,
						pCmd->descriptor_index);
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				pDescriptorStreamPort = openavbAemGetDescriptor(openavbAemGetConfigIdx(),
					pCmd->descriptor_type,
					pCmd->descriptor_index);
				if (!pDescriptorStreamPort) {
					AVB_LOGF_WARNING("REMOVE_AUDIO_MAPPINGS missing descriptor type=0x%04x index=%u",
						pCmd->descriptor_type,
						pCmd->descriptor_index);
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NO_SUCH_DESCRIPTOR;
					break;
				}

				if (pCmd->descriptor_type != OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_INPUT) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
					break;
				}

				AEM_LOCK();
				if (!pDescriptorStreamPort->dynamic_mappings_supported) {
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_NOT_SUPPORTED;
				}
				else {
					appliedCount = openavbAecpApplyRemoveAudioMappings(pDescriptorStreamPort, pCmd, pRsp);
					if (appliedCount > 0 && pAem && pAem->pDescriptorEntity) {
						pAem->pDescriptorEntity->available_index++;
					}
					pCommand->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
				}
				AEM_UNLOCK();
			}
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_VIDEO_MAP:
			break;
		case OPENAVB_AEM_COMMAND_CODE_ADD_VIDEO_MAPPINGS:
			break;
		case OPENAVB_AEM_COMMAND_CODE_REMOVE_VIDEO_MAPPINGS:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_SENSOR_MAP:
			break;
		case OPENAVB_AEM_COMMAND_CODE_ADD_SENSOR_MAPPINGS:
			break;
		case OPENAVB_AEM_COMMAND_CODE_REMOVE_SENSOR_MAPPINGS:
			break;
		case OPENAVB_AEM_COMMAND_CODE_START_OPERATION:
			break;
		case OPENAVB_AEM_COMMAND_CODE_ABORT_OPERATION:
			break;
		case OPENAVB_AEM_COMMAND_CODE_OPERATION_STATUS:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_ADD_KEY:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_DELETE_KEY:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_GET_KEY_LIST:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_GET_KEY:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_ADD_KEY_TO_CHAIN:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_DELETE_KEY_FROM_CHAIN:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_GET_KEYCHAIN_LIST:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_GET_IDENTITY:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_ADD_TOKEN:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTH_DELETE_TOKEN:
			break;
		case OPENAVB_AEM_COMMAND_CODE_AUTHENTICATE:
			break;
		case OPENAVB_AEM_COMMAND_CODE_DEAUTHENTICATE:
			break;
		case OPENAVB_AEM_COMMAND_CODE_ENABLE_TRANSPORT_SECURITY:
			break;
		case OPENAVB_AEM_COMMAND_CODE_DISABLE_TRANSPORT_SECURITY:
			break;
		case OPENAVB_AEM_COMMAND_CODE_ENABLE_STREAM_ENCRYPTION:
			break;
		case OPENAVB_AEM_COMMAND_CODE_DISABLE_STREAM_ENCRYPTION:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_MEMORY_OBJECT_LENGTH:
			break;
		case OPENAVB_AEM_COMMAND_CODE_SET_STREAM_BACKUP:
			break;
		case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_BACKUP:
			break;
		case OPENAVB_AEM_COMMAND_CODE_EXPANSION:
			break;
		default:
			break;
	}

	if (pCommand->headers.status != OPENAVB_AEM_COMMAND_STATUS_SUCCESS) {
		AVB_LOGF_WARNING("AEM command non-success: seq=%u type=0x%04x status=%u",
			pCommand->commonPdu.sequence_id,
			pCommand->entityModelPdu.command_type,
			pCommand->headers.status);
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntityStateMachine()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	bool bRunning = TRUE;

	openavb_aecp_sm_entity_model_entity_state_t state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING;

	// Lock such that the mutex is held unless waiting for a semaphore. Synchronous processing of command responses.
	AECP_SM_LOCK();
	while (bRunning) {
		switch (state) {
			case OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING:
				AVB_LOG_DEBUG("State:  OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING");

				if (openavbAecpHandleMaintenanceTimeoutsLocked()) {
					state = hasQueuedUnsolicited() ?
						OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE :
						OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING;
					break;
				}
				if (hasQueuedUnsolicited()) {
					openavbAecpSMEntityModelEntityVars.doUnsolicited = TRUE;
					state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE;
					break;
				}
				if (openavbAecpSMEntityModelEntityVars.rcvdAEMCommand || hasQueuedCommands()) {
					state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_RECEIVED_COMMAND;
					break;
				}

				// Wait for a change in state
				while (state == OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING && bRunning) {
					U32 timeoutMSec = 0;
					bool useTimedWait = FALSE;
					struct timespec nextDeadline;

					if (openavbAecpGetNextMaintenanceDeadlineLocked(&nextDeadline)) {
						struct timespec now;

						CLOCK_GETTIME(OPENAVB_CLOCK_REALTIME, &now);
						if (openavbTimeTimespecCmp(&now, &nextDeadline) >= 0) {
							if (openavbAecpHandleMaintenanceTimeoutsLocked()) {
								state = hasQueuedUnsolicited() ?
									OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE :
									OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING;
							}
							continue;
						}

						timeoutMSec = openavbTimeUntilMSec(&now, &nextDeadline);
						if (timeoutMSec == 0) {
							timeoutMSec = 1;
						}
						useTimedWait = TRUE;
					}

					AECP_SM_UNLOCK();
					SEM_ERR_T(err);
					if (useTimedWait) {
						SEM_TIMEDWAIT(openavbAecpSMEntityModelEntityWaitingSemaphore, timeoutMSec, err);
					}
					else {
						SEM_WAIT(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
					}
					AECP_SM_LOCK();

					if (SEM_IS_ERR_NONE(err)) {
						if (openavbAecpSMEntityModelEntityVars.doTerminate) {
							bRunning = FALSE;
						}
						else if (hasQueuedUnsolicited()) {
							openavbAecpSMEntityModelEntityVars.doUnsolicited = TRUE;
							state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE;
						}
						else if (openavbAecpSMEntityModelEntityVars.rcvdAEMCommand || hasQueuedCommands()) {
							state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_RECEIVED_COMMAND;
						}
					}
					else if (useTimedWait && SEM_IS_ERR_TIMEOUT(err)) {
						if (openavbAecpHandleMaintenanceTimeoutsLocked()) {
							state = hasQueuedUnsolicited() ?
								OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE :
								OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING;
						}
					}
				}
				break;

			case OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE:
				AVB_LOG_DEBUG("State:  OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE");

				while (hasQueuedUnsolicited()) {
					openavb_aecp_AEMCommandResponse_t *pUnsolicited = getNextUnsolicitedFromQueue();
					if (!pUnsolicited) {
						break;
					}
					openavbAecpMessageSend(pUnsolicited);
					free(pUnsolicited);
				}
				openavbAecpSMEntityModelEntityVars.doUnsolicited = hasQueuedUnsolicited();

				if (openavbAecpSMEntityModelEntityVars.rcvdAEMCommand || hasQueuedCommands()) {
					state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_RECEIVED_COMMAND;
				}
				else if (hasQueuedUnsolicited()) {
					state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE;
				}
				else {
					state = OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING;
				}
				break;

			case OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_RECEIVED_COMMAND:
				AVB_LOG_DEBUG("State:  OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_RECEIVED_COMMAND");
				openavbAecpSMEntityModelEntityVars.rcvdAEMCommand = FALSE;

				while (TRUE) {
					openavbAecpSMEntityModelEntityVars.rcvdCommand = getNextCommandFromQueue();
					if (openavbAecpSMEntityModelEntityVars.rcvdCommand == NULL) {
						break;
					}

					if (memcmp(openavbAecpSMEntityModelEntityVars.rcvdCommand->headers.target_entity_id,
							openavbAecpSMGlobalVars.myEntityID,
							sizeof(openavbAecpSMGlobalVars.myEntityID)) != 0) {
						// Not intended for us.
						free(openavbAecpSMEntityModelEntityVars.rcvdCommand);
						continue;
					}

					if (openavbAecpSMEntityModelEntityVars.rcvdCommand->headers.message_type ==
							OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND) {
						processVendorUniqueCommand();
					}
					else if (openavbAecpSMEntityModelEntityVars.rcvdCommand->entityModelPdu.command_type == OPENAVB_AEM_COMMAND_CODE_ACQUIRE_ENTITY) {
						acquireEntity();
					}
					else if (openavbAecpSMEntityModelEntityVars.rcvdCommand->entityModelPdu.command_type == OPENAVB_AEM_COMMAND_CODE_LOCK_ENTITY) {
						lockEntity();
					}
					else if (openavbAecpSMEntityModelEntityVars.rcvdCommand->entityModelPdu.command_type == OPENAVB_AEM_COMMAND_CODE_ENTITY_AVAILABLE) {
						// State machine defines just returning the request command. Doing that in the processCommand function for consistency.
						processCommand();
					}
					else {
						processCommand();
					}

					openavbAecpMessageSend(openavbAecpSMEntityModelEntityVars.rcvdCommand);
					free(openavbAecpSMEntityModelEntityVars.rcvdCommand);
				}

				state = hasQueuedUnsolicited() ?
					OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_UNSOLICITED_RESPONSE :
					OPENAVB_AECP_SM_ENTITY_MODEL_ENTITY_STATE_WAITING;
				break;

			default:
				AVB_LOG_ERROR("State:  Unknown");
				bRunning = FALSE;	// Unexpected
				break;

		}
	}
	AECP_SM_UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void* openavbAecpSMEntityModelEntityThreadFn(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	openavbAecpSMEntityModelEntityStateMachine();
	AVB_TRACE_EXIT(AVB_TRACE_AECP);
	return NULL;
}

void openavbAecpSMEntityModelEntityStart()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	MUTEX_ATTR_HANDLE(mtq);
	MUTEX_ATTR_INIT(mtq);
	MUTEX_ATTR_SET_TYPE(mtq, MUTEX_ATTR_TYPE_DEFAULT);
	MUTEX_ATTR_SET_NAME(mtq, "openavbAecpQueueMutex");
	MUTEX_CREATE_ERR();
	MUTEX_CREATE(openavbAecpQueueMutex, mtq);
	MUTEX_LOG_ERR("Could not create/initialize 'openavbAecpQueueMutex' mutex");

	MUTEX_ATTR_HANDLE(mta);
	MUTEX_ATTR_INIT(mta);
	MUTEX_ATTR_SET_TYPE(mta, MUTEX_ATTR_TYPE_DEFAULT);
	MUTEX_ATTR_SET_NAME(mta, "openavbAecpSMMutex");
	MUTEX_CREATE(openavbAecpSMMutex, mta);
	MUTEX_LOG_ERR("Could not create/initialize 'openavbAecpSMMutex' mutex");

	SEM_ERR_T(err);
	SEM_INIT(openavbAecpSMEntityModelEntityWaitingSemaphore, 1, err);
	SEM_LOG_ERR(err);

	memset(&openavbAecpSMEntityModelEntityVars, 0, sizeof(openavbAecpSMEntityModelEntityVars));
	s_identifyLogActive = FALSE;
	s_identifyLogTimeoutArmed = FALSE;
	memset(&s_identifyLogTimeout, 0, sizeof(s_identifyLogTimeout));
	s_acquiredOwnerActivityValid = FALSE;
	s_lockedOwnerActivityValid = FALSE;
	s_unsolicitedControllerActivityValid = FALSE;
	memset(&s_acquiredOwnerActivity, 0, sizeof(s_acquiredOwnerActivity));
	memset(&s_lockedOwnerActivity, 0, sizeof(s_lockedOwnerActivity));
	memset(&s_unsolicitedControllerActivity, 0, sizeof(s_unsolicitedControllerActivity));
	s_openavbSystemUniqueIdValid = FALSE;
	s_openavbSystemUniqueId = 0;
	memset(s_openavbSystemName, 0, sizeof(s_openavbSystemName));

	// Initialize the linked list (queue).
	s_commandQueue = openavbListNewList();
	s_unsolicitedQueue = openavbListNewList();
	s_unsolicitedStateCache = openavbListNewList();

	// Start the Advertise Entity State Machine
	bool errResult;
	THREAD_CREATE(openavbAecpSMEntityModelEntityThread, openavbAecpSMEntityModelEntityThread, NULL, openavbAecpSMEntityModelEntityThreadFn, NULL);
	THREAD_CHECK_ERROR(openavbAecpSMEntityModelEntityThread, "Thread / task creation failed", errResult);
	if (errResult);		// Already reported

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntityStop()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	openavbAecpSMEntityModelEntitySet_doTerminate(TRUE);

	THREAD_JOIN(openavbAecpSMEntityModelEntityThread, NULL);

	// Delete the linked list (queue).
	openavb_aecp_AEMCommandResponse_t *item;
	while ((item = getNextCommandFromQueue()) != NULL) {
		 free(item);
	}
	openavbListDeleteListShallow(s_commandQueue);
	while ((item = getNextUnsolicitedFromQueue()) != NULL) {
		free(item);
	}
	openavbListDeleteListShallow(s_unsolicitedQueue);
	openavbListDeleteList(s_unsolicitedStateCache);

	SEM_ERR_T(err);
	SEM_DESTROY(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
	SEM_LOG_ERR(err);

	MUTEX_CREATE_ERR();
	MUTEX_DESTROY(openavbAecpQueueMutex);
	MUTEX_LOG_ERR("Could not destroy 'openavbAecpQueueMutex' mutex");
	MUTEX_DESTROY(openavbAecpSMMutex);
	MUTEX_LOG_ERR("Could not destroy 'openavbAecpSMMutex' mutex");

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntitySet_rcvdCommand(openavb_aecp_AEMCommandResponse_t *rcvdCommand)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	if (memcmp(rcvdCommand->headers.target_entity_id,
			openavbAecpSMGlobalVars.myEntityID,
			sizeof(openavbAecpSMGlobalVars.myEntityID)) != 0) {
		// Not intended for us.
		free(rcvdCommand);
		return;
	}

	int result = addCommandToQueue(rcvdCommand);
	if (result < 0) {
		AVB_LOG_DEBUG("addCommandToQueue failed");
		free(rcvdCommand);
	}
	else if (result == 0) {
		// We just added an item to an empty queue.
		// Notify the machine state thread that something is waiting.
		AECP_SM_LOCK();
		openavbAecpSMEntityModelEntityVars.rcvdAEMCommand = TRUE;

		SEM_ERR_T(err);
		SEM_POST(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
		SEM_LOG_ERR(err);
		AECP_SM_UNLOCK();
	}
	else {
		// We added an item to a non-empty queue.
		// Assume it will be handled when the other items in the queue are handled.
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntitySet_unsolicited(openavb_aecp_AEMCommandResponse_t *unsolicited)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	AECP_SM_LOCK();
	if (openavbAecpQueueUnsolicitedResponseLocked(unsolicited)) {
		openavbAecpSMEntityModelEntityVars.doUnsolicited = TRUE;

		SEM_ERR_T(err);
		SEM_POST(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
		SEM_LOG_ERR(err);
	}

	AECP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntityNotifyStreamState(U16 descriptor_type, U16 descriptor_index)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	AECP_SM_LOCK();
	if (openavbAecpNotifyStreamStateChangedLocked(descriptor_type, descriptor_index)) {
		SEM_ERR_T(err);
		SEM_POST(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
		SEM_LOG_ERR(err);
	}
	AECP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntityNotifyCounters(U16 descriptor_type, U16 descriptor_index)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	AECP_SM_LOCK();
	if (openavbAecpNotifyCountersChangedLocked(descriptor_type, descriptor_index)) {
		SEM_ERR_T(err);
		SEM_POST(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
		SEM_LOG_ERR(err);
	}
	AECP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void openavbAecpSMEntityModelEntitySet_doTerminate(bool value)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	AECP_SM_LOCK();
	openavbAecpSMEntityModelEntityVars.doTerminate = value;

	SEM_ERR_T(err);
	SEM_POST(openavbAecpSMEntityModelEntityWaitingSemaphore, err);
	SEM_LOG_ERR(err);

	AECP_SM_UNLOCK();
	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}
