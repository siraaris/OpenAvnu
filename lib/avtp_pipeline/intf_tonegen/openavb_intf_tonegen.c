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
* MODULE SUMMARY : Tone generator interface module. Talker only.
* 
* - This interface module generates and audio tone for use with -6 and AAF mappings
* - Requires an OSAL sin implementation of reasonable performance. 
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <endian.h>
#include <unistd.h>
#include <pthread.h>
#include "openavb_platform_pub.h"
#include "openavb_osal_pub.h"
#include "openavb_types_pub.h"
#include "openavb_trace_pub.h"
#include "openavb_mediaq_pub.h"
#include "openavb_map_uncmp_audio_pub.h"
#include "openavb_map_aaf_audio_pub.h"
#include "openavb_intf_pub.h"
#include "openavb_mcs.h"

#define	AVB_LOG_COMPONENT	"Tone Gen Interface"
#include "openavb_log_pub.h"

#define PI 3.14159265358979f

typedef struct {
	U8 *pData;
	U32 dataLen;
	U64 timestampNs;
} tonegen_fifo_item_t;

typedef struct {
	/////////////
	// Config data
	/////////////
	// intf_nv_tone_hz: The tone hz to generate
	U32 toneHz;

	// intf_nv_on_off_interval_msec: Interval for turning tone on and off. A value of zero will keep the tone on.
	U32 onOffIntervalMSec;

	// Simple melody
	char *pMelodyString;

	// intf_nv_audio_rate
	avb_audio_rate_t audioRate;

	// intf_nv_audio_type
	avb_audio_type_t audioType;

	// intf_nv_audio_bit_depth
	avb_audio_bit_depth_t audioBitDepth;

	// intf_nv_audio_endian
	avb_audio_endian_t audioEndian;

	// intf_nv_channels
	avb_audio_channels_t audioChannels;

	// Volume for the tone generation
	float volume;

	// Fixed 32 bit value 1
	bool fv1Enabled;
	U32 fv1;

	// Fixed 32 bit value 2
	bool fv2Enabled;
	U32 fv2;

	/////////////
	// Variable data
	/////////////

	// Packing interval
	U32 intervalCounter;

	// Keeps track of if the tone is currently on or off
	U32 freq;

	// Keeps track of how long before toggling the tone on / off
	U32 freqCountdown;

	// Ratio precalc
	float ratio;

	// Index to into the melody string
	U32 melodyIdx;

	// Length of the melody string
	U32 melodyLen;

	U32 fvChannels;

	// Media clock synthesis for precise timestamps
	mcs_t mcs;

	bool fixedTimestampEnabled;

	// Per-instance running frame count.
	U32 runningFrameCnt;

	// Optional jitter-absorbing FIFO between sample generation and MediaQ push.
	bool jitterFifoEnabled;
	U32 jitterFifoItemCount;
	U32 jitterFifoPrefillItems;
	U32 jitterFifoLowWaterItems;
	U32 jitterFifoHighWaterItems;
	U32 jitterFifoLogIntervalSec;

	bool jitterFifoInitialized;
	bool jitterFifoThreadRunning;
	bool jitterFifoStopRequested;
	bool jitterFifoPrefillDone;

	pthread_t jitterFifoThread;
	pthread_mutex_t jitterFifoMutex;
	pthread_cond_t jitterFifoCond;

	tonegen_fifo_item_t *pJitterFifoItems;
	U32 jitterFifoReadIdx;
	U32 jitterFifoWriteIdx;
	U32 jitterFifoCount;

	// Timestamp synth for FIFO mode.
	bool fifoTsInitialized;
	U64 fifoTsCurrentNs;
	U64 fifoTsStepNs;
	U32 fifoTsStepRem;
	U32 fifoTsStepDen;
	U32 fifoTsRemAccum;
	U32 fifoTsRuntimeLeadUsec;

	// FIFO diagnostics.
	U64 jitterFifoProduced;
	U64 jitterFifoConsumed;
	U64 jitterFifoUnderrun;
	U64 jitterFifoOverrun;
	U32 jitterFifoLevelMin;
	U32 jitterFifoLevelMax;
	U64 jitterFifoLastLogNs;

} pvt_data_t;

static U16 convertToDesiredEndianOrder16(U16 hostData, avb_audio_endian_t audioEndian);
static U32 convertToDesiredEndianOrder32(U32 hostData, avb_audio_endian_t audioEndian);

#define MSEC_PER_COUNT 250
static void xGetMelodyToneAndDuration(char note, char count, U32 *freq, U32 *sampleMSec)
{
	switch (note) {
		case 'A':
			*freq = 220;
			break;
		case 'B':
			*freq = 246;
			break;
		case 'C':
			*freq = 261;
			break;
		case 'D':
			*freq = 293;
			break;
		case 'E':
			*freq = 329;
			break;
		case 'F':
			*freq = 349;
			break;
		case 'G':
			*freq = 391;
			break;
		case 'a':
			*freq = 440;
			break;
		case 'b':
			*freq = 493;
			break;
		case 'c':
			*freq = 523;
			break;
		case 'd':
			*freq = 587;
			break;
		case 'e':
			*freq = 659;
			break;
		case 'f':
			*freq = 698;
			break;
		case 'g':
			*freq = 783;
			break;
		case '-':
		default:
			*freq = 0;			
			break;
	}
	
	switch (count) {
		case '1':
			*sampleMSec = MSEC_PER_COUNT * 1;
			break;
		case '2':
			*sampleMSec = MSEC_PER_COUNT * 2;
			break;
		case '3':
			*sampleMSec = MSEC_PER_COUNT * 3;
			break;
		case '4':
			*sampleMSec = MSEC_PER_COUNT * 4;
			break;
		default:
			*sampleMSec = MSEC_PER_COUNT * 4;
			break;
	}
}

