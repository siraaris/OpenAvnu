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
 * MODULE : AVDECC Enumeration and control protocol (AECP) Message Handler
 * MODULE SUMMARY : Implements the AVDECC Enumeration and control protocol (AECP) message handler
 ******************************************************************
 */

#include "openavb_platform.h"

#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define	AVB_LOG_COMPONENT	"AECP"
#include "openavb_log.h"

#include "openavb_debug.h"
#include "openavb_rawsock.h"
#include "openavb_avtp.h"
#include "openavb_srp.h"
#include "openavb_aecp.h"
#include "openavb_aecp_sm_entity_model_entity.h"
#include "openavb_time_osal_pub.h"
#include "openavb_avdecc_pub.h"

#define INVALID_SOCKET (-1)

// message length
#define AVTP_HDR_LEN 12
#define AECP_DATA_LEN 1480		// AVDECC_TODO - Need to figure out a valid length
#define AECP_FRAME_LEN (ETH_HDR_LEN_VLAN + AVTP_HDR_LEN + AECP_DATA_LEN)

// number of buffers (arbitrary, and rounded up by rawsock)
#define AECP_NUM_BUFFERS 3
#define OPENAVB_AECP_AA_TLV_MODE_READ (0u)
#define OPENAVB_AECP_AA_TLV_HEADER_LENGTH (10u)
#define OPENAVB_AECP_AA_MAX_MEMORY_DATA_LENGTH (1400u)

// do cast from ether_addr to U8*
#define ADDR_PTR(A) (U8*)(&((A)->ether_addr_octet))

extern openavb_avdecc_cfg_t gAvdeccCfg;

static void *rxSock = NULL;
static void *txSock = NULL;
static struct ether_addr intfAddr;

extern openavb_aecp_sm_global_vars_t openavbAecpSMGlobalVars;

THREAD_TYPE(openavbAecpMessageRxThread);
THREAD_DEFINITON(openavbAecpMessageRxThread);

static bool bRunning = FALSE;

static bool openavbAecpShouldLogNonAvtp(void)
{
	static time_t last_warn_sec = 0;
	struct timespec now;

	if (!CLOCK_GETTIME(OPENAVB_CLOCK_MONOTONIC, &now)) {
		return true;
	}

	if (now.tv_sec != last_warn_sec) {
		last_warn_sec = now.tv_sec;
		return true;
	}

	return false;
}

static U16 openavbAecpGetAemCommandSpecificLength(const openavb_aecp_AEMCommandResponse_t *pCommand)
{
	if (!pCommand || pCommand->headers.control_data_length <= 12) {
		return 0;
	}

	return (U16)(pCommand->headers.control_data_length - 12);
}

static U16 openavbAecpGetMvuCommandSpecificLength(const openavb_aecp_AEMCommandResponse_t *pCommand)
{
	if (!pCommand || pCommand->headers.control_data_length <= 18) {
		return 0;
	}

	return (U16)(pCommand->headers.control_data_length - 18);
}

static bool openavbAecpParseAudioMappingsPayload(
	U8 *pSrc,
	U16 command_specific_length,
	openavb_aecp_commandresponse_data_audio_mappings_t *pDst)
{
	U16 i;
	U16 mappingsBytes;

	if (!pSrc || !pDst) {
		return false;
	}
	if (command_specific_length < 8) {
		return false;
	}

	OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
	OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
	OCT_B2DNTOHS(pDst->number_of_mappings, pSrc);
	OCT_B2DNTOHS(pDst->reserved, pSrc);

	mappingsBytes = (U16)(pDst->number_of_mappings * sizeof(openavb_aecp_audio_mapping_t));
	if (command_specific_length != (U16)(8 + mappingsBytes)) {
		return false;
	}
	if (pDst->number_of_mappings > OPENAVB_AECP_AUDIO_MAP_MAX_MAPPINGS) {
		return false;
	}

	pDst->mappingsCount = pDst->number_of_mappings;
	for (i = 0; i < pDst->mappingsCount; i++) {
		OCT_B2DNTOHS(pDst->mappings[i].mapping_stream_index, pSrc);
		OCT_B2DNTOHS(pDst->mappings[i].mapping_stream_channel, pSrc);
		OCT_B2DNTOHS(pDst->mappings[i].mapping_cluster_offset, pSrc);
		OCT_B2DNTOHS(pDst->mappings[i].mapping_cluster_channel, pSrc);
	}

	return true;
}

