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
* MODULE SUMMARY : NULL interface module.
* 
* This NULL interface module neither sends or receives data but will
* exercise the various functions and callback and can be used as an example
* or a template for new interfaces.
*/

#include <stdlib.h>
#include <string.h>
#include "openavb_types_pub.h"
#include "openavb_audio_pub.h"
#include "openavb_trace_pub.h"
#include "openavb_mediaq_pub.h"
#include "openavb_intf_pub.h"
#include "openavb_map_aaf_audio_pub.h"

#define	AVB_LOG_COMPONENT	"Null Interface"
#include "openavb_log_pub.h" 

typedef struct {
	/////////////
	// Config data
	/////////////
	// Ignore timestamp at listener.
	bool ignoreTimestamp;
	avb_audio_rate_t audioRate;
	avb_audio_type_t audioType;
	avb_audio_bit_depth_t audioBitDepth;
	avb_audio_endian_t audioEndian;
	avb_audio_channels_t audioChannels;

	/////////////
	// Variable data
	/////////////
} pvt_data_t;

static media_q_pub_map_uncmp_audio_info_t *xGetPubMapAudioInfo(media_q_t *pMediaQ)
{
	if (!pMediaQ || !pMediaQ->pPubMapInfo || !pMediaQ->pMediaQDataFormat) {
		return NULL;
	}

	if (strcmp(pMediaQ->pMediaQDataFormat, MapUncmpAudioMediaQDataFormat) == 0
		|| strcmp(pMediaQ->pMediaQDataFormat, MapAVTPAudioMediaQDataFormat) == 0) {
		return (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
	}

	return NULL;
}

static void xSyncAudioInfo(media_q_t *pMediaQ, const pvt_data_t *pPvtData)
{
	media_q_pub_map_uncmp_audio_info_t *pPubMapAudioInfo = xGetPubMapAudioInfo(pMediaQ);
	if (!pPubMapAudioInfo || !pPvtData) {
		return;
	}

	pPubMapAudioInfo->audioRate = pPvtData->audioRate;
	pPubMapAudioInfo->audioType = pPvtData->audioType;
	pPubMapAudioInfo->audioBitDepth = pPvtData->audioBitDepth;
	pPubMapAudioInfo->audioEndian = pPvtData->audioEndian;
	pPubMapAudioInfo->audioChannels = pPvtData->audioChannels;
}


// Each configuration name value pair for this mapping will result in this callback being called.
void openavbIntfNullCfgCB(media_q_t *pMediaQ, const char *name, const char *value) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);

	if (pMediaQ) {
		char *pEnd;
		long tmp;
		long val;

		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private interface module data not allocated.");
			return;
		}

		if (strcmp(name, "intf_nv_ignore_timestamp") == 0) {
			tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && tmp == 1) {
				pPvtData->ignoreTimestamp = (tmp == 1);
			}
		}
		else if (strcmp(name, "intf_nv_audio_rate") == 0) {
			val = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && val >= AVB_AUDIO_RATE_8KHZ && val <= AVB_AUDIO_RATE_192KHZ) {
				pPvtData->audioRate = (avb_audio_rate_t)val;
			}
			else {
				AVB_LOG_ERROR("Invalid audio rate configured for intf_nv_audio_rate.");
			}
		}
		else if (strcmp(name, "intf_nv_audio_bit_depth") == 0) {
			val = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && val >= AVB_AUDIO_BIT_DEPTH_1BIT && val <= AVB_AUDIO_BIT_DEPTH_64BIT) {
				pPvtData->audioBitDepth = (avb_audio_bit_depth_t)val;
			}
			else {
				AVB_LOG_ERROR("Invalid audio bit depth configured for intf_nv_audio_bit_depth.");
			}
		}
		else if (strcmp(name, "intf_nv_audio_type") == 0) {
			if (strncasecmp(value, "float", 5) == 0) {
				pPvtData->audioType = AVB_AUDIO_TYPE_FLOAT;
			}
			else if (strncasecmp(value, "sign", 4) == 0 || strncasecmp(value, "int", 3) == 0) {
				pPvtData->audioType = AVB_AUDIO_TYPE_INT;
			}
			else if (strncasecmp(value, "unsign", 6) == 0 || strncasecmp(value, "uint", 4) == 0) {
				pPvtData->audioType = AVB_AUDIO_TYPE_UINT;
			}
			else {
				AVB_LOG_ERROR("Invalid audio type configured for intf_nv_audio_type.");
			}
		}
		else if (strcmp(name, "intf_nv_audio_endian") == 0) {
			if (strncasecmp(value, "big", 3) == 0) {
				pPvtData->audioEndian = AVB_AUDIO_ENDIAN_BIG;
			}
			else if (strncasecmp(value, "little", 6) == 0) {
				pPvtData->audioEndian = AVB_AUDIO_ENDIAN_LITTLE;
			}
			else {
				AVB_LOG_ERROR("Invalid audio endian configured for intf_nv_audio_endian.");
			}
		}
		else if (strcmp(name, "intf_nv_audio_channels") == 0) {
			val = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && val >= AVB_AUDIO_CHANNELS_1) {
				pPvtData->audioChannels = (avb_audio_channels_t)val;
			}
			else {
				AVB_LOG_ERROR("Invalid audio channels configured for intf_nv_audio_channels.");
			}
		}

		xSyncAudioInfo(pMediaQ, pPvtData);
	}

	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

void openavbIntfNullGenInitCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	if (pMediaQ) {
		xSyncAudioInfo(pMediaQ, (const pvt_data_t *)pMediaQ->pPvtIntfInfo);
	}
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// A call to this callback indicates that this interface module will be
// a talker. Any talker initialization can be done in this function.
void openavbIntfNullTxInitCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	if (pMediaQ) {
		xSyncAudioInfo(pMediaQ, (const pvt_data_t *)pMediaQ->pPvtIntfInfo);
	}
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// This callback will be called for each AVB transmit interval. Commonly this will be
// 4000 or 8000 times  per second.
bool openavbIntfNullTxCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF_DETAIL);
	if (pMediaQ) {
		//pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;

		media_q_item_t *pMediaQItem = openavbMediaQHeadLock(pMediaQ);
		if (pMediaQItem) {
			openavbAvtpTimeSetToWallTime(pMediaQItem->pAvtpTime);
			
			// Set 1 byte
			*(U8 *)(pMediaQItem->pPubData) = 0xff;
			pMediaQItem->dataLen = 1;
			
			openavbMediaQHeadPush(pMediaQ);
			AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
			return TRUE;
		}
		else {
			AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
			return FALSE;	// Media queue full
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
	return FALSE;
}

// A call to this callback indicates that this interface module will be
// a listener. Any listener initialization can be done in this function.
void openavbIntfNullRxInitCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	if (pMediaQ) {
		xSyncAudioInfo(pMediaQ, (const pvt_data_t *)pMediaQ->pPvtIntfInfo);
	}
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// This callback is called when acting as a listener.
bool openavbIntfNullRxCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF_DETAIL);
	if (pMediaQ) {
		bool moreItems = TRUE;
		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private interface module data not allocated.");
			return FALSE;
		}

		while (moreItems) {
			media_q_item_t *pMediaQItem = openavbMediaQTailLock(pMediaQ, pPvtData->ignoreTimestamp);
			if (pMediaQItem) {
				openavbMediaQTailPull(pMediaQ);
			}
			else {
				moreItems = FALSE;
			}
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
	return FALSE;
}

// This callback will be called when the interface needs to be closed. All shutdown should 
// occur in this function.
void openavbIntfNullEndCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

void openavbIntfNullGenEndCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// Main initialization entry point into the interface module
extern DLL_EXPORT bool openavbIntfNullInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);

	if (pMediaQ) {
		pMediaQ->pPvtIntfInfo = calloc(1, sizeof(pvt_data_t));		// Memory freed by the media queue when the media queue is destroyed.

		if (!pMediaQ->pPvtIntfInfo) {
			AVB_LOG_ERROR("Unable to allocate memory for AVTP interface module.");
			return FALSE;
		}

		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;

		pIntfCB->intf_cfg_cb = openavbIntfNullCfgCB;
		pIntfCB->intf_gen_init_cb = openavbIntfNullGenInitCB;
		pIntfCB->intf_tx_init_cb = openavbIntfNullTxInitCB;
		pIntfCB->intf_tx_cb = openavbIntfNullTxCB;
		pIntfCB->intf_rx_init_cb = openavbIntfNullRxInitCB;
		pIntfCB->intf_rx_cb = openavbIntfNullRxCB;
		pIntfCB->intf_end_cb = openavbIntfNullEndCB;
		pIntfCB->intf_gen_end_cb = openavbIntfNullGenEndCB;

		pPvtData->ignoreTimestamp = FALSE;
		pPvtData->audioRate = AVB_AUDIO_RATE_48KHZ;
		pPvtData->audioType = AVB_AUDIO_TYPE_INT;
		pPvtData->audioBitDepth = AVB_AUDIO_BIT_DEPTH_32BIT;
		pPvtData->audioEndian = AVB_AUDIO_ENDIAN_BIG;
		pPvtData->audioChannels = AVB_AUDIO_CHANNELS_2;
		xSyncAudioInfo(pMediaQ, pPvtData);
	}

	AVB_TRACE_EXIT(AVB_TRACE_INTF);
	return TRUE;
}