static bool xSupportedMappingFormat(media_q_t *pMediaQ)
{
	if (pMediaQ) {
		if (pMediaQ->pMediaQDataFormat) {
			if (strcmp(pMediaQ->pMediaQDataFormat, MapUncmpAudioMediaQDataFormat) == 0 || strcmp(pMediaQ->pMediaQDataFormat, MapAVTPAudioMediaQDataFormat) == 0) {
				return TRUE;
			}
		}
	}
	return FALSE;
}

static void xTonegenInitFifoTimestampSynth(
		pvt_data_t *pPvtData,
		const media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo)
{
	U64 num;
	U64 den;

	if (!pPvtData || !pPubMapUncmpAudioInfo || pPubMapUncmpAudioInfo->audioRate == 0) {
		return;
	}

	num = (U64)NANOSECONDS_PER_SECOND * (U64)pPubMapUncmpAudioInfo->framesPerItem;
	den = (U64)pPubMapUncmpAudioInfo->audioRate;

	pPvtData->fifoTsStepNs = num / den;
	pPvtData->fifoTsStepRem = (U32)(num % den);
	pPvtData->fifoTsStepDen = (U32)den;
	pPvtData->fifoTsRemAccum = 0;
	pPvtData->fifoTsInitialized = FALSE;
}

static U64 xTonegenNextFifoTimestampNs(pvt_data_t *pPvtData)
{
	U64 nowNs = 0;

	if (!pPvtData) {
		return 0;
	}

	if (!pPvtData->fifoTsInitialized) {
		U64 bufferedLeadNs = 0;
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
		if (pPvtData->jitterFifoHighWaterItems > 0) {
			bufferedLeadNs =
				(U64)(pPvtData->jitterFifoHighWaterItems - 1U) * pPvtData->fifoTsStepNs;
		}
		pPvtData->fifoTsCurrentNs =
			nowNs
			+ bufferedLeadNs
			+ ((U64)pPvtData->fifoTsRuntimeLeadUsec * 1000ULL);
		pPvtData->fifoTsInitialized = TRUE;
	}
	else {
		pPvtData->fifoTsCurrentNs += pPvtData->fifoTsStepNs;
		if (pPvtData->fifoTsStepDen > 0 && pPvtData->fifoTsStepRem > 0) {
			pPvtData->fifoTsRemAccum += pPvtData->fifoTsStepRem;
			if (pPvtData->fifoTsRemAccum >= pPvtData->fifoTsStepDen) {
				U32 carry = pPvtData->fifoTsRemAccum / pPvtData->fifoTsStepDen;
				pPvtData->fifoTsCurrentNs += (U64)carry;
				pPvtData->fifoTsRemAccum -= carry * pPvtData->fifoTsStepDen;
			}
		}
	}

	return pPvtData->fifoTsCurrentNs;
}

static void xTonegenMaybeLogFifoStats(pvt_data_t *pPvtData)
{
	U64 nowNs;

	if (!pPvtData || pPvtData->jitterFifoLogIntervalSec == 0) {
		return;
	}

	CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
	if (pPvtData->jitterFifoLastLogNs == 0) {
		pPvtData->jitterFifoLastLogNs = nowNs;
		return;
	}
	if ((nowNs - pPvtData->jitterFifoLastLogNs) <
			((U64)pPvtData->jitterFifoLogIntervalSec * (U64)NANOSECONDS_PER_SECOND)) {
		return;
	}

	AVB_LOGF_INFO("ToneGen FIFO: level=%u min=%u max=%u produced=%llu consumed=%llu underrun=%llu overrun=%llu prefill_done=%u",
			pPvtData->jitterFifoCount,
			pPvtData->jitterFifoLevelMin,
			pPvtData->jitterFifoLevelMax,
			(unsigned long long)pPvtData->jitterFifoProduced,
			(unsigned long long)pPvtData->jitterFifoConsumed,
			(unsigned long long)pPvtData->jitterFifoUnderrun,
			(unsigned long long)pPvtData->jitterFifoOverrun,
			pPvtData->jitterFifoPrefillDone ? 1 : 0);

	pPvtData->jitterFifoLevelMin = pPvtData->jitterFifoCount;
	pPvtData->jitterFifoLevelMax = pPvtData->jitterFifoCount;
	pPvtData->jitterFifoLastLogNs = nowNs;
}

