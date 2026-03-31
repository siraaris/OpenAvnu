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
 * MODULE : AEM - AVDECC Memory Object Descriptor Public Interface
 * MODULE SUMMARY : Public Interface for the Memory Object Descriptor
 ******************************************************************
 */

#ifndef OPENAVB_DESCRIPTOR_MEMORY_OBJECT_PUB_H
#define OPENAVB_DESCRIPTOR_MEMORY_OBJECT_PUB_H 1

#include "openavb_types_pub.h"
#include "openavb_aem_types_pub.h"
#include "openavb_aem_pub.h"

typedef struct {
	openavb_descriptor_pvt_ptr_t descriptorPvtPtr;

	U16 descriptor_type;
	U16 descriptor_index;
	U8 object_name[OPENAVB_AEM_STRLEN_MAX];
	openavb_aem_string_ref_t localized_description;
	U16 memory_object_type;
	U16 target_descriptor_type;
	U16 target_descriptor_index;
	U64 start_address;
	U64 maximum_length;
	U64 length;
} openavb_aem_descriptor_memory_object_t;

openavb_aem_descriptor_memory_object_t *openavbAemDescriptorMemoryObjectNew(void);
bool openavbAemDescriptorMemoryObjectInitialize(
	openavb_aem_descriptor_memory_object_t *pDescriptor,
	U16 memory_object_type,
	U16 target_descriptor_type,
	U16 target_descriptor_index,
	U64 start_address,
	U64 maximum_length,
	U64 length,
	const char *object_name);

#endif // OPENAVB_DESCRIPTOR_MEMORY_OBJECT_PUB_H