static void openavbAecpSerializeAudioMappings(
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

static void openavbAecpSerializeSetControlResponseValues(
	U8 **ppDst,
	const openavb_aecp_commandresponse_data_set_control_t *pSrc,
	const openavb_aem_descriptor_control_t *pDescriptorControl)
{
	int i1;
	U8 *pDst;

	if (!ppDst || !*ppDst || !pSrc || !pDescriptorControl) {
		return;
	}

	pDst = *ppDst;
	for (i1 = 0; i1 < pSrc->valuesCount; i1++) {
		switch (pDescriptorControl->control_value_type) {
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT8:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8:
				OCT_D2BHTONB(pDst, pSrc->values.linear_int8[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT16:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT16:
				OCT_D2BHTONS(pDst, pSrc->values.linear_int16[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT32:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT32:
				OCT_D2BHTONL(pDst, pSrc->values.linear_int32[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT64:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT64:
				OCT_D2BMEMCP(pDst, &pSrc->values.linear_int64[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_FLOAT:
				OCT_D2BMEMCP(pDst, &pSrc->values.linear_float[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_DOUBLE:
				OCT_D2BMEMCP(pDst, &pSrc->values.linear_double[i1]);
				break;
			default:
				break;
		}
	}

	*ppDst = pDst;
}

static void openavbAecpSerializeGetControlResponseValues(
	U8 **ppDst,
	const openavb_aecp_response_data_get_control_t *pSrc,
	const openavb_aem_descriptor_control_t *pDescriptorControl)
{
	int i1;
	U8 *pDst;

	if (!ppDst || !*ppDst || !pSrc || !pDescriptorControl) {
		return;
	}

	pDst = *ppDst;
	for (i1 = 0; i1 < pSrc->valuesCount; i1++) {
		switch (pDescriptorControl->control_value_type) {
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT8:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8:
				OCT_D2BHTONB(pDst, pSrc->values.linear_int8[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT16:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT16:
				OCT_D2BHTONS(pDst, pSrc->values.linear_int16[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT32:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT32:
				OCT_D2BHTONL(pDst, pSrc->values.linear_int32[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT64:
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT64:
				OCT_D2BMEMCP(pDst, &pSrc->values.linear_int64[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_FLOAT:
				OCT_D2BMEMCP(pDst, &pSrc->values.linear_float[i1]);
				break;
			case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_DOUBLE:
				OCT_D2BMEMCP(pDst, &pSrc->values.linear_double[i1]);
				break;
			default:
				break;
		}
	}

	*ppDst = pDst;
}

static void openavbAecpMessageTxAddressAccessResponse(
	const openavb_aecp_AEMCommandResponse_t *pCommand,
	U16 status,
	U64 address,
	const U8 *pData,
	U16 dataLength)
{
	U8 *pBuf;
	U8 *pDst;
	U8 *pControlDataLength;
	U8 *pControlDataLengthStartMarker;
	U32 size;
	unsigned int hdrlen = 0;
	U16 tlvCount = (status == OPENAVB_AECP_AA_STATUS_SUCCESS) ? 1 : 0;
	U16 modeLength = (U16)(((OPENAVB_AECP_AA_TLV_MODE_READ & 0x0f) << 12) | (dataLength & 0x0fff));
	U64 addressNetworkOrder = htonll(address);

	if (!pCommand) {
		return;
	}

	pBuf = openavbRawsockGetTxFrame(txSock, TRUE, &size);
	if (!pBuf) {
		AVB_LOG_ERROR("No TX buffer for ADDRESS_ACCESS response");
		return;
	}
	if (size < AECP_FRAME_LEN) {
		AVB_LOG_ERROR("TX buffer too small for ADDRESS_ACCESS response");
		openavbRawsockRelTxFrame(txSock, pBuf);
		return;
	}

	memset(pBuf, 0, AECP_FRAME_LEN);
	openavbRawsockTxFillHdr(txSock, pBuf, &hdrlen);
	memcpy(pBuf, pCommand->host, ETH_ALEN);

	pDst = pBuf + hdrlen;
	BIT_D2BHTONB(pDst, pCommand->headers.cd, 7, 0);
	BIT_D2BHTONB(pDst, pCommand->headers.subtype, 0, 1);
	BIT_D2BHTONB(pDst, pCommand->headers.sv, 7, 0);
	BIT_D2BHTONB(pDst, pCommand->headers.version, 4, 0);
	BIT_D2BHTONB(pDst, OPENAVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE, 0, 1);
	BIT_D2BHTONS(pDst, status, 11, 0);
	pControlDataLength = pDst;
	BIT_D2BHTONS(pDst, 0, 0, 2);
	OCT_D2BMEMCP(pDst, pCommand->headers.target_entity_id);
	pControlDataLengthStartMarker = pDst;

	OCT_D2BMEMCP(pDst, pCommand->commonPdu.controller_entity_id);
	OCT_D2BHTONS(pDst, pCommand->commonPdu.sequence_id);
	OCT_D2BHTONS(pDst, tlvCount);
	if (tlvCount != 0) {
		OCT_D2BHTONS(pDst, modeLength);
		OCT_D2BMEMCP(pDst, &addressNetworkOrder);
		if (dataLength != 0 && pData) {
			OCT_D2BBUFCP(pDst, pData, dataLength);
		}
	}

	BIT_D2BHTONS(pControlDataLength, (pDst - pControlDataLengthStartMarker), 0, 2);
	openavbRawsockTxFrameReady(txSock, pBuf, pDst - pBuf, 0);
	openavbRawsockSend(txSock);
}

static void openavbAecpHandleAddressAccessCommand(
	openavb_aecp_AEMCommandResponse_t *pCommand,
	U8 *pSrc,
	U16 commandSpecificLength)
{
	U16 tlvCount = 0;
	U16 modeLength = 0;
	U16 mode = 0;
	U16 requestedLength = 0;
	U16 status = OPENAVB_AECP_AA_STATUS_SUCCESS;
	U16 bytesRead = 0;
	U16 expectedCommandSpecificLength = 0;
	U64 address = 0;
	U64 addressNetworkOrder = 0;
	U8 data[OPENAVB_AECP_AA_MAX_MEMORY_DATA_LENGTH];

	if (!pCommand || !pSrc || commandSpecificLength < 2) {
		if (pCommand) {
			openavbAecpMessageTxAddressAccessResponse(
				pCommand,
				OPENAVB_AECP_AA_STATUS_TLV_INVALID,
				0,
				NULL,
				0);
		}
		return;
	}

	OCT_B2DNTOHS(tlvCount, pSrc);
	if (tlvCount != 1 || commandSpecificLength < (U16)(2 + OPENAVB_AECP_AA_TLV_HEADER_LENGTH)) {
		openavbAecpMessageTxAddressAccessResponse(
			pCommand,
			OPENAVB_AECP_AA_STATUS_TLV_INVALID,
			0,
			NULL,
			0);
		return;
	}

	OCT_B2DNTOHS(modeLength, pSrc);
	mode = (U16)((modeLength >> 12) & 0x0f);
	requestedLength = (U16)(modeLength & 0x0fff);
	OCT_B2DMEMCP(&addressNetworkOrder, pSrc);
	address = ntohll(addressNetworkOrder);
	expectedCommandSpecificLength = (U16)(2 + OPENAVB_AECP_AA_TLV_HEADER_LENGTH + requestedLength);

	if (mode != OPENAVB_AECP_AA_TLV_MODE_READ) {
		openavbAecpMessageTxAddressAccessResponse(
			pCommand,
			OPENAVB_AECP_AA_STATUS_UNSUPPORTED,
			address,
			NULL,
			0);
		return;
	}
	if (commandSpecificLength != expectedCommandSpecificLength &&
			commandSpecificLength != (U16)(2 + OPENAVB_AECP_AA_TLV_HEADER_LENGTH)) {
		openavbAecpMessageTxAddressAccessResponse(
			pCommand,
			OPENAVB_AECP_AA_STATUS_TLV_INVALID,
			address,
			NULL,
			0);
		return;
	}
	if (requestedLength > OPENAVB_AECP_AA_MAX_MEMORY_DATA_LENGTH) {
		openavbAecpMessageTxAddressAccessResponse(
			pCommand,
			OPENAVB_AECP_AA_STATUS_TLV_INVALID,
			address,
			NULL,
			0);
		return;
	}

	if (!openavbAvdeccReadEntityLogo(address, requestedLength, data, &bytesRead, &status)) {
		openavbAecpMessageTxAddressAccessResponse(
			pCommand,
			status,
			address,
			NULL,
			0);
		return;
	}

	openavbAecpMessageTxAddressAccessResponse(
		pCommand,
		OPENAVB_AECP_AA_STATUS_SUCCESS,
		address,
		data,
		bytesRead);
}

void openavbAecpCloseSocket()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	if (rxSock) {
		openavbRawsockClose(rxSock);
		rxSock = NULL;
	}
	if (txSock) {
		openavbRawsockClose(txSock);
		txSock = NULL;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

bool openavbAecpOpenSocket(const char* ifname, U16 vlanID, U8 vlanPCP)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	hdr_info_t hdr;

#ifndef UBUNTU
	// This is the normal case for most of our supported platforms
	rxSock = openavbRawsockOpen(ifname, TRUE, FALSE, ETHERTYPE_8021Q, AECP_FRAME_LEN, AECP_NUM_BUFFERS);
#else
	rxSock = openavbRawsockOpen(ifname, TRUE, FALSE, ETHERTYPE_AVTP, AECP_FRAME_LEN, AECP_NUM_BUFFERS);
#endif
	txSock = openavbRawsockOpen(ifname, FALSE, TRUE, ETHERTYPE_AVTP, AECP_FRAME_LEN, AECP_NUM_BUFFERS);

	if (txSock && rxSock
		&& openavbRawsockGetAddr(txSock, ADDR_PTR(&intfAddr))
		&& openavbRawsockRxMulticast(rxSock, TRUE, ADDR_PTR(&intfAddr))) // Only accept packets sent directly to this interface
	{
		if (!openavbRawsockRxAVTPSubtype(rxSock, OPENAVB_AECP_AVTP_SUBTYPE | 0x80)) {
			AVB_LOG_DEBUG("RX AVTP Subtype not supported");
		}

		memset(&hdr, 0, sizeof(hdr_info_t));
		hdr.shost = ADDR_PTR(&intfAddr);
		// hdr.dhost; // Set at tx time.
		hdr.ethertype = ETHERTYPE_AVTP;
		if (vlanID != 0 || vlanPCP != 0) {
			hdr.vlan = TRUE;
			hdr.vlan_pcp = vlanPCP;
			hdr.vlan_vid = vlanID;
			AVB_LOGF_DEBUG("VLAN pcp=%d vid=%d", hdr.vlan_pcp, hdr.vlan_vid);
		}
		if (!openavbRawsockTxSetHdr(txSock, &hdr)) {
			AVB_LOG_ERROR("TX socket Header Failure");
			openavbAecpCloseSocket();
			AVB_TRACE_EXIT(AVB_TRACE_AECP);
			return false;
		}

		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return true;
	}

	AVB_LOG_ERROR("Invalid socket");
	openavbAecpCloseSocket();

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
	return false;
}

static void openavbAecpMessageRxFrameParse(U8* payload, int payload_len, hdr_info_t *hdr)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

#if 0
	AVB_LOGF_DEBUG("openavbAecpMessageRxFrameParse packet data (length %d):", payload_len);
	AVB_LOG_BUFFER(AVB_LOG_LEVEL_DEBUG, payload, payload_len, 16);
#endif

	// Save the source address
	openavb_aecp_AEMCommandResponse_t * openavbAecpCommandResponse = calloc(1, sizeof(openavb_aecp_AEMCommandResponse_t));
	if (!openavbAecpCommandResponse) { return; }
	memcpy(openavbAecpCommandResponse->host, hdr->shost, ETH_ALEN);

	U8 *pSrc = payload;
	{
		// AVTP Control Header
		openavb_aecp_control_header_t *pDst = &openavbAecpCommandResponse->headers;
		BIT_B2DNTOHB(pDst->cd, pSrc, 0x80, 7, 0);
		BIT_B2DNTOHB(pDst->subtype, pSrc, 0x7f, 0, 1);
		BIT_B2DNTOHB(pDst->sv, pSrc, 0x80, 7, 0);
		BIT_B2DNTOHB(pDst->version, pSrc, 0x70, 4, 0);
		BIT_B2DNTOHB(pDst->message_type, pSrc, 0x0f, 0, 1);
		BIT_B2DNTOHB(pDst->status, pSrc, 0xf800, 11, 0);
		BIT_B2DNTOHS(pDst->control_data_length, pSrc, 0x07ff, 0, 2);
		OCT_B2DMEMCP(pDst->target_entity_id, pSrc);
	}

	if (openavbAecpCommandResponse->headers.subtype == OPENAVB_AECP_AVTP_SUBTYPE) {
		// AECP Common PDU Fields
		openavb_aecp_common_data_unit_t *pDst = &openavbAecpCommandResponse->commonPdu;
		OCT_B2DMEMCP(pDst->controller_entity_id, pSrc);
		OCT_B2DNTOHS(pDst->sequence_id, pSrc);
	}
	else {
		free(openavbAecpCommandResponse);
		return;
	}

	if (openavbAecpCommandResponse->headers.message_type == OPENAVB_AECP_MESSAGE_TYPE_AEM_COMMAND) {
		// Entity Model PDU Fields
		openavb_aecp_entity_model_data_unit_t *pDst = &openavbAecpCommandResponse->entityModelPdu;
		U16 command_specific_length = openavbAecpGetAemCommandSpecificLength(openavbAecpCommandResponse);
		BIT_B2DNTOHS(pDst->u, pSrc, 0x8000, 15, 0);
		BIT_B2DNTOHS(pDst->command_type, pSrc, 0x7fff, 0, 2);

		switch (openavbAecpCommandResponse->entityModelPdu.command_type) {
			case OPENAVB_AEM_COMMAND_CODE_ACQUIRE_ENTITY:
				{
					openavb_aecp_command_data_acquire_entity_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.acquireEntityCmd;
					OCT_B2DNTOHL(pDst->flags, pSrc);
					OCT_B2DMEMCP(pDst->owner_id, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_LOCK_ENTITY:
				{
					openavb_aecp_commandresponse_data_lock_entity_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.lockEntityCmd;
					OCT_B2DNTOHL(pDst->flags, pSrc);
					OCT_B2DMEMCP(pDst->locked_id, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_ENTITY_AVAILABLE:
				// No command specific data
				break;
			case OPENAVB_AEM_COMMAND_CODE_CONTROLLER_AVAILABLE:
				// No command specific data
				break;
			case OPENAVB_AEM_COMMAND_CODE_READ_DESCRIPTOR:
				{
					openavb_aecp_command_data_read_descriptor_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.readDescriptorCmd;
					OCT_B2DNTOHS(pDst->configuration_index, pSrc);
					OCT_B2DNTOHS(pDst->reserved, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_WRITE_DESCRIPTOR:
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_CONFIGURATION:
				{
					openavb_aecp_commandresponse_data_set_configuration_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.setConfigurationCmd;
					OCT_B2DNTOHS(pDst->reserved, pSrc);
					OCT_B2DNTOHS(pDst->configuration_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_CONFIGURATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_DYNAMIC_INFO:
				{
					openavb_aecp_commandresponse_data_get_dynamic_info_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getDynamicInfoCmd;
					pDst->payload_length = command_specific_length;
					if (pDst->payload_length > sizeof(pDst->payload)) {
						pDst->payload_length = sizeof(pDst->payload);
					}
					if (pDst->payload_length > 0) {
						memcpy(pDst->payload, pSrc, pDst->payload_length);
						pSrc += pDst->payload_length;
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_STREAM_FORMAT:
				{
					openavb_aecp_commandresponse_data_set_stream_format_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.setStreamFormatCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
					OCT_B2DMEMCP(pDst->stream_format, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_FORMAT:
				{
					openavb_aecp_command_data_get_stream_format_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getStreamFormatCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
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
					openavb_aecp_commandresponse_data_set_stream_info_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.setStreamInfoCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
					OCT_B2DNTOHL(pDst->flags, pSrc);
					OCT_B2DMEMCP(pDst->stream_format, pSrc);
					OCT_B2DMEMCP(pDst->stream_id, pSrc);
					OCT_B2DNTOHL(pDst->msrp_accumulated_latency, pSrc);
					OCT_B2DMEMCP(pDst->stream_dest_mac, pSrc);
					OCT_B2DNTOHB(pDst->msrp_failure_code, pSrc);
					OCT_B2DNTOHB(pDst->reserved_1, pSrc);
					OCT_B2DMEMCP(pDst->msrp_failure_bridge_id, pSrc);
					OCT_B2DNTOHS(pDst->stream_vlan_id, pSrc);
					OCT_B2DNTOHS(pDst->reserved_2, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_INFO:
				{
					openavb_aecp_command_data_get_stream_info_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getStreamInfoCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_NAME:
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_NAME:
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_ASSOCIATION_ID:
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_ASSOCIATION_ID:
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_SAMPLING_RATE:
				{
					openavb_aecp_commandresponse_data_set_sampling_rate_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.setSamplingRateCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
					OCT_B2DMEMCP(pDst->sampling_rate, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_SAMPLING_RATE:
				{
					openavb_aecp_command_data_get_sampling_rate_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getSamplingRateCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_CLOCK_SOURCE:
				{
					openavb_aecp_commandresponse_data_set_clock_source_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.setClockSourceCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
					OCT_B2DNTOHS(pDst->clock_source_index, pSrc);
					OCT_B2DNTOHS(pDst->reserved, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_CLOCK_SOURCE:
				{
					openavb_aecp_command_data_get_clock_source_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getClockSourceCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_CONTROL:
				{
					U8 *pSrcEnd = pSrc + command_specific_length;
					openavb_aecp_commandresponse_data_set_control_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.setControlCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);

					// Need to get the descriptor to proper parse the values.
					if (pDst->descriptor_type == OPENAVB_AEM_DESCRIPTOR_CONTROL) {
						openavb_aem_descriptor_control_t *pDescriptorControl = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pDst->descriptor_type, pDst->descriptor_index);
						if (pDescriptorControl) {
							bool bDone = FALSE;
							pDst->valuesCount = 0;
							while ((pSrc < pSrcEnd) && !bDone) {
								switch (pDescriptorControl->control_value_type) {
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT8:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8:
										OCT_B2DNTOHB(pDst->values.linear_int8[pDst->valuesCount++], pSrc);
										break;
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT16:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT16:
										OCT_B2DNTOHS(pDst->values.linear_int16[pDst->valuesCount++], pSrc);
										break;
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT32:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT32:
										OCT_B2DNTOHL(pDst->values.linear_int32[pDst->valuesCount++], pSrc);
										break;
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_INT64:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT64:
										OCT_B2DMEMCP(&pDst->values.linear_int64[pDst->valuesCount++], pSrc);
										break;
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_FLOAT:
										OCT_B2DMEMCP(&pDst->values.linear_float[pDst->valuesCount++], pSrc);
										break;
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_DOUBLE:
										OCT_B2DMEMCP(&pDst->values.linear_double[pDst->valuesCount++], pSrc);
										break;
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_INT8:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_UINT8:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_INT16:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_UINT16:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_INT32:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_UINT32:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_INT64:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_UINT64:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_FLOAT:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_DOUBLE:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SELECTOR_STRING:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_INT8:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_UINT8:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_INT16:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_UINT16:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_INT32:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_UINT32:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_INT64:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_UINT64:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_FLOAT:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_ARRAY_DOUBLE:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_UTF8:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_BODE_PLOT:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SMPTE_TIME:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_SAMPLE_RATE:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_GPTP_TIME:
									case OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_VENDOR:
										bDone = TRUE;	// Not yet supported
										break;
								}
							}
						}
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_CONTROL:
				{
					openavb_aecp_command_data_get_control_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getControlCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
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
					openavb_aecp_commandresponse_data_start_streaming_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.startStreamingCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_STOP_STREAMING:
				{
					openavb_aecp_commandresponse_data_stop_streaming_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.stopStreamingCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_REGISTER_UNSOLICITED_NOTIFICATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_DEREGISTER_UNSOLICITED_NOTIFICATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_IDENTIFY_NOTIFICATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_AVB_INFO:
				{
					openavb_aecp_command_data_get_avb_info_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getAvbInfoCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_AS_PATH:
				{
					openavb_aecp_command_data_get_as_path_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getAsPathCmd;
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
					OCT_B2DNTOHS(pDst->reserved, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS:
				{
					openavb_aecp_command_data_get_counters_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getCountersCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME:
				{
					openavb_aecp_command_data_get_max_transit_time_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getMaxTransitTimeCmd;
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_AUDIO_MAP:
				{
					openavb_aecp_command_data_get_audio_map_t *pDst = &openavbAecpCommandResponse->entityModelPdu.command_data.getAudioMapCmd;
					if (command_specific_length != 8) {
						free(openavbAecpCommandResponse);
						return;
					}
					OCT_B2DNTOHS(pDst->descriptor_type, pSrc);
					OCT_B2DNTOHS(pDst->descriptor_index, pSrc);
					OCT_B2DNTOHS(pDst->map_index, pSrc);
					OCT_B2DNTOHS(pDst->reserved, pSrc);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_ADD_AUDIO_MAPPINGS:
				{
					if (!openavbAecpParseAudioMappingsPayload(
							pSrc,
							command_specific_length,
							&openavbAecpCommandResponse->entityModelPdu.command_data.addAudioMappingsCmd)) {
						free(openavbAecpCommandResponse);
						return;
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_REMOVE_AUDIO_MAPPINGS:
				{
					if (!openavbAecpParseAudioMappingsPayload(
							pSrc,
							command_specific_length,
							&openavbAecpCommandResponse->entityModelPdu.command_data.removeAudioMappingsCmd)) {
						free(openavbAecpCommandResponse);
						return;
					}
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
			case OPENAVB_AEM_COMMAND_CODE_GET_MEMORY_OBJECT_LENGTH:
				{
					openavb_aecp_command_data_get_memory_object_length_t *pDst =
						&openavbAecpCommandResponse->entityModelPdu.command_data.getMemoryObjectLengthCmd;
					OCT_B2DNTOHS(pDst->configuration_index, pSrc);
					OCT_B2DNTOHS(pDst->memory_object_index, pSrc);
				}
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

		if (pSrc - payload <= payload_len) {
			// Notify the state machine of the command request
			// The buffer will be deleted once the request is handled.
			openavbAecpSMEntityModelEntitySet_rcvdCommand(openavbAecpCommandResponse);
		}
		else {
			AVB_LOGF_ERROR("Expected packet of size %d, but received one of size %d.  Discarding.", pSrc - payload, payload_len);
			free(openavbAecpCommandResponse);
		}
	}
	else if (openavbAecpCommandResponse->headers.message_type == OPENAVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND) {
		U16 commandSpecificLength;

		if (openavbAecpCommandResponse->headers.control_data_length < 10) {
			free(openavbAecpCommandResponse);
			return;
		}

		commandSpecificLength = (U16)(openavbAecpCommandResponse->headers.control_data_length - 10);
		if ((pSrc - payload + commandSpecificLength) <= payload_len) {
			openavbAecpHandleAddressAccessCommand(openavbAecpCommandResponse, pSrc, commandSpecificLength);
		}
		else {
			AVB_LOGF_ERROR("Expected ADDRESS_ACCESS payload of size %d, but received one of size %d. Discarding.",
				(int)(pSrc - payload + commandSpecificLength), payload_len);
		}
		free(openavbAecpCommandResponse);
	}
	else if (openavbAecpCommandResponse->headers.message_type == OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND) {
		openavb_aecp_entity_model_data_unit_t *pDst = &openavbAecpCommandResponse->entityModelPdu;
		U16 command_specific_length = openavbAecpGetMvuCommandSpecificLength(openavbAecpCommandResponse);

		OCT_B2DMEMCP(openavbAecpCommandResponse->protocol_id, pSrc);
		BIT_B2DNTOHS(pDst->u, pSrc, 0x8000, 15, 0);
		BIT_B2DNTOHS(pDst->command_type, pSrc, 0x7fff, 0, 2);

		switch (pDst->command_type) {
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MILAN_INFO:
				{
					openavb_aecp_mvu_commandresponse_data_get_milan_info_t *pCmd = &pDst->command_data.getMilanInfoCmd;
					OCT_B2DNTOHS(pCmd->reserved_0, pSrc);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_SET_SYSTEM_UNIQUE_ID:
				{
					openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t *pCmd =
						&pDst->command_data.setSystemUniqueIdCmd;
					U32 systemUniqueIdHi = 0;
					U32 systemUniqueIdLo = 0;

					OCT_B2DNTOHS(pCmd->reserved_0, pSrc);
					if (command_specific_length == 6) {
						OCT_B2DNTOHL(systemUniqueIdLo, pSrc);
						pCmd->system_unique_id = (U64)systemUniqueIdLo;
						memset(pCmd->system_name, 0, sizeof(pCmd->system_name));
					}
					else if (command_specific_length >= 74) {
						OCT_B2DNTOHL(systemUniqueIdHi, pSrc);
						OCT_B2DNTOHL(systemUniqueIdLo, pSrc);
						pCmd->system_unique_id = (((U64)systemUniqueIdHi) << 32) | (U64)systemUniqueIdLo;
						OCT_B2DMEMCP(pCmd->system_name, pSrc);
					}
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_SYSTEM_UNIQUE_ID:
				{
					openavb_aecp_mvu_commandresponse_data_get_system_unique_id_t *pCmd =
						&pDst->command_data.getSystemUniqueIdCmd;
					OCT_B2DNTOHS(pCmd->reserved_0, pSrc);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MEDIA_CLOCK_REFERENCE_INFO:
				{
					openavb_aecp_mvu_commandresponse_data_get_media_clock_reference_info_t *pCmd =
						&pDst->command_data.getMediaClockReferenceInfoCmd;
					OCT_B2DNTOHS(pCmd->clock_domain_index, pSrc);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_BIND_STREAM:
				{
					openavb_aecp_mvu_commandresponse_data_bind_stream_t *pCmd = &pDst->command_data.bindStreamCmd;
					OCT_B2DNTOHS(pCmd->flags, pSrc);
					OCT_B2DNTOHS(pCmd->descriptor_type, pSrc);
					OCT_B2DNTOHS(pCmd->descriptor_index, pSrc);
					OCT_B2DMEMCP(pCmd->talker_entity_id, pSrc);
					OCT_B2DNTOHS(pCmd->talker_stream_index, pSrc);
					OCT_B2DNTOHS(pCmd->reserved, pSrc);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_UNBIND_STREAM:
				{
					openavb_aecp_mvu_commandresponse_data_unbind_stream_t *pCmd = &pDst->command_data.unbindStreamCmd;
					OCT_B2DNTOHS(pCmd->reserved_0, pSrc);
					OCT_B2DNTOHS(pCmd->descriptor_type, pSrc);
					OCT_B2DNTOHS(pCmd->descriptor_index, pSrc);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_STREAM_INPUT_INFO_EX:
				{
					openavb_aecp_mvu_commandresponse_data_unbind_stream_t *pCmd = &pDst->command_data.getStreamInputInfoExCmd;
					OCT_B2DNTOHS(pCmd->reserved_0, pSrc);
					OCT_B2DNTOHS(pCmd->descriptor_type, pSrc);
					OCT_B2DNTOHS(pCmd->descriptor_index, pSrc);
				}
				break;
			default:
				if (command_specific_length > 0) {
					pSrc += command_specific_length;
				}
				break;
		}

		if (pSrc - payload <= payload_len) {
			openavbAecpSMEntityModelEntitySet_rcvdCommand(openavbAecpCommandResponse);
		}
		else {
			AVB_LOGF_ERROR("Expected packet of size %d, but received one of size %d.  Discarding.", pSrc - payload, payload_len);
			free(openavbAecpCommandResponse);
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

static void openavbAecpMessageRxFrameReceive(U32 timeoutUsec)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	hdr_info_t hdrInfo;
	unsigned int offset, len;
	U8 *pBuf, *pFrame;

	memset(&hdrInfo, 0, sizeof(hdr_info_t));

	pBuf = (U8 *)openavbRawsockGetRxFrame(rxSock, timeoutUsec, &offset, &len);
	if (pBuf) {
		pFrame = pBuf + offset;

		offset = openavbRawsockRxParseHdr(rxSock, pBuf, &hdrInfo);
		{
#ifndef UBUNTU
			if (hdrInfo.ethertype == ETHERTYPE_8021Q) {
				// Oh!  Need to look past the VLAN tag
				U16 vlan_bits = ntohs(*(U16 *)(pFrame + offset));
				hdrInfo.vlan = TRUE;
				hdrInfo.vlan_vid = vlan_bits & 0x0FFF;
				hdrInfo.vlan_pcp = (vlan_bits >> 13) & 0x0007;
				offset += 2;
				hdrInfo.ethertype = ntohs(*(U16 *)(pFrame + offset));
				offset += 2;
			}
#endif

			// Make sure that this is an AVTP packet
			// (Should always be AVTP if it's to our AVTP-specific multicast address)
			if (hdrInfo.ethertype == ETHERTYPE_AVTP) {
				// parse the PDU only for AECP messages
				if (*(pFrame + offset) == (0x80 | OPENAVB_AECP_AVTP_SUBTYPE)) {
					openavbAecpMessageRxFrameParse(pFrame + offset, len - offset, &hdrInfo);
				}
			}
				else {
					if (openavbAecpShouldLogNonAvtp()) {
						AVB_LOG_DEBUG("Received non-AVTP frame.");
						AVB_LOGF_DEBUG("Unexpected packet data (length %d):", len);
						AVB_LOG_BUFFER(AVB_LOG_LEVEL_DEBUG, pFrame, len, 16);
					}
				}
		}

		// Release the frame
		openavbRawsockRelRxFrame(rxSock, pBuf);
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}


void openavbAecpMessageTxFrame(openavb_aecp_AEMCommandResponse_t *AEMCommandResponse)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	U8 *pBuf;
	U8 *pcontrol_data_length;
	U8 *pcontrol_data_length_start_marker;
	U32 size;
	unsigned int hdrlen = 0;

	pBuf = openavbRawsockGetTxFrame(txSock, TRUE, &size);

	if (!pBuf) {
		AVB_LOG_ERROR("No TX buffer");
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return;
	}

	if (size < AECP_FRAME_LEN) {
		AVB_LOG_ERROR("TX buffer too small");
		openavbRawsockRelTxFrame(txSock, pBuf);
		pBuf = NULL;
		AVB_TRACE_EXIT(AVB_TRACE_AECP);
		return;
	}

	memset(pBuf, 0, AECP_FRAME_LEN);
	openavbRawsockTxFillHdr(txSock, pBuf, &hdrlen);

	// Set the destination address
	memcpy(pBuf, AEMCommandResponse->host, ETH_ALEN);

	U8 *pDst = pBuf + hdrlen;
	{
		// AVTP Control Header
		openavb_aecp_control_header_t *pSrc = &AEMCommandResponse->headers;
		BIT_D2BHTONB(pDst, pSrc->cd, 7, 0);
		BIT_D2BHTONB(pDst, pSrc->subtype, 0, 1);
		BIT_D2BHTONB(pDst, pSrc->sv, 7, 0);
		BIT_D2BHTONB(pDst, pSrc->version, 4, 0);
		BIT_D2BHTONB(pDst, pSrc->message_type, 0, 1);
		BIT_D2BHTONS(pDst, pSrc->status, 11, 0);
		pcontrol_data_length = pDst;							// Set later
		BIT_D2BHTONS(pDst, pSrc->control_data_length, 0, 2);
		OCT_D2BMEMCP(pDst, pSrc->target_entity_id);
		pcontrol_data_length_start_marker = pDst;
	}

	{
		// AECP Common PDU
		openavb_aecp_common_data_unit_t *pSrc = &AEMCommandResponse->commonPdu;
		OCT_D2BMEMCP(pDst, pSrc->controller_entity_id);
		OCT_D2BHTONS(pDst, pSrc->sequence_id);
	}

	if (AEMCommandResponse->headers.message_type == OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE) {
		if (AEMCommandResponse->entityModelPdu.command_type == OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME ||
				AEMCommandResponse->entityModelPdu.command_type == OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME_2021) {
			openavb_aecp_response_data_get_max_transit_time_t *pRsp = &AEMCommandResponse->entityModelPdu.command_data.getMaxTransitTimeRsp;
			AEMCommandResponse->headers.status = OPENAVB_AEM_COMMAND_STATUS_SUCCESS;
			if (pRsp->descriptor_type == 0 && pRsp->descriptor_index == 0 && pRsp->max_transit_time == 0) {
				openavb_aecp_command_data_get_max_transit_time_t *pCmd = &AEMCommandResponse->entityModelPdu.command_data.getMaxTransitTimeCmd;
				pRsp->descriptor_type = pCmd->descriptor_type;
				pRsp->descriptor_index = pCmd->descriptor_index;
				pRsp->max_transit_time = 0;
			}
		}

		// Entity Model PDU Fields
		openavb_aecp_entity_model_data_unit_t *pSrc = &AEMCommandResponse->entityModelPdu;
		BIT_D2BHTONS(pDst, pSrc->u, 15, 0);
		BIT_D2BHTONS(pDst, pSrc->command_type, 0, 2);

		// Command specific data
		switch (AEMCommandResponse->entityModelPdu.command_type) {
			case OPENAVB_AEM_COMMAND_CODE_ACQUIRE_ENTITY:
				{
					openavb_aecp_command_data_acquire_entity_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.acquireEntityRsp;
					OCT_D2BHTONL(pDst, pSrc->flags);
					OCT_D2BMEMCP(pDst, pSrc->owner_id);
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_LOCK_ENTITY:
				{
					openavb_aecp_commandresponse_data_lock_entity_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.lockEntityRsp;
					OCT_D2BHTONL(pDst, pSrc->flags);
					OCT_D2BMEMCP(pDst, pSrc->locked_id);
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_ENTITY_AVAILABLE:
				// No command specific data
				break;
			case OPENAVB_AEM_COMMAND_CODE_CONTROLLER_AVAILABLE:
				// No command specific data
				break;
			case OPENAVB_AEM_COMMAND_CODE_READ_DESCRIPTOR:
				{
					openavb_aecp_response_data_read_descriptor_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.readDescriptorRsp;
					OCT_D2BHTONS(pDst, pSrc->configuration_index);
					OCT_D2BHTONS(pDst, pSrc->reserved);
					OCT_D2BBUFCP(pDst, pSrc->descriptor_data, pSrc->descriptor_length);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_WRITE_DESCRIPTOR:
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_CONFIGURATION:
				{
					openavb_aecp_commandresponse_data_set_configuration_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.setConfigurationRsp;
					OCT_D2BHTONS(pDst, pSrc->reserved);
					OCT_D2BHTONS(pDst, pSrc->configuration_index);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_CONFIGURATION:
				{
					openavb_aecp_response_data_get_configuration_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getConfigurationRsp;
					OCT_D2BHTONS(pDst, pSrc->reserved);
					OCT_D2BHTONS(pDst, pSrc->configuration_index);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_DYNAMIC_INFO:
				{
					openavb_aecp_commandresponse_data_get_dynamic_info_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getDynamicInfoRsp;
					if (pSrc->payload_length > sizeof(pSrc->payload)) {
						pSrc->payload_length = sizeof(pSrc->payload);
					}
					if (pSrc->payload_length > 0) {
						OCT_D2BBUFCP(pDst, pSrc->payload, pSrc->payload_length);
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_STREAM_FORMAT:
				{
					openavb_aecp_commandresponse_data_set_stream_format_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.setStreamFormatRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BMEMCP(pDst, pSrc->stream_format);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_FORMAT:
				{
					openavb_aecp_response_data_get_stream_format_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getStreamFormatRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BMEMCP(pDst, pSrc->stream_format);
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
					openavb_aecp_commandresponse_data_set_stream_info_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.setStreamInfoRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONL(pDst, pSrc->flags);
					OCT_D2BMEMCP(pDst, pSrc->stream_format);
					OCT_D2BMEMCP(pDst, pSrc->stream_id);
					OCT_D2BHTONL(pDst, pSrc->msrp_accumulated_latency);
					OCT_D2BMEMCP(pDst, pSrc->stream_dest_mac);
					OCT_D2BHTONB(pDst, pSrc->msrp_failure_code);
					OCT_D2BHTONB(pDst, pSrc->reserved_1);
					OCT_D2BMEMCP(pDst, pSrc->msrp_failure_bridge_id);
					OCT_D2BHTONS(pDst, pSrc->stream_vlan_id);
					OCT_D2BHTONS(pDst, pSrc->reserved_2);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_STREAM_INFO:
				{
					openavb_aecp_response_data_get_stream_info_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getStreamInfoRsp;
					bool emitMilanExtension = TRUE;
					U32 streamInfoFlagsEx = pSrc->flags_ex;
					U8 probingAcmpStatus = 0;
					U8 reserved3 = 0;
					U16 reserved4 = 0;

					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONL(pDst, pSrc->flags);
					OCT_D2BMEMCP(pDst, pSrc->stream_format);
					OCT_D2BMEMCP(pDst, pSrc->stream_id);
					OCT_D2BHTONL(pDst, pSrc->msrp_accumulated_latency);
					OCT_D2BMEMCP(pDst, pSrc->stream_dest_mac);
					OCT_D2BHTONB(pDst, pSrc->msrp_failure_code);
					OCT_D2BHTONB(pDst, pSrc->reserved_1);
					OCT_D2BMEMCP(pDst, pSrc->msrp_failure_bridge_id);
					OCT_D2BHTONS(pDst, pSrc->stream_vlan_id);
					OCT_D2BHTONS(pDst, pSrc->reserved_2);

					if (emitMilanExtension) {
						// Probing status "Completed" (0x03) in upper 3 bits when stream is connected.
						if (pSrc->flags & OPENAVB_AEM_SET_STREAM_INFO_COMMAND_FLAG_CONNECTED) {
							probingAcmpStatus = (U8)(0x03 << 5);
						}
						// ACMP status "Success" is encoded as 0 in lower 5 bits.
						OCT_D2BHTONL(pDst, streamInfoFlagsEx);
						OCT_D2BHTONB(pDst, probingAcmpStatus);
						OCT_D2BHTONB(pDst, reserved3);
						OCT_D2BHTONS(pDst, reserved4);
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
					openavb_aecp_response_data_get_association_id_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getAssociationIDRsp;
					OCT_D2BMEMCP(pDst, pSrc->association_id);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_SAMPLING_RATE:
				{
					openavb_aecp_commandresponse_data_set_sampling_rate_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.setSamplingRateRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BMEMCP(pDst, pSrc->sampling_rate);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_SAMPLING_RATE:
				{
					openavb_aecp_response_data_get_sampling_rate_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getSamplingRateRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BMEMCP(pDst, pSrc->sampling_rate);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_CLOCK_SOURCE:
				{
					openavb_aecp_commandresponse_data_set_clock_source_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.setClockSourceRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONS(pDst, pSrc->clock_source_index);
					OCT_D2BHTONS(pDst, pSrc->reserved);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_CLOCK_SOURCE:
				{
					openavb_aecp_response_data_get_clock_source_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getClockSourceRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONS(pDst, pSrc->clock_source_index);
					OCT_D2BHTONS(pDst, pSrc->reserved);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_SET_CONTROL:
				{
					openavb_aecp_commandresponse_data_set_control_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.setControlRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);

					openavb_aem_descriptor_control_t *pDescriptorControl = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pSrc->descriptor_type, pSrc->descriptor_index);
					if (pDescriptorControl) {
						openavbAecpSerializeSetControlResponseValues(&pDst, pSrc, pDescriptorControl);
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_CONTROL:
				{
					openavb_aecp_response_data_get_control_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getControlRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);

					openavb_aem_descriptor_control_t *pDescriptorControl = openavbAemGetDescriptor(openavbAemGetConfigIdx(), pSrc->descriptor_type, pSrc->descriptor_index);
					if (pDescriptorControl) {
						openavbAecpSerializeGetControlResponseValues(&pDst, pSrc, pDescriptorControl);
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
					openavb_aecp_commandresponse_data_start_streaming_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.startStreamingRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_STOP_STREAMING:
				{
					openavb_aecp_commandresponse_data_stop_streaming_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.stopStreamingRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_REGISTER_UNSOLICITED_NOTIFICATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_DEREGISTER_UNSOLICITED_NOTIFICATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_IDENTIFY_NOTIFICATION:
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_AVB_INFO:
				{
					openavb_aecp_response_data_get_avb_info_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getAvbInfoRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BBUFCP(pDst, pSrc->as_grandmaster_id, sizeof(pSrc->as_grandmaster_id));
					OCT_D2BHTONL(pDst, pSrc->propagation_delay);
					OCT_D2BHTONB(pDst, pSrc->as_domain_number);
					OCT_D2BHTONB(pDst, pSrc->flags);
					OCT_D2BHTONS(pDst, pSrc->msrp_mappings_count);
					if (pSrc->msrp_mappings_count > 0) {
						OCT_D2BBUFCP(pDst, pSrc->msrp_mappings, pSrc->msrp_mappings_count * 8);
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_AS_PATH:
				{
					openavb_aecp_response_data_get_as_path_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getAsPathRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONS(pDst, pSrc->as_path_count);
					OCT_D2BHTONL(pDst, pSrc->path_latency);
					if (pSrc->as_path_count > 0) {
						OCT_D2BBUFCP(pDst, pSrc->as_path, pSrc->as_path_count * 8);
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_COUNTERS:
				{
					openavb_aecp_response_data_get_counters_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getCountersRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONL(pDst, pSrc->counters_valid);
					OCT_D2BBUFCP(pDst, pSrc->counters_block, pSrc->counters_block_length);
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME:
				{
					openavb_aecp_response_data_get_max_transit_time_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getMaxTransitTimeRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					if (AEMCommandResponse->entityModelPdu.command_type == OPENAVB_AEM_COMMAND_CODE_GET_MAX_TRANSIT_TIME_2021) {
						U64 max_transit_time = pSrc->max_transit_time;
						OCT_D2BHTONL(pDst, (U32)(max_transit_time >> 32));
						OCT_D2BHTONL(pDst, (U32)(max_transit_time & 0xffffffffu));
					}
					else {
						OCT_D2BHTONL(pDst, (U32)(pSrc->max_transit_time & 0xffffffffu));
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_GET_AUDIO_MAP:
				{
					openavb_aecp_response_data_get_audio_map_t *pSrc = &AEMCommandResponse->entityModelPdu.command_data.getAudioMapRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONS(pDst, pSrc->map_index);
					OCT_D2BHTONS(pDst, pSrc->number_of_maps);
					OCT_D2BHTONS(pDst, pSrc->number_of_mappings);
					OCT_D2BHTONS(pDst, pSrc->reserved);
					if (pSrc->number_of_mappings > 0) {
						openavbAecpSerializeAudioMappings(&pDst, pSrc->mappings, pSrc->number_of_mappings);
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_ADD_AUDIO_MAPPINGS:
				{
					openavb_aecp_commandresponse_data_audio_mappings_t *pSrc =
						&AEMCommandResponse->entityModelPdu.command_data.addAudioMappingsRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONS(pDst, pSrc->number_of_mappings);
					OCT_D2BHTONS(pDst, pSrc->reserved);
					if (pSrc->number_of_mappings > 0) {
						openavbAecpSerializeAudioMappings(&pDst, pSrc->mappings, pSrc->number_of_mappings);
					}
				}
				break;
			case OPENAVB_AEM_COMMAND_CODE_REMOVE_AUDIO_MAPPINGS:
				{
					openavb_aecp_commandresponse_data_audio_mappings_t *pSrc =
						&AEMCommandResponse->entityModelPdu.command_data.removeAudioMappingsRsp;
					OCT_D2BHTONS(pDst, pSrc->descriptor_type);
					OCT_D2BHTONS(pDst, pSrc->descriptor_index);
					OCT_D2BHTONS(pDst, pSrc->number_of_mappings);
					OCT_D2BHTONS(pDst, pSrc->reserved);
					if (pSrc->number_of_mappings > 0) {
						openavbAecpSerializeAudioMappings(&pDst, pSrc->mappings, pSrc->number_of_mappings);
					}
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
			case OPENAVB_AEM_COMMAND_CODE_GET_MEMORY_OBJECT_LENGTH:
				{
					openavb_aecp_response_data_get_memory_object_length_t *pSrc =
						&AEMCommandResponse->entityModelPdu.command_data.getMemoryObjectLengthRsp;
					U64 lengthNetworkOrder = htonll(pSrc->length);
					OCT_D2BHTONS(pDst, pSrc->configuration_index);
					OCT_D2BHTONS(pDst, pSrc->memory_object_index);
					OCT_D2BMEMCP(pDst, &lengthNetworkOrder);
				}
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
	}
	else if (AEMCommandResponse->headers.message_type == OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE) {
		openavb_aecp_entity_model_data_unit_t *pSrc = &AEMCommandResponse->entityModelPdu;
		OCT_D2BBUFCP(pDst, AEMCommandResponse->protocol_id, OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH);
		BIT_D2BHTONS(pDst, pSrc->u, 15, 0);
		BIT_D2BHTONS(pDst, pSrc->command_type, 0, 2);

		switch (pSrc->command_type) {
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MILAN_INFO:
				{
					openavb_aecp_mvu_response_data_get_milan_info_t *pMvu = &pSrc->command_data.getMilanInfoRsp;
					OCT_D2BHTONS(pDst, pMvu->reserved_0);
					OCT_D2BHTONL(pDst, pMvu->protocol_version);
					OCT_D2BHTONL(pDst, pMvu->features_flags);
					OCT_D2BHTONL(pDst, pMvu->certification_version);
					OCT_D2BHTONL(pDst, pMvu->specification_version);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_SET_SYSTEM_UNIQUE_ID:
				{
					openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t *pMvu =
						&pSrc->command_data.setSystemUniqueIdRsp;
					OCT_D2BHTONS(pDst, pMvu->reserved_0);
					OCT_D2BHTONL(pDst, (U32)(pMvu->system_unique_id >> 32));
					OCT_D2BHTONL(pDst, (U32)(pMvu->system_unique_id & 0xffffffffULL));
					OCT_D2BBUFCP(pDst, pMvu->system_name, OPENAVB_AEM_STRLEN_MAX);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_SYSTEM_UNIQUE_ID:
				{
					openavb_aecp_mvu_response_data_get_system_unique_id_t *pMvu =
						&pSrc->command_data.getSystemUniqueIdRsp;
					OCT_D2BHTONS(pDst, pMvu->reserved_0);
					OCT_D2BHTONL(pDst, (U32)(pMvu->system_unique_id >> 32));
					OCT_D2BHTONL(pDst, (U32)(pMvu->system_unique_id & 0xffffffffULL));
					OCT_D2BBUFCP(pDst, pMvu->system_name, OPENAVB_AEM_STRLEN_MAX);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MEDIA_CLOCK_REFERENCE_INFO:
				{
					openavb_aecp_mvu_response_data_get_media_clock_reference_info_t *pMvu =
						&pSrc->command_data.getMediaClockReferenceInfoRsp;
					OCT_D2BHTONS(pDst, pMvu->clock_domain_index);
					OCT_D2BHTONB(pDst, pMvu->flags);
					OCT_D2BHTONB(pDst, pMvu->reserved_0);
					OCT_D2BHTONB(pDst, pMvu->default_media_clock_priority);
					OCT_D2BHTONB(pDst, pMvu->user_media_clock_priority);
					OCT_D2BHTONL(pDst, pMvu->reserved_1);
					OCT_D2BBUFCP(pDst, pMvu->media_clock_domain_name, OPENAVB_AEM_STRLEN_MAX);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_BIND_STREAM:
				{
					openavb_aecp_mvu_commandresponse_data_bind_stream_t *pMvu = &pSrc->command_data.bindStreamRsp;
					OCT_D2BHTONS(pDst, pMvu->flags);
					OCT_D2BHTONS(pDst, pMvu->descriptor_type);
					OCT_D2BHTONS(pDst, pMvu->descriptor_index);
					OCT_D2BMEMCP(pDst, pMvu->talker_entity_id);
					OCT_D2BHTONS(pDst, pMvu->talker_stream_index);
					OCT_D2BHTONS(pDst, pMvu->reserved);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_UNBIND_STREAM:
				{
					openavb_aecp_mvu_commandresponse_data_unbind_stream_t *pMvu = &pSrc->command_data.unbindStreamRsp;
					OCT_D2BHTONS(pDst, pMvu->reserved_0);
					OCT_D2BHTONS(pDst, pMvu->descriptor_type);
					OCT_D2BHTONS(pDst, pMvu->descriptor_index);
				}
				break;
			case OPENAVB_AECP_MVU_COMMAND_TYPE_GET_STREAM_INPUT_INFO_EX:
				{
					openavb_aecp_mvu_response_data_get_stream_input_info_ex_t *pMvu = &pSrc->command_data.getStreamInputInfoExRsp;
					OCT_D2BHTONS(pDst, pMvu->reserved_0);
					OCT_D2BHTONS(pDst, pMvu->descriptor_type);
					OCT_D2BHTONS(pDst, pMvu->descriptor_index);
					OCT_D2BMEMCP(pDst, pMvu->talker_entity_id);
					OCT_D2BHTONS(pDst, pMvu->talker_unique_id);
					OCT_D2BHTONB(pDst, pMvu->probing_acmp_status);
					OCT_D2BHTONB(pDst, pMvu->reserved_1);
				}
				break;
			default:
				break;
		}
	}

	// Set length into buffer
	BIT_D2BHTONS(pcontrol_data_length, (pDst - pcontrol_data_length_start_marker), 0, 2);

	// Make sure the packet will be at least 64 bytes long.
	if (pDst - pBuf < 64) { pDst = pBuf + 64; }

#if 0
	AVB_LOGF_DEBUG("openavbAecpMessageTxFrame packet data (length %d):", pDst - pBuf);
	AVB_LOG_BUFFER(AVB_LOG_LEVEL_DEBUG, pBuf, pDst - pBuf, 16);
#endif

	openavbRawsockTxFrameReady(txSock, pBuf, pDst - pBuf, 0);
	openavbRawsockSend(txSock);

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

void* openavbAecpMessageRxThreadFn(void *pv)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	AVB_LOG_DEBUG("AECP Thread Started");
	while (bRunning) {
		// Try to get and process an AECP message.
		openavbAecpMessageRxFrameReceive(MICROSECONDS_PER_SECOND);
	}
	AVB_LOG_DEBUG("AECP Thread Done");

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
	return NULL;
}

openavbRC openavbAecpMessageHandlerStart()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	bRunning = TRUE;

	if (openavbAecpOpenSocket((const char *)gAvdeccCfg.ifname, gAvdeccCfg.vlanID, gAvdeccCfg.vlanPCP)) {

		// Start the RX thread
		bool errResult;
		THREAD_CREATE(openavbAecpMessageRxThread, openavbAecpMessageRxThread, NULL, openavbAecpMessageRxThreadFn, NULL);
		THREAD_CHECK_ERROR(openavbAecpMessageRxThread, "Thread / task creation failed", errResult);
		if (errResult) {
			bRunning = FALSE;
			openavbAecpCloseSocket();
			AVB_RC_TRACE_RET(OPENAVB_AVDECC_FAILURE, AVB_TRACE_AECP);
		}

		AVB_RC_TRACE_RET(OPENAVB_AVDECC_SUCCESS, AVB_TRACE_AECP);
	}

	bRunning = FALSE;
	AVB_RC_TRACE_RET(OPENAVB_AVDECC_FAILURE, AVB_TRACE_AECP);
}

void openavbAecpMessageHandlerStop()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);

	if (bRunning) {
		bRunning = FALSE;
		THREAD_JOIN(openavbAecpMessageRxThread, NULL);
		openavbAecpCloseSocket();
	}

	AVB_TRACE_EXIT(AVB_TRACE_AECP);
}

openavbRC openavbAecpMessageSendUnsolicitedNotificationIfNeeded(openavb_aecp_AEMCommandResponse_t *AEMCommandResponse)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	// AVDECC_TODO : Currently not implemented. IEEE Std 1722.1-2013 clause 7.5
	// The basic idea is to inform interested controllers about a change in state of the Entity Model. This change in state
	// could be from a AEM Command or other interfaces into the Entity Model. For AEM Commands this function will check the CommandResponse
	// for the Success and correct Message Type and set the Unsolicited flag and sent to interested controllers.
	AVB_RC_TRACE_RET(OPENAVB_AVDECC_SUCCESS, AVB_TRACE_AECP);
}

openavbRC openavbAecpMessageSend(openavb_aecp_AEMCommandResponse_t *AEMCommandResponse)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AECP);
	openavbAecpMessageTxFrame(AEMCommandResponse);
	openavbAecpMessageSendUnsolicitedNotificationIfNeeded(AEMCommandResponse);
	AVB_RC_TRACE_RET(OPENAVB_AVDECC_SUCCESS, AVB_TRACE_AECP);
}