static bool xTonegenGenerateItem(
		media_q_t *pMediaQ,
		pvt_data_t *pPvtData,
		U8 *pData,
		U32 *pDataLen,
		U64 *pTimestampNs,
		bool useFifoTimestampSynth)
{
	media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo;
	U32 frameCnt;
	U32 channelCnt;
	U8 *pWrite;

	if (!pMediaQ || !pPvtData || !pData || !pDataLen || !pTimestampNs) {
		return FALSE;
	}

	pPubMapUncmpAudioInfo = pMediaQ->pPubMapInfo;
	if (!pPubMapUncmpAudioInfo) {
		return FALSE;
	}

	pWrite = pData;
	for (frameCnt = 0; frameCnt < pPubMapUncmpAudioInfo->framesPerItem; frameCnt++) {
		// Check for tone on / off toggle
		if (!pPvtData->freqCountdown) {
			if (pPvtData->pMelodyString) {
				U32 intervalMSec;
				xGetMelodyToneAndDuration(
					pPvtData->pMelodyString[pPvtData->melodyIdx],
					pPvtData->pMelodyString[pPvtData->melodyIdx + 1],
					&pPvtData->freq,
					&intervalMSec);
				pPvtData->melodyIdx += 2;

				pPvtData->freqCountdown = (pPubMapUncmpAudioInfo->audioRate / 1000) * intervalMSec;
				if (pPvtData->melodyIdx >= pPvtData->melodyLen) {
					pPvtData->melodyIdx = 0;
				}
			}
			else {
				if (pPvtData->onOffIntervalMSec > 0) {
					if (pPvtData->freq == 0) {
						pPvtData->freq = pPvtData->toneHz;
					}
					else {
						pPvtData->freq = 0;
					}
					pPvtData->freqCountdown = (pPubMapUncmpAudioInfo->audioRate / 1000) * pPvtData->onOffIntervalMSec;
				}
				else {
					pPvtData->freqCountdown = pPubMapUncmpAudioInfo->audioRate;
					pPvtData->freq = pPvtData->toneHz;
				}
			}
			pPvtData->ratio = (float)pPvtData->freq / (float)pPubMapUncmpAudioInfo->audioRate;
		}
		pPvtData->freqCountdown--;

		{
			float value = SIN(2 * PI * (pPvtData->runningFrameCnt++ % pPubMapUncmpAudioInfo->audioRate) * pPvtData->ratio) * pPvtData->volume;
			for (channelCnt = 0; channelCnt < pPubMapUncmpAudioInfo->audioChannels - pPvtData->fvChannels; channelCnt++) {
				if (pPvtData->audioType == AVB_AUDIO_TYPE_INT) {
					if (pPvtData->audioBitDepth == 32) {
						S32 sample32 = (S32)(value * (32000 << 16));
						S32 tmp32 = convertToDesiredEndianOrder32(sample32, pPvtData->audioEndian);
						memcpy(pWrite, (U8 *)&tmp32, 4);
						pWrite += 4;
					}
					else if (pPvtData->audioBitDepth == 24) {
						S32 sample24 = (S32)(value * (32000 << 16));
						S32 tmp24 = convertToDesiredEndianOrder32(sample24, pPvtData->audioEndian);
						if (pPvtData->audioEndian == AVB_AUDIO_ENDIAN_BIG) {
							memcpy(pWrite, (U8 *)&tmp24, 3);
						}
						else {
							memcpy(pWrite, ((U8 *)&tmp24) + 1, 3);
						}
						pWrite += 3;
					}
					else if (pPvtData->audioBitDepth == 16) {
						S16 sample16 = (S32)(value * 32000);
						S16 tmp16 = convertToDesiredEndianOrder16(sample16, pPvtData->audioEndian);
						memcpy(pWrite, (U8 *)&tmp16, 2);
						pWrite += 2;
					}
				}
				else if (pPvtData->audioType == AVB_AUDIO_TYPE_FLOAT) {
					U32 tmp32f;
					memcpy((U8 *)&tmp32f, (U8 *)&value, 4);
					tmp32f = convertToDesiredEndianOrder32(tmp32f, pPvtData->audioEndian);
					memcpy(pWrite, (U8 *)&tmp32f, 4);
					pWrite += 4;
				}
				else {
					AVB_LOG_ERROR("Audio sample size format not implemented yet for tone generator interface module");
					return FALSE;
				}
			}

			if (pPvtData->fvChannels > 0 && pPvtData->audioType == AVB_AUDIO_TYPE_INT && pPvtData->audioBitDepth == 32) {
				if (pPvtData->fv1Enabled) {
					S32 tmp32 = convertToDesiredEndianOrder32(pPvtData->fv1, pPvtData->audioEndian);
					memcpy(pWrite, (U8 *)&tmp32, 4);
					pWrite += 4;
				}
				if (pPvtData->fv2Enabled) {
					S32 tmp32 = convertToDesiredEndianOrder32(pPvtData->fv2, pPvtData->audioEndian);
					memcpy(pWrite, (U8 *)&tmp32, 4);
					pWrite += 4;
				}
			}
		}
	}

	*pDataLen = pPubMapUncmpAudioInfo->itemSize;
	if (useFifoTimestampSynth) {
		*pTimestampNs = xTonegenNextFifoTimestampNs(pPvtData);
	}
	else if (!pPvtData->fixedTimestampEnabled) {
		CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, pTimestampNs);
	}
	else {
		openavbMcsAdvance(&pPvtData->mcs);
		*pTimestampNs = pPvtData->mcs.edgeTime;
	}

	return TRUE;
}

