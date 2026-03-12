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

#include <stdlib.h>

#define AVB_LOG_COMPONENT "AEM"
#include "openavb_log.h"

#include "openavb_rawsock.h"
#include "openavb_aem.h"
#include "openavb_descriptor_memory_object.h"

static openavbRC openavbAemDescriptorMemoryObjectToBuf(void *pVoidDescriptor, U16 bufLength, U8 *pBuf, U16 *descriptorSize)
{
	openavb_aem_descriptor_memory_object_t *pDescriptor = pVoidDescriptor;
	U8 *pDst = pBuf;
	U64 value64;

	if (!pDescriptor || !pBuf || !descriptorSize) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVDECC_FAILURE | OPENAVB_RC_INVALID_ARGUMENT), AVB_TRACE_AEM);
	}
	if (bufLength < OPENAVB_DESCRIPTOR_MEMORY_OBJECT_BASE_LENGTH) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVDECC_FAILURE | OPENAVBAVDECC_RC_BUFFER_TOO_SMALL), AVB_TRACE_AEM);
	}

	*descriptorSize = 0;

	OCT_D2BHTONS(pDst, pDescriptor->descriptor_type);
	OCT_D2BHTONS(pDst, pDescriptor->descriptor_index);
	OCT_D2BMEMCP(pDst, pDescriptor->object_name);
	BIT_D2BHTONS(pDst, pDescriptor->localized_description.offset, 3, 0);
	BIT_D2BHTONS(pDst, pDescriptor->localized_description.index, 0, 2);
	OCT_D2BHTONS(pDst, pDescriptor->memory_object_type);
	OCT_D2BHTONS(pDst, pDescriptor->target_descriptor_type);
	OCT_D2BHTONS(pDst, pDescriptor->target_descriptor_index);

	value64 = htonll(pDescriptor->start_address);
	OCT_D2BMEMCP(pDst, &value64);
	value64 = htonll(pDescriptor->maximum_length);
	OCT_D2BMEMCP(pDst, &value64);
	value64 = htonll(pDescriptor->length);
	OCT_D2BMEMCP(pDst, &value64);

	*descriptorSize = (U16)(pDst - pBuf);
	AVB_RC_TRACE_RET(OPENAVB_AVDECC_SUCCESS, AVB_TRACE_AEM);
}

static openavbRC openavbAemDescriptorMemoryObjectFromBuf(void *pVoidDescriptor, U16 bufLength, U8 *pBuf)
{
	openavb_aem_descriptor_memory_object_t *pDescriptor = pVoidDescriptor;
	U8 *pSrc = pBuf;
	U64 value64;

	if (!pDescriptor || !pBuf) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVDECC_FAILURE | OPENAVB_RC_INVALID_ARGUMENT), AVB_TRACE_AEM);
	}
	if (bufLength < OPENAVB_DESCRIPTOR_MEMORY_OBJECT_BASE_LENGTH) {
		AVB_RC_LOG_TRACE_RET(AVB_RC(OPENAVB_AVDECC_FAILURE | OPENAVBAVDECC_RC_BUFFER_TOO_SMALL), AVB_TRACE_AEM);
	}

	OCT_B2DNTOHS(pDescriptor->descriptor_type, pSrc);
	OCT_B2DNTOHS(pDescriptor->descriptor_index, pSrc);
	OCT_B2DMEMCP(pDescriptor->object_name, pSrc);
	BIT_B2DNTOHS(pDescriptor->localized_description.offset, pSrc, 0xfff8, 3, 0);
	BIT_B2DNTOHS(pDescriptor->localized_description.index, pSrc, 0x0007, 0, 2);
	OCT_B2DNTOHS(pDescriptor->memory_object_type, pSrc);
	OCT_B2DNTOHS(pDescriptor->target_descriptor_type, pSrc);
	OCT_B2DNTOHS(pDescriptor->target_descriptor_index, pSrc);
	OCT_B2DMEMCP(&value64, pSrc);
	pDescriptor->start_address = ntohll(value64);
	OCT_B2DMEMCP(&value64, pSrc);
	pDescriptor->maximum_length = ntohll(value64);
	OCT_B2DMEMCP(&value64, pSrc);
	pDescriptor->length = ntohll(value64);

	AVB_RC_TRACE_RET(OPENAVB_AVDECC_SUCCESS, AVB_TRACE_AEM);
}

static openavbRC openavbAemDescriptorMemoryObjectUpdate(void *pVoidDescriptor)
{
	(void)pVoidDescriptor;
	AVB_RC_TRACE_RET(OPENAVB_AVDECC_SUCCESS, AVB_TRACE_AEM);
}

extern DLL_EXPORT openavb_aem_descriptor_memory_object_t *openavbAemDescriptorMemoryObjectNew(void)
{
	openavb_aem_descriptor_memory_object_t *pDescriptor = malloc(sizeof(*pDescriptor));

	if (!pDescriptor) {
		AVB_RC_LOG(AVB_RC(OPENAVB_AVDECC_FAILURE | OPENAVB_RC_OUT_OF_MEMORY));
		return NULL;
	}
	memset(pDescriptor, 0, sizeof(*pDescriptor));

	pDescriptor->descriptorPvtPtr = malloc(sizeof(*pDescriptor->descriptorPvtPtr));
	if (!pDescriptor->descriptorPvtPtr) {
		free(pDescriptor);
		AVB_RC_LOG(AVB_RC(OPENAVB_AVDECC_FAILURE | OPENAVB_RC_OUT_OF_MEMORY));
		return NULL;
	}

	pDescriptor->descriptorPvtPtr->size = sizeof(*pDescriptor);
	pDescriptor->descriptorPvtPtr->bTopLevel = TRUE;
	pDescriptor->descriptorPvtPtr->toBuf = openavbAemDescriptorMemoryObjectToBuf;
	pDescriptor->descriptorPvtPtr->fromBuf = openavbAemDescriptorMemoryObjectFromBuf;
	pDescriptor->descriptorPvtPtr->update = openavbAemDescriptorMemoryObjectUpdate;

	pDescriptor->descriptor_type = OPENAVB_AEM_DESCRIPTOR_MEMORY_OBJECT;
	pDescriptor->localized_description.offset = OPENAVB_AEM_NO_STRING_OFFSET;
	pDescriptor->localized_description.index = OPENAVB_AEM_NO_STRING_INDEX;

	return pDescriptor;
}

extern DLL_EXPORT bool openavbAemDescriptorMemoryObjectInitialize(
	openavb_aem_descriptor_memory_object_t *pDescriptor,
	U16 memory_object_type,
	U16 target_descriptor_type,
	U16 target_descriptor_index,
	U64 start_address,
	U64 maximum_length,
	U64 length,
	const char *object_name)
{
	if (!pDescriptor) {
		return FALSE;
	}

	pDescriptor->memory_object_type = memory_object_type;
	pDescriptor->target_descriptor_type = target_descriptor_type;
	pDescriptor->target_descriptor_index = target_descriptor_index;
	pDescriptor->start_address = start_address;
	pDescriptor->maximum_length = maximum_length;
	pDescriptor->length = length;
	if (object_name) {
		openavbAemSetString(pDescriptor->object_name, object_name);
	}
	return TRUE;
}