static void *openavbIntfToneGenFifoThread(void *pv)
{
	media_q_t *pMediaQ = pv;
	pvt_data_t *pPvtData;

	if (!pMediaQ) {
		return NULL;
	}
	pPvtData = pMediaQ->pPvtIntfInfo;
	if (!pPvtData) {
		return NULL;
	}

	while (TRUE) {
		U32 slotIdx;
		U32 dataLen;
		U64 tsNs;
		bool stopRequested = FALSE;

		pthread_mutex_lock(&pPvtData->jitterFifoMutex);
		while (!pPvtData->jitterFifoStopRequested &&
				pPvtData->jitterFifoCount >= pPvtData->jitterFifoHighWaterItems) {
			pthread_cond_wait(&pPvtData->jitterFifoCond, &pPvtData->jitterFifoMutex);
		}
		if (pPvtData->jitterFifoStopRequested) {
			stopRequested = TRUE;
		}
		slotIdx = pPvtData->jitterFifoWriteIdx;
		pthread_mutex_unlock(&pPvtData->jitterFifoMutex);

		if (stopRequested) {
			break;
		}

		if (!xTonegenGenerateItem(
				pMediaQ,
				pPvtData,
				pPvtData->pJitterFifoItems[slotIdx].pData,
				&dataLen,
				&tsNs,
				TRUE)) {
			AVB_LOG_ERROR("ToneGen FIFO: failed to generate item");
			usleep(1000);
			continue;
		}

		pthread_mutex_lock(&pPvtData->jitterFifoMutex);
		if (pPvtData->jitterFifoCount >= pPvtData->jitterFifoItemCount) {
			// Consumer fell behind unexpectedly; drop oldest to preserve forward progress.
			pPvtData->jitterFifoReadIdx = (pPvtData->jitterFifoReadIdx + 1) % pPvtData->jitterFifoItemCount;
			pPvtData->jitterFifoCount--;
			pPvtData->jitterFifoOverrun++;
		}

		pPvtData->pJitterFifoItems[slotIdx].dataLen = dataLen;
		pPvtData->pJitterFifoItems[slotIdx].timestampNs = tsNs;
		pPvtData->jitterFifoWriteIdx = (pPvtData->jitterFifoWriteIdx + 1) % pPvtData->jitterFifoItemCount;
		pPvtData->jitterFifoCount++;
		pPvtData->jitterFifoProduced++;
		if (pPvtData->jitterFifoCount > pPvtData->jitterFifoLevelMax) {
			pPvtData->jitterFifoLevelMax = pPvtData->jitterFifoCount;
		}
		if (pPvtData->jitterFifoCount < pPvtData->jitterFifoLevelMin) {
			pPvtData->jitterFifoLevelMin = pPvtData->jitterFifoCount;
		}
		if (!pPvtData->jitterFifoPrefillDone &&
				pPvtData->jitterFifoCount >= pPvtData->jitterFifoPrefillItems) {
			pPvtData->jitterFifoPrefillDone = TRUE;
		}
		pthread_cond_broadcast(&pPvtData->jitterFifoCond);
		pthread_mutex_unlock(&pPvtData->jitterFifoMutex);
	}

	return NULL;
}

static bool xTonegenFifoSetup(media_q_t *pMediaQ, pvt_data_t *pPvtData)
{
	U32 i;
	media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo = pMediaQ->pPubMapInfo;

	if (!pPvtData->jitterFifoEnabled) {
		return TRUE;
	}
	if (!pPubMapUncmpAudioInfo || pPubMapUncmpAudioInfo->itemSize == 0) {
		AVB_LOG_ERROR("ToneGen FIFO: map audio info unavailable at setup.");
		return FALSE;
	}

	if (pPvtData->jitterFifoItemCount < 4) {
		pPvtData->jitterFifoItemCount = 4;
	}
	if (pPvtData->jitterFifoHighWaterItems == 0 ||
			pPvtData->jitterFifoHighWaterItems >= pPvtData->jitterFifoItemCount) {
		pPvtData->jitterFifoHighWaterItems = pPvtData->jitterFifoItemCount - 1;
	}
	if (pPvtData->jitterFifoLowWaterItems >= pPvtData->jitterFifoHighWaterItems) {
		pPvtData->jitterFifoLowWaterItems = pPvtData->jitterFifoHighWaterItems / 2;
	}
	if (pPvtData->jitterFifoPrefillItems == 0 ||
			pPvtData->jitterFifoPrefillItems > pPvtData->jitterFifoHighWaterItems) {
		pPvtData->jitterFifoPrefillItems = pPvtData->jitterFifoLowWaterItems;
	}

	pPvtData->pJitterFifoItems = (tonegen_fifo_item_t *)calloc(pPvtData->jitterFifoItemCount, sizeof(tonegen_fifo_item_t));
	if (!pPvtData->pJitterFifoItems) {
		AVB_LOG_ERROR("ToneGen FIFO: failed to allocate item descriptors.");
		return FALSE;
	}

	for (i = 0; i < pPvtData->jitterFifoItemCount; i++) {
		pPvtData->pJitterFifoItems[i].pData = (U8 *)malloc(pPubMapUncmpAudioInfo->itemSize);
		if (!pPvtData->pJitterFifoItems[i].pData) {
			AVB_LOG_ERROR("ToneGen FIFO: failed to allocate item data.");
			return FALSE;
		}
		pPvtData->pJitterFifoItems[i].dataLen = 0;
		pPvtData->pJitterFifoItems[i].timestampNs = 0;
	}

	xTonegenInitFifoTimestampSynth(pPvtData, pPubMapUncmpAudioInfo);

	pPvtData->jitterFifoReadIdx = 0;
	pPvtData->jitterFifoWriteIdx = 0;
	pPvtData->jitterFifoCount = 0;
	pPvtData->jitterFifoProduced = 0;
	pPvtData->jitterFifoConsumed = 0;
	pPvtData->jitterFifoUnderrun = 0;
	pPvtData->jitterFifoOverrun = 0;
	pPvtData->jitterFifoLevelMin = pPvtData->jitterFifoItemCount;
	pPvtData->jitterFifoLevelMax = 0;
	pPvtData->jitterFifoLastLogNs = 0;
	pPvtData->jitterFifoPrefillDone = FALSE;
	pPvtData->jitterFifoStopRequested = FALSE;

	if (pthread_mutex_init(&pPvtData->jitterFifoMutex, NULL) != 0) {
		AVB_LOG_ERROR("ToneGen FIFO: failed to initialize mutex.");
		return FALSE;
	}
	if (pthread_cond_init(&pPvtData->jitterFifoCond, NULL) != 0) {
		AVB_LOG_ERROR("ToneGen FIFO: failed to initialize condition variable.");
		return FALSE;
	}
	pPvtData->jitterFifoInitialized = TRUE;

	if (pthread_create(&pPvtData->jitterFifoThread, NULL, openavbIntfToneGenFifoThread, pMediaQ) != 0) {
		AVB_LOG_ERROR("ToneGen FIFO: failed to start producer thread.");
		return FALSE;
	}
	pPvtData->jitterFifoThreadRunning = TRUE;

	AVB_LOGF_INFO("ToneGen FIFO enabled: items=%u prefill=%u low=%u high=%u log=%us",
			pPvtData->jitterFifoItemCount,
			pPvtData->jitterFifoPrefillItems,
			pPvtData->jitterFifoLowWaterItems,
			pPvtData->jitterFifoHighWaterItems,
			pPvtData->jitterFifoLogIntervalSec);

	return TRUE;
}

static void xTonegenFifoTeardown(pvt_data_t *pPvtData)
{
	U32 i;

	if (!pPvtData) {
		return;
	}

	if (pPvtData->jitterFifoInitialized) {
		pthread_mutex_lock(&pPvtData->jitterFifoMutex);
		pPvtData->jitterFifoStopRequested = TRUE;
		pthread_cond_broadcast(&pPvtData->jitterFifoCond);
		pthread_mutex_unlock(&pPvtData->jitterFifoMutex);
	}

	if (pPvtData->jitterFifoThreadRunning) {
		pthread_join(pPvtData->jitterFifoThread, NULL);
		pPvtData->jitterFifoThreadRunning = FALSE;
	}

	if (pPvtData->jitterFifoInitialized) {
		pthread_cond_destroy(&pPvtData->jitterFifoCond);
		pthread_mutex_destroy(&pPvtData->jitterFifoMutex);
		pPvtData->jitterFifoInitialized = FALSE;
	}

	if (pPvtData->pJitterFifoItems) {
		for (i = 0; i < pPvtData->jitterFifoItemCount; i++) {
			free(pPvtData->pJitterFifoItems[i].pData);
			pPvtData->pJitterFifoItems[i].pData = NULL;
		}
		free(pPvtData->pJitterFifoItems);
		pPvtData->pJitterFifoItems = NULL;
	}
}

// Each configuration name value pair for this mapping will result in this callback being called.
void openavbIntfToneGenCfgCB(media_q_t *pMediaQ, const char *name, const char *value) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	
	char *pEnd;
	U32 val;

	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private interface module data not allocated.");
			return;
		}

		media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo;
		pPubMapUncmpAudioInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
		if (!pPubMapUncmpAudioInfo) {
			AVB_LOG_ERROR("Public map data for audio info not allocated.");
			return;
		}

		if (strcmp(name, "intf_nv_tone_hz") == 0) {
			pPvtData->toneHz = strtol(value, &pEnd, 10);
		}

		else if (strcmp(name, "intf_nv_on_off_interval_msec") == 0) {
			pPvtData->onOffIntervalMSec = strtol(value, &pEnd, 10);
		}

		else if (strcmp(name, "intf_nv_melody_string") == 0) {
			if (pPvtData->pMelodyString)
				free(pPvtData->pMelodyString);
			pPvtData->pMelodyString = strdup(value);
			if (pPvtData->pMelodyString) {
				pPvtData->melodyLen = strlen(pPvtData->pMelodyString);
			}
		}

		else if (strcmp(name, "intf_nv_audio_rate") == 0) {
			val = strtol(value, &pEnd, 10);
			// TODO: Should check for specific values
			if (val >= AVB_AUDIO_RATE_8KHZ && val <= AVB_AUDIO_RATE_192KHZ) {
				pPvtData->audioRate = (avb_audio_rate_t)val;
			}
			else {
				AVB_LOG_ERROR("Invalid audio rate configured for intf_nv_audio_rate.");
				pPvtData->audioRate = AVB_AUDIO_RATE_44_1KHZ;
			}

			// Give the audio parameters to the mapping module.
			if (xSupportedMappingFormat(pMediaQ)) {
				pPubMapUncmpAudioInfo->audioRate = pPvtData->audioRate;
			}
		}

		else if (strcmp(name, "intf_nv_audio_bit_depth") == 0) {
			val = strtol(value, &pEnd, 10);
			// TODO: Should check for specific values
			if (val >= AVB_AUDIO_BIT_DEPTH_1BIT && val <= AVB_AUDIO_BIT_DEPTH_64BIT) {
				pPvtData->audioBitDepth = (avb_audio_bit_depth_t)val;
			}
			else {
				AVB_LOG_ERROR("Invalid audio type configured for intf_nv_audio_bits.");
				pPvtData->audioBitDepth = AVB_AUDIO_BIT_DEPTH_24BIT;
			}

			// Give the audio parameters to the mapping module.
			if (xSupportedMappingFormat(pMediaQ)) {
				pPubMapUncmpAudioInfo->audioBitDepth = pPvtData->audioBitDepth;
			}
		}

		else if (strcmp(name, "intf_nv_audio_type") == 0) {
			if (strncasecmp(value, "float", 5) == 0)
				pPvtData->audioType = AVB_AUDIO_TYPE_FLOAT;
			else if (strncasecmp(value, "sign", 4) == 0 || strncasecmp(value, "int", 4) == 0)
				pPvtData->audioType = AVB_AUDIO_TYPE_INT;
			else if (strncasecmp(value, "unsign", 6) == 0 || strncasecmp(value, "uint", 4) == 0)
				pPvtData->audioType = AVB_AUDIO_TYPE_UINT;
			else {
				AVB_LOG_ERROR("Invalid audio type configured for intf_nv_audio_type.");
				pPvtData->audioType = AVB_AUDIO_TYPE_UNSPEC;
			}

			// Give the audio parameters to the mapping module.
			if (xSupportedMappingFormat(pMediaQ)) {
				pPubMapUncmpAudioInfo->audioType = pPvtData->audioType;
			}
		}

		else if (strcmp(name, "intf_nv_audio_endian") == 0) {
			if (strncasecmp(value, "big", 3) == 0)
				pPvtData->audioEndian = AVB_AUDIO_ENDIAN_BIG;
			else if (strncasecmp(value, "little", 6) == 0)
				pPvtData->audioEndian = AVB_AUDIO_ENDIAN_LITTLE;
			else {
				AVB_LOG_ERROR("Invalid audio type configured for intf_nv_audio_endian.");
				pPvtData->audioEndian = AVB_AUDIO_ENDIAN_UNSPEC;
			}

			// Give the audio parameters to the mapping module.
			if (xSupportedMappingFormat(pMediaQ)) {
				pPubMapUncmpAudioInfo->audioEndian = pPvtData->audioEndian;
			}
		}

		else if (strcmp(name, "intf_nv_audio_channels") == 0) {
			val = strtol(value, &pEnd, 10);
			// TODO: Should check for specific values
			if (val >= AVB_AUDIO_CHANNELS_1) {
				pPvtData->audioChannels = (avb_audio_channels_t)val;
			}
			else {
				AVB_LOG_ERROR("Invalid audio channels configured for intf_nv_audio_channels.");
				pPvtData->audioChannels = (avb_audio_channels_t)AVB_AUDIO_CHANNELS_2;
			}

			// Give the audio parameters to the mapping module.
			if (xSupportedMappingFormat(pMediaQ)) {
				pPubMapUncmpAudioInfo->audioChannels = pPvtData->audioChannels;
			}
		}

		else if (strcmp(name, "intf_nv_volume") == 0) {
			S32 vol = strtol(value, &pEnd, 10);
			pPvtData->volume = pow(10.0, vol/10.0);
		}

		else if (strcmp(name, "intf_nv_fv1") == 0) {
			pPvtData->fv1 = strtol(value, &pEnd, 10);
			pPvtData->fv1Enabled = true;
			pPvtData->fvChannels++;
		}

		else if (strcmp(name, "intf_nv_fv2") == 0) {
			pPvtData->fv2 = strtol(value, &pEnd, 10);
			pPvtData->fv2Enabled = true;
			pPvtData->fvChannels++;
		}

		else if (strcmp(name, "intf_nv_jitter_fifo_enable") == 0) {
			pPvtData->jitterFifoEnabled = (strtol(value, &pEnd, 10) != 0);
		}
		else if (strcmp(name, "intf_nv_jitter_fifo_item_count") == 0) {
			pPvtData->jitterFifoItemCount = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "intf_nv_jitter_fifo_prefill") == 0) {
			pPvtData->jitterFifoPrefillItems = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "intf_nv_jitter_fifo_low_water") == 0) {
			pPvtData->jitterFifoLowWaterItems = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "intf_nv_jitter_fifo_high_water") == 0) {
			pPvtData->jitterFifoHighWaterItems = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "intf_nv_jitter_fifo_log_interval_sec") == 0) {
			pPvtData->jitterFifoLogIntervalSec = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "intf_nv_fixed_ts_runtime_lead_usec") == 0 ||
				strcmp(name, "intf_nv_timestamp_runtime_lead_usec") == 0) {
			pPvtData->fifoTsRuntimeLeadUsec = strtol(value, &pEnd, 10);
		}

	}

	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

void openavbIntfToneGenGenInitCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// A call to this callback indicates that this interface module will be
// a talker. Any talker initialization can be done in this function.
void openavbIntfToneGenTxInitCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);

	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private interface module data not allocated.");
			return;
		}

		media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo;
		pPubMapUncmpAudioInfo = (media_q_pub_map_uncmp_audio_info_t *)pMediaQ->pPubMapInfo;
		if (!pPubMapUncmpAudioInfo) {
			AVB_LOG_ERROR("Public map data for audio info not allocated.");
			return;
		}
		
		// Will get toggle on at the first tx cb
		if (pPvtData->onOffIntervalMSec > 0) {
			pPvtData->freq = pPvtData->toneHz;
			pPvtData->freqCountdown = 0;
		}
		else {
			pPvtData->freq = 0;
			pPvtData->freqCountdown = 0;
		}
		
			pPvtData->melodyIdx = 0;
			pPvtData->fifoTsInitialized = FALSE;

			if (pPvtData->jitterFifoEnabled) {
				if (!xTonegenFifoSetup(pMediaQ, pPvtData)) {
					AVB_LOG_ERROR("ToneGen FIFO setup failed; disabling FIFO path");
					pPvtData->jitterFifoEnabled = FALSE;
					xTonegenFifoTeardown(pPvtData);
				}
				else if (pPvtData->fifoTsRuntimeLeadUsec > 0) {
					AVB_LOGF_INFO("ToneGen FIFO runtime timestamp lead: %u usec",
						pPvtData->fifoTsRuntimeLeadUsec);
				}
			}
		}

	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}


#define SWAPU16(x)	(((x) >> 8) | ((x) << 8))
static U16 convertToDesiredEndianOrder16(U16 hostData, avb_audio_endian_t audioEndian)
{
#if defined __BYTE_ORDER && defined __BIG_ENDIAN && defined __LITTLE_ENDIAN
#if __BYTE_ORDER == __BIG_ENDIAN
	if (audioEndian == AVB_AUDIO_ENDIAN_LITTLE) { hostData = SWAPU16(hostData); }
	return hostData;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	if (audioEndian == AVB_AUDIO_ENDIAN_BIG) { hostData = SWAPU16(hostData); }
	return hostData;
#else
	#error Unsupported Endian format
#endif
#else
	U16 test_var = 1;
	unsigned char test_endian* = (unsigned char*)&test_var;
	if (test_endian[0] == 0) {
		// This is big-endian.
		if (audioEndian == AVB_AUDIO_ENDIAN_LITTLE) { hostData = SWAPU16(hostData); }
	} else {
		// This is little-endian.
		if (audioEndian == AVB_AUDIO_ENDIAN_BIG) { hostData = SWAPU16(hostData); }
	}
	return hostData;
#endif
}

#define SWAPU32(x)	(((x) >> 24) | (((x) & 0x00FF0000) >> 8) | (((x) & 0x0000FF00) << 8) | ((x) << 24))
static U32 convertToDesiredEndianOrder32(U32 hostData, avb_audio_endian_t audioEndian)
{
#if defined __BYTE_ORDER && defined __BIG_ENDIAN && defined __LITTLE_ENDIAN
#if __BYTE_ORDER == __BIG_ENDIAN
	if (audioEndian == AVB_AUDIO_ENDIAN_LITTLE) { hostData = SWAPU32(hostData); }
	return hostData;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	if (audioEndian == AVB_AUDIO_ENDIAN_BIG) { hostData = SWAPU32(hostData); }
	return hostData;
#else
	#error Unsupported Endian format
#endif
#else
	U16 test_var = 1;
	unsigned char test_endian* = (unsigned char*)&test_var;
	if (test_endian[0] == 0) {
		// This is big-endian.
		if (audioEndian == AVB_AUDIO_ENDIAN_LITTLE) { hostData = SWAPU32(hostData); }
	} else {
		// This is little-endian.
		if (audioEndian == AVB_AUDIO_ENDIAN_BIG) { hostData = SWAPU32(hostData); }
	}
	return hostData;
#endif
}

// This callback will be called for each AVB transmit interval. Commonly this will be
// 4000 or 8000 times  per second.
bool openavbIntfToneGenTxCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF_DETAIL);

	if (pMediaQ) {
		media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo = pMediaQ->pPubMapInfo;
		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private interface module data not allocated.");
			return FALSE;
		}

		if (pPvtData->intervalCounter++ % pPubMapUncmpAudioInfo->packingFactor != 0)
			return TRUE;

		media_q_item_t *pMediaQItem = openavbMediaQHeadLock(pMediaQ);
		if (pMediaQItem) {
			if (pMediaQItem->itemSize < pPubMapUncmpAudioInfo->itemSize) {
				AVB_LOG_ERROR("Media queue item not large enough for samples");
			}

			if (pPvtData->jitterFifoEnabled) {
				bool gotItem = FALSE;
				pthread_mutex_lock(&pPvtData->jitterFifoMutex);
				if (!pPvtData->jitterFifoPrefillDone &&
						pPvtData->jitterFifoCount >= pPvtData->jitterFifoPrefillItems) {
					pPvtData->jitterFifoPrefillDone = TRUE;
				}
				if (pPvtData->jitterFifoPrefillDone && pPvtData->jitterFifoCount > 0) {
					tonegen_fifo_item_t *pItem = &pPvtData->pJitterFifoItems[pPvtData->jitterFifoReadIdx];
					memcpy(pMediaQItem->pPubData, pItem->pData, pItem->dataLen);
					pMediaQItem->dataLen = pItem->dataLen;
					openavbAvtpTimeSetToTimestampNS(pMediaQItem->pAvtpTime, pItem->timestampNs);

					pPvtData->jitterFifoReadIdx = (pPvtData->jitterFifoReadIdx + 1) % pPvtData->jitterFifoItemCount;
					pPvtData->jitterFifoCount--;
					pPvtData->jitterFifoConsumed++;
					if (pPvtData->jitterFifoCount < pPvtData->jitterFifoLevelMin) {
						pPvtData->jitterFifoLevelMin = pPvtData->jitterFifoCount;
					}
					if (pPvtData->jitterFifoCount > pPvtData->jitterFifoLevelMax) {
						pPvtData->jitterFifoLevelMax = pPvtData->jitterFifoCount;
					}
					gotItem = TRUE;
					pthread_cond_broadcast(&pPvtData->jitterFifoCond);
				}
				else if (pPvtData->jitterFifoPrefillDone && pPvtData->jitterFifoCount == 0) {
					// Re-enter prefill mode after a complete drain to avoid bursty restart.
					pPvtData->jitterFifoPrefillDone = FALSE;
					pPvtData->jitterFifoUnderrun++;
				}
				pthread_mutex_unlock(&pPvtData->jitterFifoMutex);

				if (gotItem) {
					openavbMediaQHeadPush(pMediaQ);
					xTonegenMaybeLogFifoStats(pPvtData);
					AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
					return TRUE;
				}

				openavbMediaQHeadUnlock(pMediaQ);
				xTonegenMaybeLogFifoStats(pPvtData);
				AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
				return FALSE;
			}
			else {
				U32 dataLen = 0;
				U64 tsNs = 0;
				if (!xTonegenGenerateItem(
						pMediaQ,
						pPvtData,
						(U8 *)pMediaQItem->pPubData,
						&dataLen,
						&tsNs,
						FALSE)) {
					openavbMediaQHeadUnlock(pMediaQ);
					AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
					return FALSE;
				}

				pMediaQItem->dataLen = dataLen;
				if (!pPvtData->fixedTimestampEnabled) {
					openavbAvtpTimeSetToWallTime(pMediaQItem->pAvtpTime);
				}
				else {
					openavbAvtpTimeSetToTimestampNS(pMediaQItem->pAvtpTime, tsNs);
				}
				openavbMediaQHeadPush(pMediaQ);
				AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
				return TRUE;
			}
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
void openavbIntfToneGenRxInitCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// This callback is called when acting as a listener.
bool openavbIntfToneGenRxCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF_DETAIL);
	AVB_TRACE_EXIT(AVB_TRACE_INTF_DETAIL);
	return FALSE;
}

// This callback will be called when the interface needs to be closed. All shutdown should 
// occur in this function.
void openavbIntfToneGenEndCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	if (pMediaQ && pMediaQ->pPvtIntfInfo) {
		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;
		xTonegenFifoTeardown(pPvtData);
		if (pPvtData->pMelodyString) {
			free(pPvtData->pMelodyString);
			pPvtData->pMelodyString = NULL;
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

void openavbIntfToneGenGenEndCB(media_q_t *pMediaQ) 
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

void openavbIntfToneGenEnableFixedTimestamp(media_q_t *pMediaQ, bool enabled, U32 transmitInterval, U32 batchFactor)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);
	if (pMediaQ && pMediaQ->pPvtIntfInfo && pMediaQ->pPubMapInfo) {
		media_q_pub_map_uncmp_audio_info_t *pPubMapUncmpAudioInfo = pMediaQ->pPubMapInfo;
		pvt_data_t *pPvtData = (pvt_data_t *)pMediaQ->pPvtIntfInfo;

		pPvtData->fixedTimestampEnabled = enabled;
		if (pPvtData->fixedTimestampEnabled) {
			U32 per, rate, rem;
			/* Ignore passed in transmit interval and use framesPerItem and audioRate so
			   we work with both AAF and 61883-6 */
			/* Carefully scale values to avoid U32 overflow or loss of precision */
			per = MICROSECONDS_PER_SECOND * pPubMapUncmpAudioInfo->framesPerItem * 10;
			rate = pPvtData->audioRate/100;
			transmitInterval = per/rate;
			rem = per % rate;
			if (rem != 0) {
				rem *= 10;
				rem /= rate;
			}
			openavbMcsInit(&pPvtData->mcs, transmitInterval, rem, 10);
			AVB_LOGF_INFO("Fixed timestamping enabled: %d %d/%d", transmitInterval, rem, 10);
		}

		if (batchFactor != 1) {
			AVB_LOGF_WARNING("batchFactor of %d ignored (must be 1)", batchFactor);
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_INTF);
}

// Main initialization entry point into the interface module
//extern DLL_EXPORT bool openavbIntfToneGenInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB)
extern bool DLL_EXPORT openavbIntfToneGenInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB)
{
	AVB_TRACE_ENTRY(AVB_TRACE_INTF);

	if (pMediaQ) {
		pMediaQ->pPvtIntfInfo = calloc(1, sizeof(pvt_data_t));		// Memory freed by the media queue when the media queue is destroyed.

		if (!pMediaQ->pPvtIntfInfo) {
			AVB_LOG_ERROR("Unable to allocate memory for AVTP interface module.");
			return FALSE;
		}

		pvt_data_t *pPvtData = pMediaQ->pPvtIntfInfo;

		pIntfCB->intf_cfg_cb = openavbIntfToneGenCfgCB;
		pIntfCB->intf_gen_init_cb = openavbIntfToneGenGenInitCB;
		pIntfCB->intf_tx_init_cb = openavbIntfToneGenTxInitCB;
		pIntfCB->intf_tx_cb = openavbIntfToneGenTxCB;
		pIntfCB->intf_rx_init_cb = openavbIntfToneGenRxInitCB;
		pIntfCB->intf_rx_cb = openavbIntfToneGenRxCB;
		pIntfCB->intf_end_cb = openavbIntfToneGenEndCB;
		pIntfCB->intf_gen_end_cb = openavbIntfToneGenGenEndCB;
		pIntfCB->intf_enable_fixed_timestamp = openavbIntfToneGenEnableFixedTimestamp;

		pPvtData->intervalCounter = 0;
		pPvtData->melodyIdx = 0;
		pPvtData->audioType = AVB_AUDIO_TYPE_INT;
		pPvtData->audioEndian = AVB_AUDIO_ENDIAN_BIG;

		pPvtData->volume = 1.0f;
		pPvtData->fv1Enabled = false;
		pPvtData->fv1 = 0;
		pPvtData->fv2Enabled = false;
		pPvtData->fv2 = 0;
		pPvtData->fvChannels = 0;

			pPvtData->fixedTimestampEnabled = false;
			pPvtData->runningFrameCnt = 0;

			pPvtData->jitterFifoEnabled = TRUE;
			pPvtData->jitterFifoItemCount = 64;
			pPvtData->jitterFifoPrefillItems = 16;
			pPvtData->jitterFifoLowWaterItems = 12;
			pPvtData->jitterFifoHighWaterItems = 48;
			pPvtData->jitterFifoLogIntervalSec = 5;
			pPvtData->jitterFifoInitialized = FALSE;
			pPvtData->jitterFifoThreadRunning = FALSE;
			pPvtData->jitterFifoStopRequested = FALSE;
			pPvtData->jitterFifoPrefillDone = FALSE;
			pPvtData->pJitterFifoItems = NULL;
			pPvtData->fifoTsRuntimeLeadUsec = 0;
		}

	AVB_TRACE_EXIT(AVB_TRACE_INTF);
	return TRUE;
}
