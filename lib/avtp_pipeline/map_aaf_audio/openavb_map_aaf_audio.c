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
 * MODULE SUMMARY : Implementation for AAF mapping module
 *
 * AAF (AVTP Audio Format) is defined in IEEE 1722-2016 Clause 7.
 */

#include <stdlib.h>
#include <string.h>
#include "openavb_mcr_hal_pub.h"
#include "openavb_osal_pub.h"
#include "openavb_types_pub.h"
#include "openavb_trace_pub.h"
#include "openavb_avtp_time_pub.h"
#include "openavb_mediaq_pub.h"
#include "openavb_map_pub.h"
#include "openavb_map_aaf_audio_pub.h"
#include "openavb_aem_types_pub.h"
#include "openavb_clock_source_runtime_pub.h"
#include "openavb_time_osal_pub.h"

#define	AVB_LOG_COMPONENT	"AAF Mapping"
#include "openavb_log_pub.h"

#define AVTP_SUBTYPE_AAF			2

// Header sizes (bytes)
#define AVTP_V0_HEADER_SIZE			12
#define AAF_HEADER_SIZE				12
#define TOTAL_HEADER_SIZE			(AVTP_V0_HEADER_SIZE + AAF_HEADER_SIZE)

// - 1 Byte - TV bit (timestamp valid)
#define HIDX_AVTP_HIDE7_TV1			1

// - 1 Byte - Sequence number
#define HIDX_AVTP_SEQ_NUM			2

// - 1 Byte - TU bit (timestamp uncertain)
#define HIDX_AVTP_HIDE7_TU1			3

// - 2 bytes	Stream data length
#define HIDX_STREAM_DATA_LEN16		20

// - 1 Byte - SP bit (sparse mode)
#define HIDX_AVTP_HIDE7_SP			22
#define SP_M0_BIT					(1 << 4)

typedef enum {
	AAF_RATE_UNSPEC = 0,
	AAF_RATE_8K,
	AAF_RATE_16K,
	AAF_RATE_32K,
	AAF_RATE_44K1,
	AAF_RATE_48K,
	AAF_RATE_88K2,
	AAF_RATE_96K,
	AAF_RATE_176K4,
	AAF_RATE_192K,
	AAF_RATE_24K,
} aaf_nominal_sample_rate_t;

typedef enum {
	AAF_FORMAT_UNSPEC = 0,
	AAF_FORMAT_FLOAT_32,
	AAF_FORMAT_INT_32,
	AAF_FORMAT_INT_24,
	AAF_FORMAT_INT_16,
	AAF_FORMAT_AES3_32, // AVDECC_TODO:  Implement this
} aaf_sample_format_t;

typedef enum {
	AAF_STATIC_CHANNELS_LAYOUT	= 0,
	AAF_MONO_CHANNELS_LAYOUT	= 1,
	AAF_STEREO_CHANNELS_LAYOUT	= 2,
	AAF_5_1_CHANNELS_LAYOUT		= 3,
	AAF_7_1_CHANNELS_LAYOUT		= 4,
	AAF_MAX_CHANNELS_LAYOUT		= 15,
} aaf_automotive_channels_layout_t;

typedef enum {
	// Disabled - timestamp is valid in every avtp packet
	TS_SPARSE_MODE_DISABLED		= 0,
	// Enabled - timestamp is valid in every 8th avtp packet
	TS_SPARSE_MODE_ENABLED		= 1
} avb_audio_sparse_mode_t;

typedef struct {
	/////////////
	// Config data
	/////////////
	// map_nv_item_count
	U32 itemCount;

	// Transmit interval in frames per second. 0 = default for talker class.
	U32 txInterval;

	// A multiple of how many frames of audio to accept in an media queue item and
	// into the AVTP payload above the minimal needed.
	U32 packingFactor;

	// MCR mode
	avb_audio_mcr_t audioMcr;

	// MCR timestamp interval
	U32 mcrTimestampInterval;

	// MCR clock recovery interval
	U32 mcrRecoveryInterval;

	/////////////
	// Variable data
	/////////////
	U32 maxTransitUsec;     // In microseconds

	aaf_nominal_sample_rate_t 	aaf_rate;
	aaf_sample_format_t			aaf_format;
	U8							aaf_bit_depth;
	U32 payloadSize;
	U32 payloadSizeMaxTalker, payloadSizeMaxListener;
	bool isTalker;

	U8 aaf_event_field;

	bool dataValid;

	U32 intervalCounter;
	U32 txLeadLogEveryPackets;
	U32 txLeadLogCounter;
	U32 txMinLeadUsec;
	bool txDisableNetworkSwap;
	S32 txLaunchSkewUsec;
	S32 selectedClockTrimUsec;
	U32 selectedClockWarmupUsec;
	U32 selectedClockMuteUsec;

	avb_audio_sparse_mode_t sparseMode;

	bool mediaQItemSyncTS;
	bool usingSelectedInputClock;
	U32 selectedClockGeneration;
	U32 selectedCrfGeneration;
	U32 selectedClockRecoveryGeneration;
	U32 selectedClockPendingGeneration;
	bool selectedClockPendingLogged;
	bool selectedClockCadenceValid;
	bool selectedClockCadenceUsingMediaClock;
	bool selectedClockPreferLocalMediaPhase;
	bool useSelectedClockTimestamps;
	bool itemTimestampIsPresentation;
	U64 selectedClockCadenceBaseNs;
	U32 selectedClockCadenceGeneration;
	U32 selectedClockCadenceCrfGeneration;
	bool selectedClockPresentationValid;
	U64 selectedClockPresentationBaseNs;
	S64 selectedClockPresentationOffsetNs;
	U32 selectedClockPresentationOffsetUsec;
	bool selectedClockFollowUpdates;
	bool selectedClockWarmupActive;
	bool selectedClockWarmupLogged;
	U64 selectedClockWarmupUntilNs;
	U64 selectedClockWarmupDropCount;
	bool selectedClockMuteActive;
	bool selectedClockMuteLogged;
	U64 selectedClockMuteUntilNs;
	U64 selectedClockMutePacketCount;
	U64 txCadenceSlewEventCount;
	U64 txCadenceHardRebaseCount;
	U64 txPresentationSlewEventCount;
	U64 txPresentationHardRebaseCount;

	// TX timestamp continuity diagnostics.
	bool txDiagPrevPacketTsValid;
	U64 txDiagPrevPacketTsNs;
	U64 txDiagPrevItemBaseNs;
	U64 txDiagPrevSourceBaseNs;
	U64 txDiagPrevCadenceBaseNs;
	U64 txDiagPrevLaunchTimeNs;
	U32 txDiagPrevReadIdx;
	U32 txDiagPrevPacketIndex;
	U32 txDiagPrevClockGeneration;
	U32 txDiagPrevCrfGeneration;
	U64 txDiagPacketCount;
	U64 txDiagAnomalyCount;
	bool txItem0PrevValid;
	U64 txItem0PrevItemBaseNs;
	U64 txItem0PrevSourceBaseNs;
	U64 txItem0PrevCadenceBaseNs;
	U64 txItem0PrevLaunchTimeNs;
	U64 txItem0PrevPacketTsNs;
	U32 txItem0PrevReadIdx;
	U32 txItem0PrevClockGeneration;
	U32 txItem0PrevCrfGeneration;
	U64 txItem0DiagCount;
	U64 txPacket0BuildDiagCount;

	// TX phase diagnostics against the currently selected stream/media clock.
	U32 txPhaseDiagEveryPackets;
	U64 txPhaseDiagPacketCounter;
	bool txPhaseDiagWindowValid;
	S64 txPhaseDiagItemToSourceMinNs;
	S64 txPhaseDiagItemToSourceMaxNs;
	S64 txPhaseDiagItemToSourceSumNs;
	S64 txPhaseDiagItemToSourceAbsMaxNs;
	S64 txPhaseDiagSourceToCadenceMinNs;
	S64 txPhaseDiagSourceToCadenceMaxNs;
	S64 txPhaseDiagSourceToCadenceSumNs;
	S64 txPhaseDiagSourceToCadenceAbsMaxNs;
	U64 txPhaseDiagClampMaxNs;
	U64 txPhaseDiagWindowPackets;
	U64 txPhaseDiagLogCount;
	U32 txPhaseDiagSourceGeneration;
	bool txPhaseDiagUsingLocalMediaClock;

	// TX launch-time diagnostics at the mapper boundary before AVTP/rawsock.
	U32 txLaunchDiagEveryPackets;
	U64 txLaunchDiagPacketCounter;
	U64 txLaunchDiagLogCount;
	U64 txLaunchStaleCount;

#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
	U64 lastLaunchTimeNs;
	bool lastLaunchTimeValid;
#endif

} pvt_data_t;

static void mapAafResetTxPhaseDiag(pvt_data_t *pPvtData)
{
	if (!pPvtData) {
		return;
	}

	pPvtData->txPhaseDiagPacketCounter = 0;
	pPvtData->txPhaseDiagWindowValid = FALSE;
	pPvtData->txPhaseDiagItemToSourceMinNs = 0;
	pPvtData->txPhaseDiagItemToSourceMaxNs = 0;
	pPvtData->txPhaseDiagItemToSourceSumNs = 0;
	pPvtData->txPhaseDiagItemToSourceAbsMaxNs = 0;
	pPvtData->txPhaseDiagSourceToCadenceMinNs = 0;
	pPvtData->txPhaseDiagSourceToCadenceMaxNs = 0;
	pPvtData->txPhaseDiagSourceToCadenceSumNs = 0;
	pPvtData->txPhaseDiagSourceToCadenceAbsMaxNs = 0;
	pPvtData->txPhaseDiagClampMaxNs = 0;
	pPvtData->txPhaseDiagWindowPackets = 0;
	pPvtData->txPhaseDiagLogCount = 0;
	pPvtData->txPhaseDiagSourceGeneration = 0;
	pPvtData->txPhaseDiagUsingLocalMediaClock = FALSE;
}

static void mapAafResetTxItem0Diag(pvt_data_t *pPvtData)
{
	if (!pPvtData) {
		return;
	}

	pPvtData->txItem0PrevValid = FALSE;
	pPvtData->txItem0PrevItemBaseNs = 0;
	pPvtData->txItem0PrevSourceBaseNs = 0;
	pPvtData->txItem0PrevCadenceBaseNs = 0;
	pPvtData->txItem0PrevLaunchTimeNs = 0;
	pPvtData->txItem0PrevPacketTsNs = 0;
	pPvtData->txItem0PrevReadIdx = 0;
	pPvtData->txItem0PrevClockGeneration = 0;
	pPvtData->txItem0PrevCrfGeneration = 0;
	pPvtData->txItem0DiagCount = 0;
}

static bool mapAafPayloadNeedsNetworkSwap(const media_q_pub_map_aaf_audio_info_t *pPubMapInfo,
					      const pvt_data_t *pPvtData)
{
	if (!pPubMapInfo) {
		return FALSE;
	}

	if (pPvtData && pPvtData->txDisableNetworkSwap) {
		return FALSE;
	}

	if (pPubMapInfo->itemSampleSizeBytes <= 1) {
		return FALSE;
	}

	return (pPubMapInfo->audioEndian == AVB_AUDIO_ENDIAN_LITTLE);
}

static void mapAafCopyPayloadToNetworkOrder(U8 *pDst,
							const U8 *pSrc,
							U32 payloadSize,
							const media_q_pub_map_aaf_audio_info_t *pPubMapInfo,
							const pvt_data_t *pPvtData)
{
	U32 sampleSize;
	U32 idx;

	if (!pDst || !pSrc || payloadSize == 0 || !pPubMapInfo) {
		return;
	}

	sampleSize = pPubMapInfo->itemSampleSizeBytes;
	if (!mapAafPayloadNeedsNetworkSwap(pPubMapInfo, pPvtData)
			|| sampleSize <= 1
			|| (payloadSize % sampleSize) != 0) {
		memcpy(pDst, pSrc, payloadSize);
		return;
	}

	switch (sampleSize) {
		case 2:
			for (idx = 0; idx < payloadSize; idx += 2) {
				pDst[idx + 0] = pSrc[idx + 1];
				pDst[idx + 1] = pSrc[idx + 0];
			}
			break;
		case 3:
			for (idx = 0; idx < payloadSize; idx += 3) {
				pDst[idx + 0] = pSrc[idx + 2];
				pDst[idx + 1] = pSrc[idx + 1];
				pDst[idx + 2] = pSrc[idx + 0];
			}
			break;
		case 4:
			for (idx = 0; idx < payloadSize; idx += 4) {
				pDst[idx + 0] = pSrc[idx + 3];
				pDst[idx + 1] = pSrc[idx + 2];
				pDst[idx + 2] = pSrc[idx + 1];
				pDst[idx + 3] = pSrc[idx + 0];
			}
			break;
		default:
			memcpy(pDst, pSrc, payloadSize);
			break;
	}
}

static void mapAafUpdateTxPhaseDiag(
	media_q_t *pMediaQ,
	pvt_data_t *pPvtData,
	U64 itemPacketTsNs,
	U64 sourcePacketTsNs,
	U64 cadencePacketTsNs,
	U64 clampDeltaNs,
	const openavb_clock_source_runtime_t *pClockSelection,
	U32 crfGeneration,
	bool usingLocalMediaClock)
{
	if (!pMediaQ || !pPvtData || !pClockSelection) {
		return;
	}

	if (!pPvtData->txPhaseDiagWindowValid ||
			pPvtData->txPhaseDiagSourceGeneration != pClockSelection->generation ||
			pPvtData->txPhaseDiagUsingLocalMediaClock != usingLocalMediaClock) {
		pPvtData->txPhaseDiagWindowValid = TRUE;
		pPvtData->txPhaseDiagItemToSourceMinNs = 0;
		pPvtData->txPhaseDiagItemToSourceMaxNs = 0;
		pPvtData->txPhaseDiagItemToSourceSumNs = 0;
		pPvtData->txPhaseDiagItemToSourceAbsMaxNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceMinNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceMaxNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceSumNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceAbsMaxNs = 0;
		pPvtData->txPhaseDiagClampMaxNs = 0;
		pPvtData->txPhaseDiagWindowPackets = 0;
		pPvtData->txPhaseDiagSourceGeneration = pClockSelection->generation;
		pPvtData->txPhaseDiagUsingLocalMediaClock = usingLocalMediaClock;
	}

	S64 itemToSourceNs = (sourcePacketTsNs >= itemPacketTsNs)
		? (S64)(sourcePacketTsNs - itemPacketTsNs)
		: -((S64)(itemPacketTsNs - sourcePacketTsNs));
	S64 sourceToCadenceNs = (cadencePacketTsNs >= sourcePacketTsNs)
		? (S64)(cadencePacketTsNs - sourcePacketTsNs)
		: -((S64)(sourcePacketTsNs - cadencePacketTsNs));
	S64 itemToSourceAbsNs = llabs(itemToSourceNs);
	S64 sourceToCadenceAbsNs = llabs(sourceToCadenceNs);

	if (pPvtData->txPhaseDiagWindowPackets == 0) {
		pPvtData->txPhaseDiagItemToSourceMinNs = itemToSourceNs;
		pPvtData->txPhaseDiagItemToSourceMaxNs = itemToSourceNs;
		pPvtData->txPhaseDiagSourceToCadenceMinNs = sourceToCadenceNs;
		pPvtData->txPhaseDiagSourceToCadenceMaxNs = sourceToCadenceNs;
	}
	else {
		if (itemToSourceNs < pPvtData->txPhaseDiagItemToSourceMinNs) {
			pPvtData->txPhaseDiagItemToSourceMinNs = itemToSourceNs;
		}
		if (itemToSourceNs > pPvtData->txPhaseDiagItemToSourceMaxNs) {
			pPvtData->txPhaseDiagItemToSourceMaxNs = itemToSourceNs;
		}
		if (sourceToCadenceNs < pPvtData->txPhaseDiagSourceToCadenceMinNs) {
			pPvtData->txPhaseDiagSourceToCadenceMinNs = sourceToCadenceNs;
		}
		if (sourceToCadenceNs > pPvtData->txPhaseDiagSourceToCadenceMaxNs) {
			pPvtData->txPhaseDiagSourceToCadenceMaxNs = sourceToCadenceNs;
		}
	}

	pPvtData->txPhaseDiagItemToSourceSumNs += itemToSourceNs;
	pPvtData->txPhaseDiagSourceToCadenceSumNs += sourceToCadenceNs;
	if (itemToSourceAbsNs > pPvtData->txPhaseDiagItemToSourceAbsMaxNs) {
		pPvtData->txPhaseDiagItemToSourceAbsMaxNs = itemToSourceAbsNs;
	}
	if (sourceToCadenceAbsNs > pPvtData->txPhaseDiagSourceToCadenceAbsMaxNs) {
		pPvtData->txPhaseDiagSourceToCadenceAbsMaxNs = sourceToCadenceAbsNs;
	}
	if (clampDeltaNs > pPvtData->txPhaseDiagClampMaxNs) {
		pPvtData->txPhaseDiagClampMaxNs = clampDeltaNs;
	}
	pPvtData->txPhaseDiagWindowPackets++;
	pPvtData->txPhaseDiagPacketCounter++;

	U32 logEveryPackets = pPvtData->txPhaseDiagEveryPackets;
	if (logEveryPackets == 0) {
		logEveryPackets = (pPvtData->txInterval > 0) ? pPvtData->txInterval : 4000;
	}

	if (logEveryPackets > 0 &&
			(pPvtData->txPhaseDiagPacketCounter % logEveryPackets) == 0 &&
			pPvtData->txPhaseDiagWindowPackets > 0) {
		S64 itemToSourceAvgNs =
			pPvtData->txPhaseDiagItemToSourceSumNs / (S64)pPvtData->txPhaseDiagWindowPackets;
		S64 sourceToCadenceAvgNs =
			pPvtData->txPhaseDiagSourceToCadenceSumNs / (S64)pPvtData->txPhaseDiagWindowPackets;

		pPvtData->txPhaseDiagLogCount++;
		AVB_LOGF_WARNING(
			"TX OUTLIER ROW flags=.Y. stage=aaf_phase stream=%u i2s_avg=%lldns i2s_min=%lldns i2s_max=%lldns i2s_abs=%lldns s2c_avg=%lldns s2c_min=%lldns s2c_max=%lldns s2c_abs=%lldns clamp_max=%lluns packets=%llu source=%s gen=%u crf_gen=%u windows=%llu",
			pMediaQ->debug_stream_uid,
			(long long)itemToSourceAvgNs,
			(long long)pPvtData->txPhaseDiagItemToSourceMinNs,
			(long long)pPvtData->txPhaseDiagItemToSourceMaxNs,
			(long long)pPvtData->txPhaseDiagItemToSourceAbsMaxNs,
			(long long)sourceToCadenceAvgNs,
			(long long)pPvtData->txPhaseDiagSourceToCadenceMinNs,
			(long long)pPvtData->txPhaseDiagSourceToCadenceMaxNs,
			(long long)pPvtData->txPhaseDiagSourceToCadenceAbsMaxNs,
			(unsigned long long)pPvtData->txPhaseDiagClampMaxNs,
			(unsigned long long)pPvtData->txPhaseDiagWindowPackets,
			usingLocalMediaClock ? "local_media" : "projected_crf",
			pClockSelection->generation,
			crfGeneration,
			(unsigned long long)pPvtData->txPhaseDiagLogCount);

		pPvtData->txPhaseDiagItemToSourceMinNs = 0;
		pPvtData->txPhaseDiagItemToSourceMaxNs = 0;
		pPvtData->txPhaseDiagItemToSourceSumNs = 0;
		pPvtData->txPhaseDiagItemToSourceAbsMaxNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceMinNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceMaxNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceSumNs = 0;
		pPvtData->txPhaseDiagSourceToCadenceAbsMaxNs = 0;
		pPvtData->txPhaseDiagClampMaxNs = 0;
		pPvtData->txPhaseDiagWindowPackets = 0;
	}
}

static void x_calculateSizes(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);

	if (pMediaQ) {
		media_q_pub_map_aaf_audio_info_t *pPubMapInfo = pMediaQ->pPubMapInfo;
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return;
		}

		switch (pPubMapInfo->audioRate) {
			case AVB_AUDIO_RATE_8KHZ:
				pPvtData->aaf_rate = AAF_RATE_8K;
				break;
			case AVB_AUDIO_RATE_16KHZ:
				pPvtData->aaf_rate = AAF_RATE_16K;
				break;
			case AVB_AUDIO_RATE_24KHZ:
				pPvtData->aaf_rate = AAF_RATE_24K;
				break;
			case AVB_AUDIO_RATE_32KHZ:
				pPvtData->aaf_rate = AAF_RATE_32K;
				break;
			case AVB_AUDIO_RATE_44_1KHZ:
				pPvtData->aaf_rate = AAF_RATE_44K1;
				break;
			case AVB_AUDIO_RATE_48KHZ:
				pPvtData->aaf_rate = AAF_RATE_48K;
				break;
			case AVB_AUDIO_RATE_88_2KHZ:
				pPvtData->aaf_rate = AAF_RATE_88K2;
				break;
			case AVB_AUDIO_RATE_96KHZ:
				pPvtData->aaf_rate = AAF_RATE_96K;
				break;
			case AVB_AUDIO_RATE_176_4KHZ:
				pPvtData->aaf_rate = AAF_RATE_176K4;
				break;
			case AVB_AUDIO_RATE_192KHZ:
				pPvtData->aaf_rate = AAF_RATE_192K;
				break;
			default:
				AVB_LOG_ERROR("Invalid audio frequency configured");
				pPvtData->aaf_rate = AAF_RATE_UNSPEC;
				break;
		}
		AVB_LOGF_INFO("aaf_rate=%d (%dKhz)", pPvtData->aaf_rate, pPubMapInfo->audioRate);

		char *typeStr = "int";
		if (pPubMapInfo->audioType == AVB_AUDIO_TYPE_FLOAT) {
			typeStr = "float";
			switch (pPubMapInfo->audioBitDepth) {
				case AVB_AUDIO_BIT_DEPTH_32BIT:
					pPvtData->aaf_format = AAF_FORMAT_FLOAT_32;
					pPubMapInfo->itemSampleSizeBytes = 4;
					pPubMapInfo->packetSampleSizeBytes = 4;
					pPvtData->aaf_bit_depth = 32;
					break;
				default:
					AVB_LOG_ERROR("Invalid audio bit-depth configured for float");
					pPvtData->aaf_format = AAF_FORMAT_UNSPEC;
					break;
			}
		}
		else {
			switch (pPubMapInfo->audioBitDepth) {
				case AVB_AUDIO_BIT_DEPTH_32BIT:
					pPvtData->aaf_format = AAF_FORMAT_INT_32;
					pPubMapInfo->itemSampleSizeBytes = 4;
					pPubMapInfo->packetSampleSizeBytes = 4;
					pPvtData->aaf_bit_depth = 32;
					break;
				case AVB_AUDIO_BIT_DEPTH_24BIT:
					pPvtData->aaf_format = AAF_FORMAT_INT_24;
					pPubMapInfo->itemSampleSizeBytes = 3;
					pPubMapInfo->packetSampleSizeBytes = 3;
					pPvtData->aaf_bit_depth = 24;
					break;
				case AVB_AUDIO_BIT_DEPTH_16BIT:
					pPvtData->aaf_format = AAF_FORMAT_INT_16;
					pPubMapInfo->itemSampleSizeBytes = 2;
					pPubMapInfo->packetSampleSizeBytes = 2;
					pPvtData->aaf_bit_depth = 16;
					break;
#if 0
					// should work - test content?
				case AVB_AUDIO_BIT_DEPTH_20BIT:
					pPvtData->aaf_format = AAF_FORMAT_INT_24;
					pPubMapInfo->itemSampleSizeBytes = 3;
					pPubMapInfo->packetSampleSizeBytes = 3;
					pPvtData->aaf_bit_depth = 20;
					break;
					// would require byte-by-byte copy
				case AVB_AUDIO_BIT_DEPTH_8BIT:
					pPvtData->aaf_format = AAF_FORMAT_INT_24;
					pPubMapInfo->itemSampleSizeBytes = 1;
					pPubMapInfo->packetSampleSizeBytes = 2;
					pPvtData->aaf_bit_depth = 8;
					break;
#endif
				default:
					AVB_LOG_ERROR("Invalid audio bit-depth configured");
					pPvtData->aaf_format = AAF_FORMAT_UNSPEC;
					break;
			}
		}
		AVB_LOGF_INFO("aaf_format=%d (%s%d)",
			pPvtData->aaf_format, typeStr, pPubMapInfo->audioBitDepth);

		// Audio frames per packet
		pPubMapInfo->framesPerPacket = (pPubMapInfo->audioRate / pPvtData->txInterval);
		if (pPubMapInfo->audioRate % pPvtData->txInterval != 0) {
			AVB_LOGF_WARNING("Audio rate (%d) is not integer multiple of TX interval (%d)",
				pPubMapInfo->audioRate, pPvtData->txInterval);
			pPubMapInfo->framesPerPacket += 1;
		}
		AVB_LOGF_INFO("Frames/packet = %d", pPubMapInfo->framesPerPacket);

		// AAF packet size calculations
		pPubMapInfo->packetFrameSizeBytes = pPubMapInfo->packetSampleSizeBytes * pPubMapInfo->audioChannels;
		pPvtData->payloadSize = pPvtData->payloadSizeMaxTalker = pPvtData->payloadSizeMaxListener =
			pPubMapInfo->framesPerPacket * pPubMapInfo->packetFrameSizeBytes;
		AVB_LOGF_INFO("packet: sampleSz=%d * channels=%d => frameSz=%d * %d => payloadSz=%d",
			pPubMapInfo->packetSampleSizeBytes,
			pPubMapInfo->audioChannels,
			pPubMapInfo->packetFrameSizeBytes,
			pPubMapInfo->framesPerPacket,
			pPvtData->payloadSize);
		if (pPvtData->aaf_format >= AAF_FORMAT_INT_32 && pPvtData->aaf_format <= AAF_FORMAT_INT_16) {
			// Determine the largest size we could receive before adjustments.
			pPvtData->payloadSizeMaxListener = 4 * pPubMapInfo->audioChannels * pPubMapInfo->framesPerPacket;
			AVB_LOGF_DEBUG("packet: payloadSizeMaxListener=%d", pPvtData->payloadSizeMaxListener);
		}

		// MediaQ item size calculations
		pPubMapInfo->packingFactor = pPvtData->packingFactor;
		pPubMapInfo->framesPerItem = pPubMapInfo->framesPerPacket * pPvtData->packingFactor;
		pPubMapInfo->itemFrameSizeBytes = pPubMapInfo->itemSampleSizeBytes * pPubMapInfo->audioChannels;
		pPubMapInfo->itemSize = pPubMapInfo->itemFrameSizeBytes * pPubMapInfo->framesPerItem;
		AVB_LOGF_INFO("item: sampleSz=%d * channels=%d => frameSz=%d * %d * packing=%d => itemSz=%d",
			pPubMapInfo->itemSampleSizeBytes,
			pPubMapInfo->audioChannels,
			pPubMapInfo->itemFrameSizeBytes,
			pPubMapInfo->framesPerPacket,
			pPubMapInfo->packingFactor,
			pPubMapInfo->itemSize);
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}


// Each configuration name value pair for this mapping will result in this callback being called.
void openavbMapAVTPAudioCfgCB(media_q_t *pMediaQ, const char *name, const char *value)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);

	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return;
		}

		if (strcmp(name, "map_nv_item_count") == 0) {
			char *pEnd;
			pPvtData->itemCount = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_packing_factor") == 0) {
			char *pEnd;
			pPvtData->packingFactor = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_tx_rate") == 0
			|| strcmp(name, "map_nv_tx_interval") == 0) {
			char *pEnd;
			pPvtData->txInterval = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_sparse_mode") == 0) {
			char* pEnd;
			U32 tmp;
			tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && tmp == 1) {
				pPvtData->sparseMode = TS_SPARSE_MODE_ENABLED;
			}
			else if (*pEnd == '\0' && tmp == 0) {
				pPvtData->sparseMode = TS_SPARSE_MODE_DISABLED;
			}
		}
		else if (strcmp(name, "map_nv_audio_mcr") == 0) {
			char *pEnd;
			pPvtData->audioMcr = (avb_audio_mcr_t)strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_mcr_timestamp_interval") == 0) {
			char *pEnd;
			pPvtData->mcrTimestampInterval = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_mcr_recovery_interval") == 0) {
			char *pEnd;
			pPvtData->mcrRecoveryInterval = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_tx_lead_log_every") == 0) {
			char *pEnd;
			pPvtData->txLeadLogEveryPackets = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_tx_phase_log_every") == 0 ||
				strcmp(name, "map_nv_tx_phase_diag_every") == 0) {
			char *pEnd;
			pPvtData->txPhaseDiagEveryPackets = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_tx_min_lead_usec") == 0) {
			char *pEnd;
			pPvtData->txMinLeadUsec = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_disable_network_swap") == 0 ||
				strcmp(name, "map_nv_tx_disable_network_swap") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0') {
				pPvtData->txDisableNetworkSwap = (tmp != 0);
			}
		}
		else if (strcmp(name, "map_nv_tx_launch_skew_usec") == 0 ||
				strcmp(name, "map_nv_launch_skew_usec") == 0) {
			char *pEnd;
			pPvtData->txLaunchSkewUsec = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_selected_clock_trim_usec") == 0 ||
				strcmp(name, "map_nv_clock_trim_usec") == 0) {
			char *pEnd;
			pPvtData->selectedClockTrimUsec = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_selected_clock_follow_updates") == 0 ||
				strcmp(name, "map_nv_clock_follow_selected_stream") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0') {
				pPvtData->selectedClockFollowUpdates = (tmp != 0);
			}
		}
		else if (strcmp(name, "map_nv_selected_clock_prefer_local_media_phase") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0') {
				pPvtData->selectedClockPreferLocalMediaPhase = (tmp != 0);
			}
		}
		else if (strcmp(name, "map_nv_use_selected_clock_timestamps") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0') {
				pPvtData->useSelectedClockTimestamps = (tmp != 0);
			}
		}
		else if (strcmp(name, "map_nv_item_timestamp_is_presentation") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0') {
				pPvtData->itemTimestampIsPresentation = (tmp != 0);
			}
		}
		else if (strcmp(name, "map_nv_selected_clock_presentation_offset_usec") == 0 ||
				strcmp(name, "map_nv_clock_presentation_offset_usec") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0' && tmp >= 0) {
				pPvtData->selectedClockPresentationOffsetUsec = (U32)tmp;
			}
		}
		else if (strcmp(name, "map_nv_selected_clock_warmup_usec") == 0 ||
				strcmp(name, "map_nv_clock_warmup_usec") == 0) {
			char *pEnd;
			pPvtData->selectedClockWarmupUsec = strtol(value, &pEnd, 10);
		}
		else if (strcmp(name, "map_nv_selected_clock_mute_usec") == 0 ||
				strcmp(name, "map_nv_clock_mute_usec") == 0) {
			char *pEnd;
			pPvtData->selectedClockMuteUsec = strtol(value, &pEnd, 10);
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

U8 openavbMapAVTPAudioSubtypeCB()
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return AVTP_SUBTYPE_AAF;        // AAF AVB subtype
}

// Returns the AVTP version used by this mapping
U8 openavbMapAVTPAudioAvtpVersionCB()
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);
	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return 0x00;        // Version 0
}

U16 openavbMapAVTPAudioMaxDataSizeCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return 0;
		}

		// Return the largest size a frame payload could be.
		// If we don't yet know if we are a Talker or Listener, the larger Listener max will be returned.
		U16 payloadSizeMax;
		if (pPvtData->isTalker) {
			payloadSizeMax = pPvtData->payloadSizeMaxTalker + TOTAL_HEADER_SIZE;
		}
		else {
			payloadSizeMax = pPvtData->payloadSizeMaxListener + TOTAL_HEADER_SIZE;
		}
		AVB_TRACE_EXIT(AVB_TRACE_MAP);
		return payloadSizeMax;
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return 0;
}

// Returns the intended transmit interval (in frames per second). 0 = default for talker / class.
U32 openavbMapAVTPAudioTransmitIntervalCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return 0;
		}

		AVB_TRACE_EXIT(AVB_TRACE_MAP);
		return pPvtData->txInterval;
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return 0;
}

void openavbMapAVTPAudioGenInitCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ) {
		media_q_pub_map_uncmp_audio_info_t *pPubMapInfo = pMediaQ->pPubMapInfo;
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return;
		}

		x_calculateSizes(pMediaQ);
		openavbMediaQSetSize(pMediaQ, pPvtData->itemCount, pPubMapInfo->itemSize);

		pPvtData->dataValid = TRUE;
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

// A call to this callback indicates that this mapping module will be
// a talker. Any talker initialization can be done in this function.
void openavbMapAVTPAudioTxInitCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		media_q_pub_map_aaf_audio_info_t *pPubMapInfo = pMediaQ->pPubMapInfo;
		if (pPvtData) {
			pPvtData->isTalker = TRUE;
			if (pPubMapInfo) {
				AVB_LOGF_INFO(
					"AAF TX payload byte order: mediaQ_endian=%s sample_bytes=%u network_swap=%s",
					(pPubMapInfo->audioEndian == AVB_AUDIO_ENDIAN_BIG) ? "big" : "little",
					pPubMapInfo->itemSampleSizeBytes,
					mapAafPayloadNeedsNetworkSwap(pPubMapInfo, pPvtData) ? "enabled" : "bypassed");
			}
			pPvtData->selectedClockPendingGeneration = 0;
			pPvtData->selectedClockPendingLogged = FALSE;
			pPvtData->selectedClockCadenceValid = FALSE;
			pPvtData->selectedClockCadenceUsingMediaClock = FALSE;
			pPvtData->selectedClockCadenceBaseNs = 0;
			pPvtData->selectedClockCadenceGeneration = 0;
			pPvtData->selectedClockCadenceCrfGeneration = 0;
			pPvtData->selectedClockPresentationValid = FALSE;
			pPvtData->selectedClockPresentationBaseNs = 0;
			pPvtData->selectedClockPresentationOffsetNs = 0;
			pPvtData->selectedClockWarmupActive = FALSE;
			pPvtData->selectedClockWarmupLogged = FALSE;
			pPvtData->selectedClockWarmupUntilNs = 0;
			pPvtData->selectedClockWarmupDropCount = 0;
			pPvtData->selectedClockMuteActive = FALSE;
			pPvtData->selectedClockMuteLogged = FALSE;
			pPvtData->selectedClockMuteUntilNs = 0;
			pPvtData->selectedClockMutePacketCount = 0;
			pPvtData->txCadenceSlewEventCount = 0;
			pPvtData->txCadenceHardRebaseCount = 0;
			pPvtData->txDiagPrevPacketTsValid = FALSE;
			pPvtData->txDiagPrevPacketTsNs = 0;
			pPvtData->txDiagPrevItemBaseNs = 0;
			pPvtData->txDiagPrevSourceBaseNs = 0;
			pPvtData->txDiagPrevCadenceBaseNs = 0;
			pPvtData->txDiagPrevLaunchTimeNs = 0;
			pPvtData->txDiagPrevReadIdx = 0;
			pPvtData->txDiagPrevPacketIndex = 0;
			pPvtData->txDiagPrevClockGeneration = 0;
			pPvtData->txDiagPrevCrfGeneration = 0;
			mapAafResetTxItem0Diag(pPvtData);
			pPvtData->txDiagPacketCount = 0;
			pPvtData->txDiagAnomalyCount = 0;
			pPvtData->txLaunchDiagEveryPackets = 0;
			pPvtData->txLaunchDiagPacketCounter = 0;
			pPvtData->txLaunchDiagLogCount = 0;
			pPvtData->txLaunchStaleCount = 0;
#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
			pPvtData->lastLaunchTimeNs = 0;
			pPvtData->lastLaunchTimeValid = FALSE;
#endif
			mapAafResetTxPhaseDiag(pPvtData);
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

static bool mapAafGetSelectedStreamClock(
	U64 *pClockNs,
	bool *pClockUncertain,
	openavb_clock_source_runtime_t *pSelection,
	U32 *pCrfGeneration,
	bool preferLocalMediaClockPhase,
	bool *pUsingLocalMediaClock)
{
	openavb_clock_source_runtime_t selection = {0};
	U64 clockNs = 0;
	bool clockUncertain = FALSE;
	U32 crfGeneration = 0;
	bool usingLocalMediaClock = FALSE;

	if (!openavbClockSourceRuntimeGetSelection(&selection)) {
		return FALSE;
	}

	if ((selection.clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID) == 0) {
		return FALSE;
	}

	if (selection.clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT &&
			selection.clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		return FALSE;
	}

	// Prefer a transport-free media-clock phase anchor when the selected source
	// publishes one locally (e.g. our CRF talker feeding our AAF talkers). This
	// keeps CRF and AAF on the same underlying clock timeline instead of deriving
	// AAF from already offset CRF packet timestamps.
	if (preferLocalMediaClockPhase) {
		if (openavbClockSourceRuntimeGetMediaClockForLocation(
				selection.clock_source_location_type,
				selection.clock_source_location_index,
				&clockNs,
				&clockUncertain,
				&crfGeneration)) {
			usingLocalMediaClock = TRUE;
			if (pClockNs) {
				*pClockNs = clockNs;
			}
			if (pClockUncertain) {
				*pClockUncertain = clockUncertain;
			}
			if (pSelection) {
				*pSelection = selection;
			}
			if (pCrfGeneration) {
				*pCrfGeneration = crfGeneration;
			}
			if (pUsingLocalMediaClock) {
				*pUsingLocalMediaClock = usingLocalMediaClock;
			}
			return TRUE;
		}

		/*
		 * For direct/shared-media-clock talkers, "prefer local media phase"
		 * means exactly that. If the selected source is not publishing a
		 * local media-clock phase yet, stay on the interface-provided media
		 * timestamp instead of silently falling back to projected CRF time.
		 * Projected CRF fallback reintroduces transport-phase jitter into the
		 * AAF packet cadence and defeats the point of the direct shared clock.
		 */
		return FALSE;
	}

	if (!openavbClockSourceRuntimeGetCrfTimeForLocation(
			selection.clock_source_location_type,
			selection.clock_source_location_index,
			&clockNs,
			&clockUncertain,
			&crfGeneration)) {
		return FALSE;
	}

	if (pClockNs) {
		*pClockNs = clockNs;
	}
	if (pClockUncertain) {
		*pClockUncertain = clockUncertain;
	}
	if (pSelection) {
		*pSelection = selection;
	}
	if (pCrfGeneration) {
		*pCrfGeneration = crfGeneration;
	}
	if (pUsingLocalMediaClock) {
		*pUsingLocalMediaClock = usingLocalMediaClock;
	}
	return TRUE;
}

static bool mapAafSelectedStreamClockPending(openavb_clock_source_runtime_t *pSelection, bool preferLocalMediaClockPhase)
{
	openavb_clock_source_runtime_t selection = {0};
	U64 ignoredNs = 0;
	bool ignoredUncertain = FALSE;
	U32 ignoredGeneration = 0;

	if (!openavbClockSourceRuntimeGetSelection(&selection)) {
		return FALSE;
	}

	if ((selection.clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID) == 0) {
		return FALSE;
	}

	if (selection.clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT &&
			selection.clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
		return FALSE;
	}

	if (preferLocalMediaClockPhase) {
		if (openavbClockSourceRuntimeGetMediaClockForLocation(
				selection.clock_source_location_type,
				selection.clock_source_location_index,
				&ignoredNs,
				&ignoredUncertain,
				&ignoredGeneration)) {
			return FALSE;
		}

		/*
		 * Milan audio/clocking expects the AAF talker to follow the selected
		 * media clock domain, not to free-run on an interface-local
		 * presentation timeline until the selected clock becomes available.
		 *
		 * If selected-clock mode prefers a local published media phase
		 * (e.g. the shared CRF-derived output clock), keep the talker pending
		 * until that phase is available instead of silently falling back to
		 * interface timestamps.
		 */
		if (pSelection) {
			*pSelection = selection;
		}
		return TRUE;
	}

	if (openavbClockSourceRuntimeGetCrfTimeForLocation(
			selection.clock_source_location_type,
			selection.clock_source_location_index,
			&ignoredNs,
			&ignoredUncertain,
			&ignoredGeneration)) {
		return FALSE;
	}

	if (pSelection) {
		*pSelection = selection;
	}
	return TRUE;
}

// CORE_TODO: This callback should be updated to work in a similar way the uncompressed audio mapping. With allowing AVTP packets to be built
//  from multiple media queue items. This allows interface to set into the media queue blocks of audio frames to properly correspond to
//  a SYT_INTERVAL. Additionally the public data member sytInterval needs to be set in the same way the uncompressed audio mapping does.
// This talker callback will be called for each AVB observation interval.
tx_cb_ret_t openavbMapAVTPAudioTxCB(media_q_t *pMediaQ, U8 *pData, U32 *dataLen)
{
	media_q_item_t *pMediaQItem = NULL;
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);

	if (!pMediaQ) {
		AVB_LOG_ERROR("Mapping module invalid MediaQ");
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	if (!pData || !dataLen) {
		AVB_LOG_ERROR("Mapping module data or data length argument incorrect.");
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	media_q_pub_map_aaf_audio_info_t *pPubMapInfo = pMediaQ->pPubMapInfo;

	U32 bytesNeeded = pPubMapInfo->itemFrameSizeBytes * pPubMapInfo->framesPerPacket;
	if (!openavbMediaQIsAvailableBytes(pMediaQ, pPubMapInfo->itemFrameSizeBytes * pPubMapInfo->framesPerPacket, TRUE)) {
		AVB_LOG_VERBOSE("Not enough bytes are ready");
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
	if (!pPvtData) {
		AVB_LOG_ERROR("Private mapping module data not allocated.");
		openavbMediaQTailUnlock(pMediaQ);
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	openavb_clock_source_runtime_t pendingSelection = {0};
	if (pPvtData->useSelectedClockTimestamps &&
			mapAafSelectedStreamClockPending(&pendingSelection, pPvtData->selectedClockPreferLocalMediaPhase)) {
		if (!pPvtData->selectedClockPendingLogged ||
				pPvtData->selectedClockPendingGeneration != pendingSelection.generation) {
			AVB_LOGF_INFO(
				"AAF TX waiting for selected stream clock: domain=%u source=%u type=0x%04x location=%s/%u generation=%u",
				pendingSelection.clock_domain_index,
				pendingSelection.clock_source_index,
				pendingSelection.clock_source_type,
				(pendingSelection.clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) ? "input" : "output",
				pendingSelection.clock_source_location_index,
				pendingSelection.generation);
			pPvtData->selectedClockPendingLogged = TRUE;
			pPvtData->selectedClockPendingGeneration = pendingSelection.generation;
		}
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}
	pPvtData->selectedClockPendingLogged = FALSE;

	if ((*dataLen - TOTAL_HEADER_SIZE) < pPvtData->payloadSize) {
		AVB_LOG_ERROR("Not enough room in packet for payload");
		openavbMediaQTailUnlock(pMediaQ);
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	U32 tmp32;
	U8 *pHdrV0 = pData;
	U32 *pHdr = (U32 *)(pData + AVTP_V0_HEADER_SIZE);
	U8  *pPayload = pData + TOTAL_HEADER_SIZE;

	U32 bytesProcessed = 0;
	bool dropForWarmup = FALSE;
	while (bytesProcessed < bytesNeeded) {
		pMediaQItem = openavbMediaQTailLock(pMediaQ, TRUE);
		if (pMediaQItem && pMediaQItem->pPubData && pMediaQItem->dataLen > 0) {
			bool dropThisPacketForWarmup = FALSE;
			bool muteThisPacketForSettle = FALSE;

			// timestamp set in the interface module, here just validate
			// In sparse mode, the timestamp valid flag should be set every eighth AAF AVPTDU.
			if (pPvtData->sparseMode == TS_SPARSE_MODE_ENABLED && (pHdrV0[HIDX_AVTP_SEQ_NUM] & 0x07) != 0) {
				// Skip over this timestamp, as using sparse mode.
				pHdrV0[HIDX_AVTP_HIDE7_TV1] &= ~0x01;
				pHdrV0[HIDX_AVTP_HIDE7_TU1] &= ~0x01;
				*pHdr++ = 0; // Clear the timestamp field
			}
			else if (!openavbAvtpTimeTimestampIsValid(pMediaQItem->pAvtpTime)) {
				// Timestamp not available; clear timestamp valid flag.
				AVB_LOG_ERROR("Unable to get the timestamp value");
				pHdrV0[HIDX_AVTP_HIDE7_TV1] &= ~0x01;
				pHdrV0[HIDX_AVTP_HIDE7_TU1] &= ~0x01;
				*pHdr++ = 0; // Clear the timestamp field
			}
			else {
				// Compute per-packet timestamp from the media queue item's base time.
				U64 itemBaseNs = openavbAvtpTimeGetAvtpTimeNS(pMediaQItem->pAvtpTime);
				U64 baseNs = itemBaseNs;
				U64 sourceBaseNs = baseNs;
				U64 intervalNs = 0;
				U64 itemIntervalNs = 0;
				U64 packetTsNs;
				U64 launchTimeNs = 0;
					U64 preClampLaunchTimeNs = 0;
					U64 preClampPacketTsNs = 0;
					U64 maxTransitNs = ((U64)pPvtData->maxTransitUsec * NANOSECONDS_PER_USEC);
					U64 minPacketTsNs = 0;
					U32 packetIndex = 0;
					bool sourceSelectionChanged = FALSE;
					U64 nowNs = 0;
					bool clamped = FALSE;
					bool reanchorAfterWarmup = FALSE;
				bool timestampUncertain = openavbAvtpTimeTimestampIsUncertain(pMediaQItem->pAvtpTime);
				openavb_clock_source_runtime_t clockSelection = {0};
				U32 crfGeneration = 0;
				U32 recoveryGeneration = osalClockGetWalltimeRecoveryGeneration();
				bool usingLocalMediaClock = FALSE;
				bool usingSelectedInputClock = FALSE;
				if (pPvtData->useSelectedClockTimestamps) {
					usingSelectedInputClock =
						mapAafGetSelectedStreamClock(
							&sourceBaseNs,
							&timestampUncertain,
							&clockSelection,
							&crfGeneration,
							pPvtData->selectedClockPreferLocalMediaPhase,
							&usingLocalMediaClock);
				}
				if (usingSelectedInputClock) {
					baseNs = sourceBaseNs;
				}
				if (pPvtData->txInterval > 0) {
					intervalNs = NANOSECONDS_PER_SECOND / pPvtData->txInterval;
				}
				if (pPvtData->payloadSize > 0) {
					packetIndex = pMediaQItem->readIdx / pPvtData->payloadSize;
				}
				itemIntervalNs = intervalNs;
				if (pPubMapInfo->packingFactor > 1) {
					itemIntervalNs *= pPubMapInfo->packingFactor;
				}

				bool recoveryGenerationChanged = usingSelectedInputClock &&
					(recoveryGeneration != pPvtData->selectedClockRecoveryGeneration);

				if (usingSelectedInputClock != pPvtData->usingSelectedInputClock ||
						(usingSelectedInputClock &&
						 (clockSelection.generation != pPvtData->selectedClockGeneration ||
						  recoveryGenerationChanged))) {
					if (recoveryGenerationChanged &&
							pPvtData->selectedClockRecoveryGeneration != 0) {
						AVB_LOGF_WARNING(
							"AAF TX clock recovery epoch change: domain=%u source=%u location=%s/%u recovery_generation=%u previous=%u",
							clockSelection.clock_domain_index,
							clockSelection.clock_source_index,
							(clockSelection.clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) ? "input" : "output",
							clockSelection.clock_source_location_index,
							recoveryGeneration,
							pPvtData->selectedClockRecoveryGeneration);
					}
					if (usingSelectedInputClock) {
						AVB_LOGF_INFO("AAF TX clock source: STREAM selected (domain=%u source=%u type=0x%04x location=%s/%u generation=%u crf_generation=%u)",
							clockSelection.clock_domain_index,
							clockSelection.clock_source_index,
							clockSelection.clock_source_type,
							(clockSelection.clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) ? "input" : "output",
							clockSelection.clock_source_location_index,
							clockSelection.generation,
							crfGeneration);
						AVB_LOGF_INFO("AAF TX selected clock follow updates: %s (%s)",
							(pPvtData->selectedClockFollowUpdates || usingLocalMediaClock) ? "enabled" : "disabled",
							usingLocalMediaClock
								? "local media phase"
								: (pPvtData->selectedClockPreferLocalMediaPhase ? "projected CRF time" : "forced projected CRF time"));
					}
					else {
						AVB_LOG_INFO("AAF TX clock source: using interface-provided media timestamp");
					}
					pPvtData->usingSelectedInputClock = usingSelectedInputClock;
					pPvtData->selectedClockGeneration = clockSelection.generation;
					pPvtData->selectedCrfGeneration = crfGeneration;
					pPvtData->selectedClockRecoveryGeneration = recoveryGeneration;
					pPvtData->txDiagPrevPacketTsValid = FALSE;
					pPvtData->txDiagPrevPacketTsNs = 0;
					pPvtData->txDiagPrevItemBaseNs = 0;
					pPvtData->txDiagPrevSourceBaseNs = 0;
					pPvtData->txDiagPrevCadenceBaseNs = 0;
					pPvtData->txDiagPrevLaunchTimeNs = 0;
					pPvtData->txDiagPrevReadIdx = 0;
					pPvtData->txDiagPrevPacketIndex = 0;
					pPvtData->txDiagPrevClockGeneration = 0;
					pPvtData->txDiagPrevCrfGeneration = 0;
					mapAafResetTxItem0Diag(pPvtData);
					mapAafResetTxPhaseDiag(pPvtData);
					if (usingSelectedInputClock && usingLocalMediaClock &&
							pPvtData->selectedClockWarmupUsec > 0) {
						CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
						pPvtData->selectedClockWarmupActive = TRUE;
						pPvtData->selectedClockWarmupLogged = FALSE;
						pPvtData->selectedClockWarmupUntilNs =
							nowNs + ((U64)pPvtData->selectedClockWarmupUsec * NANOSECONDS_PER_USEC);
						pPvtData->selectedClockWarmupDropCount = 0;
					}
					else {
						pPvtData->selectedClockWarmupActive = FALSE;
						pPvtData->selectedClockWarmupLogged = FALSE;
						pPvtData->selectedClockWarmupUntilNs = 0;
						pPvtData->selectedClockWarmupDropCount = 0;
						pPvtData->selectedClockMuteActive = FALSE;
						pPvtData->selectedClockMuteLogged = FALSE;
						pPvtData->selectedClockMuteUntilNs = 0;
						pPvtData->selectedClockMutePacketCount = 0;
						if (usingSelectedInputClock && usingLocalMediaClock &&
								pPvtData->selectedClockMuteUsec > 0) {
							CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
							pPvtData->selectedClockMuteActive = TRUE;
							pPvtData->selectedClockMuteLogged = FALSE;
							pPvtData->selectedClockMuteUntilNs =
								nowNs + ((U64)pPvtData->selectedClockMuteUsec * NANOSECONDS_PER_USEC);
							pPvtData->selectedClockMutePacketCount = 0;
						}
					}
				}

				// Selected CRF clocks can be produced at lower packet rates (e.g. 500 pps)
				// than AAF (e.g. 8000 pps). Synthesize an AAF-rate timeline from the selected
				// clock, and only rebase when the selected clock meaningfully diverges.
					if (usingSelectedInputClock && intervalNs > 0) {
						sourceSelectionChanged = (!pPvtData->selectedClockCadenceValid) ||
							(clockSelection.generation != pPvtData->selectedClockCadenceGeneration) ||
							recoveryGenerationChanged ||
							(usingLocalMediaClock != pPvtData->selectedClockCadenceUsingMediaClock);
					bool freshCrfSample =
						(crfGeneration != pPvtData->selectedClockCadenceCrfGeneration);
					bool followSelectedClockUpdates =
						pPvtData->selectedClockFollowUpdates;

					if (sourceSelectionChanged) {
						pPvtData->selectedClockCadenceBaseNs = baseNs;
						pPvtData->selectedClockCadenceValid = TRUE;
						pPvtData->selectedClockCadenceUsingMediaClock = usingLocalMediaClock;
						pPvtData->selectedClockCadenceGeneration = clockSelection.generation;
						pPvtData->selectedClockCadenceCrfGeneration = crfGeneration;
					}
					else if (packetIndex > 0) {
						/*
						 * Continuing to packetize the same media-queue item. Keep the
						 * cadence base anchored to the start of that item and let
						 * packetIndex provide the per-packet offset. Advancing the base
						 * here would double-count the packet step for packed items.
						 */
					}
					else {
						U64 nextCadenceNs = pPvtData->selectedClockCadenceBaseNs + itemIntervalNs;
						if (usingLocalMediaClock) {
							/*
							 * For the local shared-media-clock path, keep packet cadence
							 * strictly monotonic once the source is selected. The warm-up
							 * gate handles the initial lock-on transient; after that the
							 * safest behavior is to avoid introducing callback-jitter-sized
							 * packet-step perturbations from fresh local phase samples.
							 *
							 * However, packed talkers that maintain cadence independently per
							 * stream can drift apart by a whole item interval over time. When
							 * the shared local media phase is already more than an item away
							 * from the synthesized next step, treat that as lost cadence lock
							 * and re-anchor forward to the shared phase instead of preserving
							 * the stale per-stream base indefinitely.
							 *
							 * Do not hard re-anchor backward just because a fresh local phase
							 * sample arrived slightly late. That creates a non-monotonic AAF
							 * packet timestamp step at the item boundary, which is worse than
							 * briefly carrying a synthesized cadence that is ahead of the
							 * latest local sample. Instead, if the synthesized cadence is
							 * ahead of the fresh local source by more than one packet, pull it
							 * back with a bounded negative slew so the cadence stays monotonic
							 * while converging toward the shared local media phase.
							 */
							S64 cadenceErrNs = (baseNs >= nextCadenceNs)
								? (S64)(baseNs - nextCadenceNs)
								: -((S64)(nextCadenceNs - baseNs));
							S64 reanchorThresholdNs = (S64)itemIntervalNs;
							S64 maxPhaseSlewNs = (S64)(itemIntervalNs / 4ULL);

							if (cadenceErrNs > reanchorThresholdNs) {
								pPvtData->txCadenceHardRebaseCount++;
								if (pPvtData->txCadenceHardRebaseCount <= 16 ||
										(pPvtData->txCadenceHardRebaseCount % 2000) == 0) {
									AVB_LOGF_INFO(
										"AAF TX local cadence re-anchor: err=%lldns expected_step=%lluns gen=%u crf_gen=%u rebases=%llu",
										(long long)cadenceErrNs,
										(unsigned long long)itemIntervalNs,
										clockSelection.generation,
										crfGeneration,
										(unsigned long long)pPvtData->txCadenceHardRebaseCount);
								}
								pPvtData->selectedClockCadenceBaseNs = baseNs;
							}
							else {
								/*
								 * Once selected-clock warm-up has completed, keep the
								 * synthesized local cadence weakly phase-locked to the
								 * shared local media source instead of waiting until the
								 * error grows to a whole packet. Large +/-1 packet swings
								 * keep transport monotonic, but they still show up as
								 * listener lock/unlock churn. Use a small bounded per-item
								 * correction so the cadence stays monotonic while its
								 * source-to-cadence error stays well inside one packet.
								 */
								S64 phaseAdjustNs = cadenceErrNs / 4;
								if (phaseAdjustNs > maxPhaseSlewNs) {
									phaseAdjustNs = maxPhaseSlewNs;
								}
								else if (phaseAdjustNs < -maxPhaseSlewNs) {
									phaseAdjustNs = -maxPhaseSlewNs;
								}
								if (phaseAdjustNs != 0) {
									pPvtData->txCadenceSlewEventCount++;
									if ((llabs(cadenceErrNs) > maxPhaseSlewNs) &&
											(pPvtData->txCadenceSlewEventCount <= 16 ||
											 (pPvtData->txCadenceSlewEventCount % 5000) == 0)) {
										AVB_LOGF_INFO(
											"AAF TX local cadence slew: err=%lldns adj=%lldns expected_step=%lluns gen=%u crf_gen=%u slews=%llu",
											(long long)cadenceErrNs,
											(long long)phaseAdjustNs,
											(unsigned long long)itemIntervalNs,
											clockSelection.generation,
											crfGeneration,
											(unsigned long long)pPvtData->txCadenceSlewEventCount);
									}
								}
								pPvtData->selectedClockCadenceBaseNs = nextCadenceNs + phaseAdjustNs;
							}
							pPvtData->selectedClockCadenceCrfGeneration = crfGeneration;
						}
						else {
							/*
							 * In projected-CRF mode, each new mediaQ item arrives with a
							 * fresh projected source phase in sourceBaseNs/baseNs. If a
							 * per-stream synthesized cadence base has drifted materially
							 * behind that shared projection, carrying it forward can leave
							 * just one talker stream producing launch times that are already
							 * stale before AVTP sees them. Re-anchor at the item boundary
							 * once the drift is larger than one packet interval.
							 */
							S64 cadenceErrNs = (baseNs >= nextCadenceNs)
								? (S64)(baseNs - nextCadenceNs)
								: -((S64)(nextCadenceNs - baseNs));
							S64 reanchorThresholdNs = (S64)intervalNs;

							if (llabs(cadenceErrNs) > reanchorThresholdNs) {
								pPvtData->selectedClockCadenceBaseNs = baseNs;
								pPvtData->selectedClockCadenceCrfGeneration = crfGeneration;
							}
							else if (freshCrfSample && !followSelectedClockUpdates) {
								// Keep a stable synthesized cadence once selected. Ignore
								// subsequent CRF sample phase updates to avoid introducing
								// network-jitter artifacts into AAF packet timestamps.
								pPvtData->selectedClockCadenceBaseNs = nextCadenceNs;
								pPvtData->selectedClockCadenceCrfGeneration = crfGeneration;
							}
							else if (freshCrfSample) {
								S64 hardRebaseErrNs = cadenceErrNs;
								// Pull toward CRF with bounded slew to avoid frequent hard phase jumps.
								// Hard rebase is only for very large discontinuities.
								S64 hardRebaseNs = (S64)(itemIntervalNs * 256ULL);
								S64 maxSlewNs = (S64)(itemIntervalNs * 4ULL);
								S64 phaseAdjustNs = hardRebaseErrNs / 8;
								if (phaseAdjustNs > maxSlewNs) {
									phaseAdjustNs = maxSlewNs;
								}
								else if (phaseAdjustNs < -maxSlewNs) {
									phaseAdjustNs = -maxSlewNs;
								}

								if (llabs(hardRebaseErrNs) > hardRebaseNs) {
									pPvtData->txCadenceHardRebaseCount++;
									if (pPvtData->txCadenceHardRebaseCount <= 16 ||
											(pPvtData->txCadenceHardRebaseCount % 2000) == 0) {
										AVB_LOGF_WARNING(
											"AAF TX clock cadence hard rebase: err=%lldns expected_step=%lluns gen=%u crf_gen=%u rebases=%llu",
											(long long)hardRebaseErrNs,
											(unsigned long long)itemIntervalNs,
											clockSelection.generation,
											crfGeneration,
											(unsigned long long)pPvtData->txCadenceHardRebaseCount);
									}
									pPvtData->selectedClockCadenceBaseNs = baseNs;
								}
								else {
									if (llabs(hardRebaseErrNs) > maxSlewNs) {
										pPvtData->txCadenceSlewEventCount++;
										if (pPvtData->txCadenceSlewEventCount <= 16 ||
												(pPvtData->txCadenceSlewEventCount % 5000) == 0) {
											AVB_LOGF_INFO(
												"AAF TX clock cadence slew: err=%lldns adj=%lldns expected_step=%lluns gen=%u crf_gen=%u slews=%llu",
												(long long)hardRebaseErrNs,
												(long long)phaseAdjustNs,
												(unsigned long long)itemIntervalNs,
												clockSelection.generation,
												crfGeneration,
												(unsigned long long)pPvtData->txCadenceSlewEventCount);
										}
									}
									pPvtData->selectedClockCadenceBaseNs = nextCadenceNs + (S64)phaseAdjustNs;
								}
								pPvtData->selectedClockCadenceCrfGeneration = crfGeneration;
							}
							else {
								// No fresh CRF sample yet; keep advancing the synthesized AAF timeline.
								pPvtData->selectedClockCadenceBaseNs = nextCadenceNs;
							}
						}
					}
					baseNs = pPvtData->selectedClockCadenceBaseNs;
					}
					else {
						pPvtData->selectedClockCadenceValid = FALSE;
						pPvtData->selectedClockCadenceUsingMediaClock = FALSE;
						pPvtData->selectedClockPresentationValid = FALSE;
					}
					if (pPvtData->itemTimestampIsPresentation) {
					/*
					 * Milan AAF carries a presentation timestamp for the packet's
					 * audio content. The authoritative cadence comes from the
					 * selected media clock, but on the direct path the packetized
					 * samples are already some fixed distance ahead of that clock
					 * because of buffering between source export and wire. Capture
					 * that phase once when the selected source/warm-up anchor
					 * changes, then keep it fixed instead of relearning it item by
					 * item or letting launch-survival logic rewrite it.
					 */
						U64 presentationBaseNs = itemBaseNs;
						if (usingSelectedInputClock) {
							/*
							 * In the CRF-paced MVP, the direct interface exports the
							 * authoritative presentation time for the first sample in the
							 * mediaQ item. Do not create a second selected-clock
							 * presentation anchor in the mapper.
							 */
							pPvtData->selectedClockPresentationValid = FALSE;
							pPvtData->selectedClockPresentationBaseNs = itemBaseNs;
							pPvtData->selectedClockPresentationOffsetNs = 0;
						}
					packetTsNs = presentationBaseNs + (intervalNs * packetIndex);
					if (usingSelectedInputClock && pPvtData->selectedClockTrimUsec != 0) {
						S64 trimNs = (S64)pPvtData->selectedClockTrimUsec * (S64)NANOSECONDS_PER_USEC;
						if (trimNs >= 0) {
							packetTsNs += (U64)trimNs;
						}
						else {
							U64 absTrimNs = (U64)(-trimNs);
							packetTsNs = (packetTsNs > absTrimNs)
								? (packetTsNs - absTrimNs)
								: 0;
						}
					}
					launchTimeNs = (packetTsNs > maxTransitNs)
						? (packetTsNs - maxTransitNs)
						: 0;
				}
				else {
					launchTimeNs = baseNs + (intervalNs * packetIndex);
					if (usingSelectedInputClock && pPvtData->txMinLeadUsec > 0) {
						launchTimeNs += ((U64)pPvtData->txMinLeadUsec * NANOSECONDS_PER_USEC);
					}
					if (usingSelectedInputClock && pPvtData->selectedClockTrimUsec != 0) {
						S64 trimNs = (S64)pPvtData->selectedClockTrimUsec * (S64)NANOSECONDS_PER_USEC;
						if (trimNs >= 0) {
							launchTimeNs += (U64)trimNs;
						}
						else {
							U64 absTrimNs = (U64)(-trimNs);
							if (launchTimeNs > absTrimNs) {
								launchTimeNs -= absTrimNs;
							}
							else {
								launchTimeNs = 0;
							}
						}
					}
					packetTsNs = launchTimeNs + maxTransitNs;
				}
				preClampLaunchTimeNs = launchTimeNs;
				preClampPacketTsNs = packetTsNs;

				U64 unclampedPacketTsNs = packetTsNs;
				if (pPvtData->itemTimestampIsPresentation && pPvtData->txMinLeadUsec > 0) {
					CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
					minPacketTsNs = nowNs + ((U64)pPvtData->txMinLeadUsec * NANOSECONDS_PER_USEC);
					minPacketTsNs += maxTransitNs;
					if (packetTsNs < minPacketTsNs) {
						packetTsNs = minPacketTsNs;
						launchTimeNs = packetTsNs - maxTransitNs;
						clamped = TRUE;
					}
				}
				/*
				 * In a Milan-style selected-clock path, launch-margin survival logic
				 * must not rewrite the media timeline. If the packet timestamp needs
				 * to be clamped forward for transmit survival, keep that as a local
				 * transmit decision only and preserve the configured CRF-relative
				 * presentation offset as the authoritative media-time relationship.
				 */
				if (usingSelectedInputClock && usingLocalMediaClock &&
						pPvtData->selectedClockWarmupActive) {
					if (nowNs == 0) {
						CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
					}
					if (nowNs < pPvtData->selectedClockWarmupUntilNs) {
						dropThisPacketForWarmup = TRUE;
						if (!pPvtData->selectedClockWarmupLogged) {
							U64 remainingUsec =
								(pPvtData->selectedClockWarmupUntilNs - nowNs) / NANOSECONDS_PER_USEC;
							AVB_LOGF_INFO(
								"AAF TX warming selected clock before transmit: domain=%u source=%u location=%s/%u remaining=%lluus",
								clockSelection.clock_domain_index,
								clockSelection.clock_source_index,
								(clockSelection.clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) ? "input" : "output",
								clockSelection.clock_source_location_index,
								(unsigned long long)remainingUsec);
							pPvtData->selectedClockWarmupLogged = TRUE;
						}
					}
					else {
						pPvtData->selectedClockWarmupActive = FALSE;
						if (pPvtData->selectedClockWarmupLogged ||
								pPvtData->selectedClockWarmupDropCount > 0) {
							AVB_LOGF_INFO(
								"AAF TX selected clock warm-up complete: dropped_packets=%llu duration=%uus",
								(unsigned long long)pPvtData->selectedClockWarmupDropCount,
								pPvtData->selectedClockWarmupUsec);
						}
						pPvtData->selectedClockWarmupLogged = FALSE;
						pPvtData->selectedClockWarmupUntilNs = 0;
						if (usingSelectedInputClock && usingLocalMediaClock &&
								pPvtData->selectedClockCadenceValid) {
							reanchorAfterWarmup = TRUE;
							if (pPvtData->selectedClockMuteUsec > 0) {
								pPvtData->selectedClockMuteActive = TRUE;
								pPvtData->selectedClockMuteLogged = FALSE;
								pPvtData->selectedClockMuteUntilNs =
									nowNs + ((U64)pPvtData->selectedClockMuteUsec * NANOSECONDS_PER_USEC);
								pPvtData->selectedClockMutePacketCount = 0;
							}
						}
					}
				}
					if (reanchorAfterWarmup) {
					/*
					 * Warm-up intentionally drops the initial packet window while the
					 * selected local CRF/media clock settles. Re-anchor the first
					 * transmitted packet to the current shared phase instead of the
					 * synthesized cadence base accumulated during the dropped period.
					 * Without this, the whole bus32split group can inherit an
					 * arbitrary run-to-run offset that then stays stable for the
					 * duration of the run.
					 */
					U64 sharedAnchorNs = sourceBaseNs;
					if (intervalNs > 0 && packetIndex > 0) {
						/*
						 * If warm-up ends mid-item, re-anchor to the start of the item
						 * rather than the current packet within that item. Otherwise the
						 * stream that happens to leave warm-up on packet N can inherit a
						 * stable N * txInterval offset for the whole run.
						 */
						sharedAnchorNs -= (intervalNs * packetIndex);
					}
					bool createdSharedAnchor = FALSE;
					openavbClockSourceRuntimeAcquireWarmupAnchorForLocation(
						clockSelection.clock_source_location_type,
						clockSelection.clock_source_location_index,
						clockSelection.generation,
						recoveryGeneration,
						0,
						sharedAnchorNs,
						&sharedAnchorNs,
						&createdSharedAnchor);
						pPvtData->selectedClockCadenceBaseNs = sharedAnchorNs;
						pPvtData->selectedClockCadenceCrfGeneration = crfGeneration;
						pPvtData->selectedClockPresentationValid = FALSE;
						baseNs = sharedAnchorNs;
					if (pPvtData->itemTimestampIsPresentation) {
						pPvtData->selectedClockPresentationValid = FALSE;
						pPvtData->selectedClockPresentationBaseNs = itemBaseNs;
						pPvtData->selectedClockPresentationOffsetNs = 0;
						packetTsNs = itemBaseNs + (intervalNs * packetIndex);
						if (pPvtData->selectedClockTrimUsec != 0) {
							S64 trimNs = (S64)pPvtData->selectedClockTrimUsec *
								(S64)NANOSECONDS_PER_USEC;
							if (trimNs >= 0) {
								packetTsNs += (U64)trimNs;
							}
							else {
								U64 absTrimNs = (U64)(-trimNs);
								packetTsNs = (packetTsNs > absTrimNs)
									? (packetTsNs - absTrimNs)
									: 0;
							}
						}
						launchTimeNs = (packetTsNs > maxTransitNs)
							? (packetTsNs - maxTransitNs)
							: 0;
					}
					else {
						launchTimeNs = baseNs + (intervalNs * packetIndex);
						if (pPvtData->txMinLeadUsec > 0) {
							launchTimeNs += ((U64)pPvtData->txMinLeadUsec * NANOSECONDS_PER_USEC);
						}
						if (pPvtData->selectedClockTrimUsec != 0) {
							S64 trimNs = (S64)pPvtData->selectedClockTrimUsec * (S64)NANOSECONDS_PER_USEC;
							if (trimNs >= 0) {
								launchTimeNs += (U64)trimNs;
							}
							else {
								U64 absTrimNs = (U64)(-trimNs);
								if (launchTimeNs > absTrimNs) {
									launchTimeNs -= absTrimNs;
								}
								else {
									launchTimeNs = 0;
								}
							}
						}
						packetTsNs = launchTimeNs +
							((U64)pPvtData->maxTransitUsec * NANOSECONDS_PER_USEC);
					}
					unclampedPacketTsNs = packetTsNs;
					pPvtData->txDiagPrevPacketTsValid = FALSE;
					pPvtData->txDiagPrevPacketTsNs = 0;
					pPvtData->txDiagPrevItemBaseNs = 0;
					pPvtData->txDiagPrevSourceBaseNs = 0;
					pPvtData->txDiagPrevCadenceBaseNs = 0;
					pPvtData->txDiagPrevLaunchTimeNs = 0;
					pPvtData->txDiagPrevReadIdx = 0;
					pPvtData->txDiagPrevPacketIndex = 0;
					pPvtData->txDiagPrevClockGeneration = 0;
					pPvtData->txDiagPrevCrfGeneration = 0;
					mapAafResetTxItem0Diag(pPvtData);
					mapAafResetTxPhaseDiag(pPvtData);
					AVB_LOGF_INFO(
						"AAF TX warm-up re-anchor: stream=%u name=%s base=%lluns packet=%u transit=%uus launch_lead=%uus shared=%u",
						pMediaQ->debug_stream_uid,
						pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
						(unsigned long long)baseNs,
						packetIndex,
						pPvtData->maxTransitUsec,
						pPvtData->txMinLeadUsec,
						createdSharedAnchor ? 1U : 0U);
				}
				if (!dropThisPacketForWarmup && usingSelectedInputClock &&
						usingLocalMediaClock && pPvtData->selectedClockMuteActive) {
					if (nowNs == 0) {
						CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
					}
					if (nowNs < pPvtData->selectedClockMuteUntilNs) {
						muteThisPacketForSettle = TRUE;
						if (!pPvtData->selectedClockMuteLogged) {
							U64 remainingUsec =
								(pPvtData->selectedClockMuteUntilNs - nowNs) / NANOSECONDS_PER_USEC;
							AVB_LOGF_INFO(
								"AAF TX muted settle window active: stream=%u name=%s remaining=%lluus",
								pMediaQ->debug_stream_uid,
								pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
								(unsigned long long)remainingUsec);
							pPvtData->selectedClockMuteLogged = TRUE;
						}
					}
					else {
						pPvtData->selectedClockMuteActive = FALSE;
						if (pPvtData->selectedClockMuteLogged ||
								pPvtData->selectedClockMutePacketCount > 0) {
							AVB_LOGF_INFO(
								"AAF TX muted settle window complete: stream=%u name=%s muted_packets=%llu duration=%uus",
								pMediaQ->debug_stream_uid,
								pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
								(unsigned long long)pPvtData->selectedClockMutePacketCount,
								pPvtData->selectedClockMuteUsec);
						}
						pPvtData->selectedClockMuteLogged = FALSE;
						pPvtData->selectedClockMuteUntilNs = 0;
					}
				}
				if (usingSelectedInputClock) {
					/*
					 * In selected-clock mode, the useful observability points are:
					 * 1. the interface item's own packet timeline
					 * 2. the selected source packet timeline
					 * 3. the synthesized per-stream cadence packet timeline
					 *
					 * Comparing those directly tells us whether the interface export is
					 * wandering relative to the selected clock, and whether the local
					 * per-stream cadence base is drifting away from that selected source.
					 */
					U64 itemPacketTsNs = itemBaseNs + (intervalNs * packetIndex);
					U64 sourcePacketTsNs = sourceBaseNs +
						((U64)pPvtData->maxTransitUsec * NANOSECONDS_PER_USEC) +
						(intervalNs * packetIndex);
					U64 cadencePacketTsNs = baseNs +
						((U64)pPvtData->maxTransitUsec * NANOSECONDS_PER_USEC) +
						(intervalNs * packetIndex);
					if (!dropThisPacketForWarmup) {
						U64 clampDeltaNs = (packetTsNs > unclampedPacketTsNs)
							? (packetTsNs - unclampedPacketTsNs)
							: 0;
						mapAafUpdateTxPhaseDiag(
							pMediaQ,
							pPvtData,
							itemPacketTsNs,
							sourcePacketTsNs,
							cadencePacketTsNs,
							clampDeltaNs,
							&clockSelection,
							crfGeneration,
							usingLocalMediaClock);
					}
				}

				// Detect timestamp cadence anomalies (e.g., 2ms steps on 8kpps stream).
				if (!dropThisPacketForWarmup && intervalNs > 0) {
					pPvtData->txDiagPacketCount++;
					U32 tsChainLogEveryPackets = (pPvtData->txInterval > 0)
						? pPvtData->txInterval
						: 8000U;
							S64 itemToSourceNs = (sourceBaseNs >= itemBaseNs)
								? (S64)(sourceBaseNs - itemBaseNs)
								: -((S64)(itemBaseNs - sourceBaseNs));
							S64 sourceToCadenceNs = (baseNs >= sourceBaseNs)
								? (S64)(baseNs - sourceBaseNs)
								: -((S64)(sourceBaseNs - baseNs));
					S64 cadenceToLaunchNs = (launchTimeNs >= baseNs)
						? (S64)(launchTimeNs - baseNs)
						: -((S64)(baseNs - launchTimeNs));
					S64 launchToTsNs = (packetTsNs >= launchTimeNs)
						? (S64)(packetTsNs - launchTimeNs)
						: -((S64)(launchTimeNs - packetTsNs));

					if (packetIndex == 0) {
						S64 item0ThresholdNs = (itemIntervalNs > 0)
							? (S64)(itemIntervalNs / 8)
							: (S64)(intervalNs / 8);

						if (pPvtData->txItem0PrevValid) {
							S64 item0ItemStepNs = (itemBaseNs >= pPvtData->txItem0PrevItemBaseNs)
								? (S64)(itemBaseNs - pPvtData->txItem0PrevItemBaseNs)
								: -((S64)(pPvtData->txItem0PrevItemBaseNs - itemBaseNs));
							S64 item0SourceStepNs = (sourceBaseNs >= pPvtData->txItem0PrevSourceBaseNs)
								? (S64)(sourceBaseNs - pPvtData->txItem0PrevSourceBaseNs)
								: -((S64)(pPvtData->txItem0PrevSourceBaseNs - sourceBaseNs));
							S64 item0CadenceStepNs = (baseNs >= pPvtData->txItem0PrevCadenceBaseNs)
								? (S64)(baseNs - pPvtData->txItem0PrevCadenceBaseNs)
								: -((S64)(pPvtData->txItem0PrevCadenceBaseNs - baseNs));
							S64 item0LaunchStepNs = (launchTimeNs >= pPvtData->txItem0PrevLaunchTimeNs)
								? (S64)(launchTimeNs - pPvtData->txItem0PrevLaunchTimeNs)
								: -((S64)(pPvtData->txItem0PrevLaunchTimeNs - launchTimeNs));
							S64 item0TsStepNs = (packetTsNs >= pPvtData->txItem0PrevPacketTsNs)
								? (S64)(packetTsNs - pPvtData->txItem0PrevPacketTsNs)
								: -((S64)(pPvtData->txItem0PrevPacketTsNs - packetTsNs));
							bool item0Anomaly =
								llabs(item0ItemStepNs - (S64)itemIntervalNs) > item0ThresholdNs ||
								llabs(item0SourceStepNs - (S64)itemIntervalNs) > item0ThresholdNs ||
								llabs(item0CadenceStepNs - (S64)itemIntervalNs) > item0ThresholdNs ||
								llabs(item0LaunchStepNs - (S64)itemIntervalNs) > item0ThresholdNs ||
								llabs(item0TsStepNs - (S64)itemIntervalNs) > item0ThresholdNs;

							if (item0Anomaly) {
								pPvtData->txItem0DiagCount++;
								if (pPvtData->txItem0DiagCount <= 32 ||
									(pPvtData->txItem0DiagCount % 2000ULL) == 0ULL) {
									const char *buildMode =
										(!usingSelectedInputClock && pPvtData->itemTimestampIsPresentation)
											? "iface_presentation"
											: (usingSelectedInputClock ? "selected_clock" : "interface_launch");
									S64 preCadenceToLaunchNs = (preClampLaunchTimeNs >= baseNs)
										? (S64)(preClampLaunchTimeNs - baseNs)
										: -((S64)(baseNs - preClampLaunchTimeNs));
									S64 preLaunchToTsNs = (preClampPacketTsNs >= preClampLaunchTimeNs)
										? (S64)(preClampPacketTsNs - preClampLaunchTimeNs)
										: -((S64)(preClampLaunchTimeNs - preClampPacketTsNs));
									S64 postCadenceToLaunchNs = (launchTimeNs >= baseNs)
										? (S64)(launchTimeNs - baseNs)
										: -((S64)(baseNs - launchTimeNs));
									S64 postLaunchToTsNs = (packetTsNs >= launchTimeNs)
										? (S64)(packetTsNs - launchTimeNs)
										: -((S64)(launchTimeNs - packetTsNs));
									U64 clampDeltaNs = (packetTsNs > preClampPacketTsNs)
										? (packetTsNs - preClampPacketTsNs)
										: 0;
									AVB_LOGF_WARNING(
										"TX OUTLIER ROW flags=.Y. stage=mapper_item0 stream=%u item=%llu src=%llu cad=%llu launch=%llu ts=%llu prev_item=%llu prev_src=%llu prev_cad=%llu prev_launch=%llu prev_ts=%llu item_step_ns=%lld source_step_ns=%lld cadence_step_ns=%lld launch_step_ns=%lld ts_step_ns=%lld expected_item_ns=%lluns rd=%u prev_rd=%u pack=%u clk_gen=%u prev_clk_gen=%u crf_gen=%u prev_crf_gen=%u logs=%llu",
										pMediaQ->debug_stream_uid,
										(unsigned long long)itemBaseNs,
										(unsigned long long)sourceBaseNs,
										(unsigned long long)baseNs,
										(unsigned long long)launchTimeNs,
										(unsigned long long)packetTsNs,
										(unsigned long long)pPvtData->txItem0PrevItemBaseNs,
										(unsigned long long)pPvtData->txItem0PrevSourceBaseNs,
										(unsigned long long)pPvtData->txItem0PrevCadenceBaseNs,
										(unsigned long long)pPvtData->txItem0PrevLaunchTimeNs,
										(unsigned long long)pPvtData->txItem0PrevPacketTsNs,
										(long long)item0ItemStepNs,
										(long long)item0SourceStepNs,
										(long long)item0CadenceStepNs,
										(long long)item0LaunchStepNs,
										(long long)item0TsStepNs,
										(unsigned long long)itemIntervalNs,
										pMediaQItem->readIdx,
										pPvtData->txItem0PrevReadIdx,
										pPubMapInfo->packingFactor,
										clockSelection.generation,
										pPvtData->txItem0PrevClockGeneration,
										crfGeneration,
										pPvtData->txItem0PrevCrfGeneration,
										(unsigned long long)pPvtData->txItem0DiagCount);
									pPvtData->txPacket0BuildDiagCount++;
									AVB_LOGF_WARNING(
										"TX OUTLIER ROW flags=.Y. stage=mapper_pkt0_build stream=%u mode=%s sel=%u loc=%u pres=%u clamp=%u rd=%u pack=%u pre_launch=%llu pre_ts=%llu post_launch=%llu post_ts=%llu min_ts=%llu clamp_delta_ns=%lluns pre_c2l_ns=%lld pre_l2t_ns=%lld post_c2l_ns=%lld post_l2t_ns=%lld interval_ns=%lluns item_interval_ns=%lluns lead_us=%u transit_us=%u trim_us=%d clk_gen=%u crf_gen=%u build_logs=%llu",
										pMediaQ->debug_stream_uid,
										buildMode,
										usingSelectedInputClock ? 1U : 0U,
										usingLocalMediaClock ? 1U : 0U,
										pPvtData->itemTimestampIsPresentation ? 1U : 0U,
										clamped ? 1U : 0U,
										pMediaQItem->readIdx,
										pPubMapInfo->packingFactor,
										(unsigned long long)preClampLaunchTimeNs,
										(unsigned long long)preClampPacketTsNs,
										(unsigned long long)launchTimeNs,
										(unsigned long long)packetTsNs,
										(unsigned long long)minPacketTsNs,
										(unsigned long long)clampDeltaNs,
										(long long)preCadenceToLaunchNs,
										(long long)preLaunchToTsNs,
										(long long)postCadenceToLaunchNs,
										(long long)postLaunchToTsNs,
										(unsigned long long)intervalNs,
										(unsigned long long)itemIntervalNs,
										pPvtData->txMinLeadUsec,
										pPvtData->maxTransitUsec,
										pPvtData->selectedClockTrimUsec,
										clockSelection.generation,
										crfGeneration,
										(unsigned long long)pPvtData->txPacket0BuildDiagCount);
								}
							}
						}

						pPvtData->txItem0PrevItemBaseNs = itemBaseNs;
						pPvtData->txItem0PrevSourceBaseNs = sourceBaseNs;
						pPvtData->txItem0PrevCadenceBaseNs = baseNs;
						pPvtData->txItem0PrevLaunchTimeNs = launchTimeNs;
						pPvtData->txItem0PrevPacketTsNs = packetTsNs;
						pPvtData->txItem0PrevReadIdx = pMediaQItem->readIdx;
						pPvtData->txItem0PrevClockGeneration = clockSelection.generation;
						pPvtData->txItem0PrevCrfGeneration = crfGeneration;
						pPvtData->txItem0PrevValid = TRUE;
					}

					if (pPvtData->txDiagPrevPacketTsValid) {
						S64 deltaNs = (packetTsNs >= pPvtData->txDiagPrevPacketTsNs)
							? (S64)(packetTsNs - pPvtData->txDiagPrevPacketTsNs)
							: -((S64)(pPvtData->txDiagPrevPacketTsNs - packetTsNs));
						S64 errNs = deltaNs - (S64)intervalNs;
						S64 itemDeltaNs = (itemBaseNs >= pPvtData->txDiagPrevItemBaseNs)
							? (S64)(itemBaseNs - pPvtData->txDiagPrevItemBaseNs)
							: -((S64)(pPvtData->txDiagPrevItemBaseNs - itemBaseNs));
						S64 sourceDeltaNs = (sourceBaseNs >= pPvtData->txDiagPrevSourceBaseNs)
							? (S64)(sourceBaseNs - pPvtData->txDiagPrevSourceBaseNs)
							: -((S64)(pPvtData->txDiagPrevSourceBaseNs - sourceBaseNs));
						S64 cadenceDeltaNs = (baseNs >= pPvtData->txDiagPrevCadenceBaseNs)
							? (S64)(baseNs - pPvtData->txDiagPrevCadenceBaseNs)
							: -((S64)(pPvtData->txDiagPrevCadenceBaseNs - baseNs));
						S64 launchDeltaNs = (launchTimeNs >= pPvtData->txDiagPrevLaunchTimeNs)
							? (S64)(launchTimeNs - pPvtData->txDiagPrevLaunchTimeNs)
							: -((S64)(pPvtData->txDiagPrevLaunchTimeNs - launchTimeNs));
						S32 readIdxDelta = (S32)pMediaQItem->readIdx - (S32)pPvtData->txDiagPrevReadIdx;
						S32 packetIndexDelta = (S32)packetIndex - (S32)pPvtData->txDiagPrevPacketIndex;
						S32 clockGenDelta = (S32)clockSelection.generation - (S32)pPvtData->txDiagPrevClockGeneration;
						S32 crfGenDelta = (S32)crfGeneration - (S32)pPvtData->txDiagPrevCrfGeneration;
						S64 thresholdNs = (S64)(intervalNs / 8); // 12.5% tolerance
						if (llabs(errNs) > thresholdNs) {
							pPvtData->txDiagAnomalyCount++;
							if (pPvtData->txDiagAnomalyCount <= 16 ||
									(pPvtData->txDiagAnomalyCount % 2000) == 0) {
								long long gapPackets = 0;
								if (intervalNs > 0) {
									if (deltaNs >= 0) {
										gapPackets = (long long)(deltaNs / (S64)intervalNs) - 1LL;
									}
									else {
										gapPackets = -((long long)((-deltaNs) / (S64)intervalNs) + 1LL);
									}
								}
								AVB_LOGF_WARNING(
									"TX OUTLIER ROW flags=.Y. stage=mapper stream=%u src=%llu cad=%llu launch=%llu ts=%llu pkt=%u rd=%u step_ns=%lld expected_ns=%lluns delta_ns=%lld gap_packets=%lld item_step_ns=%lluns clk_gen=%u crf_gen=%u anomalies=%llu",
									pMediaQ->debug_stream_uid,
									(unsigned long long)sourceBaseNs,
									(unsigned long long)baseNs,
									(unsigned long long)launchTimeNs,
									(unsigned long long)packetTsNs,
									packetIndex,
									pMediaQItem->readIdx,
									(long long)deltaNs,
									(unsigned long long)intervalNs,
									(long long)errNs,
									gapPackets,
									(unsigned long long)itemIntervalNs,
									clockSelection.generation,
									crfGeneration,
									(unsigned long long)pPvtData->txDiagAnomalyCount);
								AVB_LOGF_WARNING(
									"AAF TX timestamp cadence anomaly: stream=%u name=%s delta=%lldns expected=%lluns err=%lldns gap_packets=%lld packet=%u read_idx=%u pack=%u item_step=%lluns selected=%u local=%u warmup_drop=%u settle_mute=%u source_base=%llu cadence_base=%llu launch=%llu ts=%llu prev=%llu item_delta=%lldns source_delta=%lldns cadence_delta=%lldns launch_delta=%lldns read_delta=%d pkt_delta=%d clk_gen=%u prev_clk_gen=%u clk_gen_delta=%d crf_gen=%u prev_crf_gen=%u crf_gen_delta=%d packets=%llu anomalies=%llu",
									pMediaQ->debug_stream_uid,
									pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
									(long long)deltaNs,
									(unsigned long long)intervalNs,
									(long long)errNs,
									gapPackets,
									packetIndex,
									pMediaQItem->readIdx,
									pPubMapInfo->packingFactor,
									(unsigned long long)itemIntervalNs,
									usingSelectedInputClock ? 1U : 0U,
									usingLocalMediaClock ? 1U : 0U,
									dropThisPacketForWarmup ? 1U : 0U,
									muteThisPacketForSettle ? 1U : 0U,
									(unsigned long long)sourceBaseNs,
									(unsigned long long)baseNs,
									(unsigned long long)launchTimeNs,
									(unsigned long long)packetTsNs,
									(unsigned long long)pPvtData->txDiagPrevPacketTsNs,
									(long long)itemDeltaNs,
									(long long)sourceDeltaNs,
									(long long)cadenceDeltaNs,
									(long long)launchDeltaNs,
									(int)readIdxDelta,
									(int)packetIndexDelta,
									clockSelection.generation,
									pPvtData->txDiagPrevClockGeneration,
									(int)clockGenDelta,
									crfGeneration,
									pPvtData->txDiagPrevCrfGeneration,
									(int)crfGenDelta,
									(unsigned long long)pPvtData->txDiagPacketCount,
									(unsigned long long)pPvtData->txDiagAnomalyCount);

								if (nowNs == 0) {
									CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
								}
								S64 nowToLaunchNs = (launchTimeNs >= nowNs)
									? (S64)(launchTimeNs - nowNs)
									: -((S64)(nowNs - launchTimeNs));
								S64 nowToTsNs = (packetTsNs >= nowNs)
									? (S64)(packetTsNs - nowNs)
									: -((S64)(nowNs - packetTsNs));
								AVB_LOGF_WARNING(
									"AAF TX ts chain anomaly: stream=%u pkt=%u rd=%u item=%llu src=%llu cad=%llu launch=%llu ts=%llu i2s=%lldns s2c=%lldns c2l=%lldns l2t=%lldns n2l=%lldns n2t=%lldns step=%lluns item_step=%lluns sel=%u loc=%u crf_gen=%u clk_gen=%u",
									pMediaQ->debug_stream_uid,
									packetIndex,
									pMediaQItem->readIdx,
									(unsigned long long)itemBaseNs,
									(unsigned long long)sourceBaseNs,
									(unsigned long long)baseNs,
									(unsigned long long)launchTimeNs,
									(unsigned long long)packetTsNs,
									(long long)itemToSourceNs,
									(long long)sourceToCadenceNs,
									(long long)cadenceToLaunchNs,
									(long long)launchToTsNs,
									(long long)nowToLaunchNs,
									(long long)nowToTsNs,
									(unsigned long long)intervalNs,
									(unsigned long long)itemIntervalNs,
									usingSelectedInputClock ? 1U : 0U,
									usingLocalMediaClock ? 1U : 0U,
									crfGeneration,
									clockSelection.generation);
							}
						}
					}
					if (usingSelectedInputClock && pPvtData->itemTimestampIsPresentation &&
							intervalNs > 0 && !dropThisPacketForWarmup) {
						U32 alignLogEveryPackets = pPvtData->txPhaseDiagEveryPackets;
						S64 configuredPresentationOffsetNs =
							(S64)((U64)pPvtData->selectedClockPresentationOffsetUsec *
								NANOSECONDS_PER_USEC);
						S64 interfacePresentationOffsetNs = (itemBaseNs >= sourceBaseNs)
							? (S64)(itemBaseNs - sourceBaseNs)
							: -((S64)(sourceBaseNs - itemBaseNs));
						S64 targetCadenceOffsetNs =
							(usingSelectedInputClock && pPvtData->itemTimestampIsPresentation)
								? (interfacePresentationOffsetNs +
									(S64)(intervalNs * (U64)packetIndex))
								: ((usingSelectedInputClock &&
									 pPvtData->selectedClockPresentationValid)
										? (pPvtData->selectedClockPresentationOffsetNs +
											(S64)(intervalNs * (U64)packetIndex))
										: (configuredPresentationOffsetNs +
											(S64)(intervalNs * (U64)packetIndex)));
						S64 extraPresentationLeadNs =
							(usingSelectedInputClock && pPvtData->itemTimestampIsPresentation)
								? (interfacePresentationOffsetNs - configuredPresentationOffsetNs)
								: ((usingSelectedInputClock &&
									 pPvtData->selectedClockPresentationValid)
										? (pPvtData->selectedClockPresentationOffsetNs -
											configuredPresentationOffsetNs)
										: 0);
						S64 actualSourceOffsetNs = (packetTsNs >= sourceBaseNs)
							? (S64)(packetTsNs - sourceBaseNs)
							: -((S64)(sourceBaseNs - packetTsNs));
						S64 actualCadenceOffsetNs = (packetTsNs >= baseNs)
							? (S64)(packetTsNs - baseNs)
							: -((S64)(baseNs - packetTsNs));
						S64 sourceAlignErrNs = actualSourceOffsetNs - targetCadenceOffsetNs;
						S64 cadenceAlignErrNs = actualCadenceOffsetNs - targetCadenceOffsetNs;
						S64 itemSourceOffsetNs = (itemBaseNs >= sourceBaseNs)
							? (S64)(itemBaseNs - sourceBaseNs)
							: -((S64)(sourceBaseNs - itemBaseNs));
						S64 launchToTsNs = (packetTsNs >= launchTimeNs)
							? (S64)(packetTsNs - launchTimeNs)
							: -((S64)(launchTimeNs - packetTsNs));
						U64 clampDeltaNs = (packetTsNs > unclampedPacketTsNs)
							? (packetTsNs - unclampedPacketTsNs)
							: 0;

						if (alignLogEveryPackets == 0) {
							alignLogEveryPackets =
								(pPvtData->txInterval > 0) ? pPvtData->txInterval : 8000U;
						}

						if ((packetIndex == 0 && alignLogEveryPackets > 0 &&
								(pPvtData->txDiagPacketCount % alignLogEveryPackets) == 0ULL) ||
								clampDeltaNs > 0 ||
								llabs(sourceAlignErrNs) > (S64)(intervalNs / 8)) {
							U64 crfRefNs = 0;
							U64 mediaRefNs = 0;
							U32 crfRefGeneration = 0;
							U32 mediaRefGeneration = 0;
							bool crfRefUncertain = FALSE;
							bool mediaRefUncertain = FALSE;
							bool haveCrfRef = FALSE;
							bool haveMediaRef = FALSE;
							S64 packetToCrfNs = 0;
							S64 packetToMediaNs = 0;
							S64 mediaToCrfNs = 0;
							S64 crfAlignErrNs = 0;
							S64 mediaAlignErrNs = 0;

							if (clockSelection.clock_source_location_type ==
									OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT ||
									clockSelection.clock_source_location_type ==
									OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
								haveCrfRef = openavbClockSourceRuntimeGetCrfTimeForLocation(
									clockSelection.clock_source_location_type,
									clockSelection.clock_source_location_index,
									&crfRefNs,
									&crfRefUncertain,
									&crfRefGeneration);
								haveMediaRef = openavbClockSourceRuntimeGetMediaClockForLocation(
									clockSelection.clock_source_location_type,
									clockSelection.clock_source_location_index,
									&mediaRefNs,
									&mediaRefUncertain,
									&mediaRefGeneration);
							}

							if (haveCrfRef) {
								packetToCrfNs = (packetTsNs >= crfRefNs)
									? (S64)(packetTsNs - crfRefNs)
									: -((S64)(crfRefNs - packetTsNs));
								crfAlignErrNs = packetToCrfNs - targetCadenceOffsetNs;
							}
							if (haveMediaRef) {
								packetToMediaNs = (packetTsNs >= mediaRefNs)
									? (S64)(packetTsNs - mediaRefNs)
									: -((S64)(mediaRefNs - packetTsNs));
								mediaAlignErrNs = packetToMediaNs - targetCadenceOffsetNs;
							}
							if (haveCrfRef && haveMediaRef) {
								mediaToCrfNs = (mediaRefNs >= crfRefNs)
									? (S64)(mediaRefNs - crfRefNs)
									: -((S64)(crfRefNs - mediaRefNs));
							}

							AVB_LOGF_WARNING(
								"TX OUTLIER ROW flags=.Y. stage=aaf_crf_align stream=%u rd=%u pack=%u cfg_ns=%lld extra_ns=%lld src_off_ns=%lld cad_off_ns=%lld src_err_ns=%lld cad_err_ns=%lld item_src_ns=%lld s2c_ns=%lld l2t_ns=%lld clamp_ns=%lluns sel=%u loc=%u clk_gen=%u crf_gen=%u",
								pMediaQ->debug_stream_uid,
								pMediaQItem->readIdx,
								pPubMapInfo->packingFactor,
								(long long)configuredPresentationOffsetNs,
								(long long)extraPresentationLeadNs,
								(long long)actualSourceOffsetNs,
								(long long)actualCadenceOffsetNs,
								(long long)sourceAlignErrNs,
								(long long)cadenceAlignErrNs,
								(long long)itemSourceOffsetNs,
								(long long)sourceToCadenceNs,
								(long long)launchToTsNs,
								(unsigned long long)clampDeltaNs,
								usingSelectedInputClock ? 1U : 0U,
								usingLocalMediaClock ? 1U : 0U,
								clockSelection.generation,
								crfGeneration);
							AVB_LOGF_WARNING(
								"TX OUTLIER ROW flags=.Y. stage=aaf_wire_ref stream=%u rd=%u pkt=%u pts=%llu want=%lld p2c=%lld c_err=%lld p2m=%lld m_err=%lld m2c=%lld clamp_ns=%lluns sel=%u loc=%u clk_gen=%u crf_gen=%u med_gen=%u crf_u=%u med_u=%u",
								pMediaQ->debug_stream_uid,
								pMediaQItem->readIdx,
								packetIndex,
								(unsigned long long)packetTsNs,
								(long long)targetCadenceOffsetNs,
								(long long)packetToCrfNs,
								(long long)crfAlignErrNs,
								(long long)packetToMediaNs,
								(long long)mediaAlignErrNs,
								(long long)mediaToCrfNs,
								(unsigned long long)clampDeltaNs,
								usingSelectedInputClock ? 1U : 0U,
								usingLocalMediaClock ? 1U : 0U,
								clockSelection.generation,
								crfRefGeneration,
								mediaRefGeneration,
								haveCrfRef ? (crfRefUncertain ? 1U : 0U) : 2U,
								haveMediaRef ? (mediaRefUncertain ? 1U : 0U) : 2U);
						}
					}
					else if (tsChainLogEveryPackets > 0 &&
							(pPvtData->txDiagPacketCount % tsChainLogEveryPackets) == 0ULL) {
						if (nowNs == 0) {
							CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
						}
						S64 nowToLaunchNs = (launchTimeNs >= nowNs)
							? (S64)(launchTimeNs - nowNs)
							: -((S64)(nowNs - launchTimeNs));
						S64 nowToTsNs = (packetTsNs >= nowNs)
							? (S64)(packetTsNs - nowNs)
							: -((S64)(nowNs - packetTsNs));
						AVB_LOGF_INFO(
							"AAF TX ts chain diag: stream=%u pkt=%u rd=%u item=%llu src=%llu cad=%llu launch=%llu ts=%llu i2s=%lldns s2c=%lldns c2l=%lldns l2t=%lldns n2l=%lldns n2t=%lldns step=%lluns item_step=%lluns sel=%u loc=%u crf_gen=%u clk_gen=%u",
							pMediaQ->debug_stream_uid,
							packetIndex,
							pMediaQItem->readIdx,
							(unsigned long long)itemBaseNs,
							(unsigned long long)sourceBaseNs,
							(unsigned long long)baseNs,
							(unsigned long long)launchTimeNs,
							(unsigned long long)packetTsNs,
							(long long)itemToSourceNs,
							(long long)sourceToCadenceNs,
							(long long)cadenceToLaunchNs,
							(long long)launchToTsNs,
							(long long)nowToLaunchNs,
							(long long)nowToTsNs,
							(unsigned long long)intervalNs,
							(unsigned long long)itemIntervalNs,
							usingSelectedInputClock ? 1U : 0U,
							usingLocalMediaClock ? 1U : 0U,
							crfGeneration,
							clockSelection.generation);
					}
					pPvtData->txDiagPrevPacketTsNs = packetTsNs;
					pPvtData->txDiagPrevItemBaseNs = itemBaseNs;
					pPvtData->txDiagPrevSourceBaseNs = sourceBaseNs;
					pPvtData->txDiagPrevCadenceBaseNs = baseNs;
					pPvtData->txDiagPrevLaunchTimeNs = launchTimeNs;
					pPvtData->txDiagPrevReadIdx = pMediaQItem->readIdx;
					pPvtData->txDiagPrevPacketIndex = packetIndex;
					pPvtData->txDiagPrevClockGeneration = clockSelection.generation;
					pPvtData->txDiagPrevCrfGeneration = crfGeneration;
					pPvtData->txDiagPrevPacketTsValid = TRUE;
				}
					else if (dropThisPacketForWarmup) {
						pPvtData->txDiagPrevPacketTsValid = FALSE;
					pPvtData->txDiagPrevPacketTsNs = 0;
					pPvtData->txDiagPrevItemBaseNs = 0;
					pPvtData->txDiagPrevSourceBaseNs = 0;
					pPvtData->txDiagPrevCadenceBaseNs = 0;
					pPvtData->txDiagPrevLaunchTimeNs = 0;
					pPvtData->txDiagPrevReadIdx = 0;
					pPvtData->txDiagPrevPacketIndex = 0;
						pPvtData->txDiagPrevClockGeneration = 0;
						pPvtData->txDiagPrevCrfGeneration = 0;
						pPvtData->selectedClockPresentationValid = FALSE;
						mapAafResetTxItem0Diag(pPvtData);
					}

				if (!dropThisPacketForWarmup) {
					U64 mapperLaunchNs = launchTimeNs;
					U64 mapperPacketTsNs = packetTsNs;
					U64 mapperLaunchAfterSkewNs = mapperLaunchNs;
					S64 mapperLaunchDeltaNs;
					S64 mapperPacketTsDeltaNs;
					S64 mapperLaunchAfterSkewDeltaNs;
					U32 mapperLaunchLogEveryPackets = pPvtData->txLaunchDiagEveryPackets;
					bool mapperLaunchStale = FALSE;

					if (pPvtData->txLaunchSkewUsec != 0) {
						S64 skewNs = (S64)pPvtData->txLaunchSkewUsec * (S64)NANOSECONDS_PER_USEC;
						if (skewNs >= 0) {
							mapperLaunchAfterSkewNs += (U64)skewNs;
						}
						else {
							U64 absSkewNs = (U64)(-skewNs);
							mapperLaunchAfterSkewNs =
								(mapperLaunchAfterSkewNs > absSkewNs)
									? (mapperLaunchAfterSkewNs - absSkewNs)
									: 0;
						}
					}

					if (nowNs == 0) {
						CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
					}

					mapperLaunchDeltaNs = (mapperLaunchNs >= nowNs)
						? (S64)(mapperLaunchNs - nowNs)
						: -((S64)(nowNs - mapperLaunchNs));
					mapperPacketTsDeltaNs = (mapperPacketTsNs >= nowNs)
						? (S64)(mapperPacketTsNs - nowNs)
						: -((S64)(nowNs - mapperPacketTsNs));
					mapperLaunchAfterSkewDeltaNs = (mapperLaunchAfterSkewNs >= nowNs)
						? (S64)(mapperLaunchAfterSkewNs - nowNs)
						: -((S64)(nowNs - mapperLaunchAfterSkewNs));

					mapperLaunchStale =
						(mapperLaunchAfterSkewDeltaNs < -((S64)NANOSECONDS_PER_MSEC));

					pPvtData->txLaunchDiagPacketCounter++;
					if (mapperLaunchStale) {
						pPvtData->txLaunchStaleCount++;
					}

					if (mapperLaunchLogEveryPackets == 0) {
						mapperLaunchLogEveryPackets =
							(pPvtData->txInterval > 0) ? (pPvtData->txInterval * 4U) : 32000U;
					}

					if ((mapperLaunchStale &&
							(pPvtData->txLaunchStaleCount <= 16 ||
								(pPvtData->txLaunchStaleCount % 2048ULL) == 0ULL)) ||
							(!mapperLaunchStale &&
							 mapperLaunchLogEveryPackets > 0 &&
							 (pPvtData->txLaunchDiagPacketCounter % mapperLaunchLogEveryPackets) == 0ULL)) {
						pPvtData->txLaunchDiagLogCount++;
						AVB_LOGF_WARNING(
							"AAF TX mapper launch %s: stream=%u name=%s packet=%u read_idx=%u pack=%u selected=%u local=%u uncertain=%u warmup_drop=%u settle_mute=%u item=%llu source_base=%llu cadence_base=%llu launch=%llu launch_skewed=%llu packet_ts=%llu now=%llu launch_delta=%lldns launch_skewed_delta=%lldns ts_delta=%lldns interval=%lluns item_step=%lluns lead=%uus transit=%uus trim=%dus crf_gen=%u clock_gen=%u recovery_gen=%u stale_events=%llu logs=%llu",
							mapperLaunchStale ? "stale" : "diag",
							pMediaQ->debug_stream_uid,
							pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
							packetIndex,
							pMediaQItem->readIdx,
							pPubMapInfo->packingFactor,
							usingSelectedInputClock ? 1U : 0U,
							usingLocalMediaClock ? 1U : 0U,
							timestampUncertain ? 1U : 0U,
							dropThisPacketForWarmup ? 1U : 0U,
							muteThisPacketForSettle ? 1U : 0U,
							(unsigned long long)itemBaseNs,
							(unsigned long long)sourceBaseNs,
							(unsigned long long)baseNs,
							(unsigned long long)mapperLaunchNs,
							(unsigned long long)mapperLaunchAfterSkewNs,
							(unsigned long long)mapperPacketTsNs,
							(unsigned long long)nowNs,
							(long long)mapperLaunchDeltaNs,
							(long long)mapperLaunchAfterSkewDeltaNs,
							(long long)mapperPacketTsDeltaNs,
							(unsigned long long)intervalNs,
							(unsigned long long)itemIntervalNs,
							pPvtData->txMinLeadUsec,
							pPvtData->maxTransitUsec,
							pPvtData->selectedClockTrimUsec,
							crfGeneration,
							clockSelection.generation,
							recoveryGeneration,
							(unsigned long long)pPvtData->txLaunchStaleCount,
							(unsigned long long)pPvtData->txLaunchDiagLogCount);
					}

					// Set timestamp valid flag
					pHdrV0[HIDX_AVTP_HIDE7_TV1] |= 0x01;

					// Set (clear) timestamp uncertain flag
					if (timestampUncertain)
						pHdrV0[HIDX_AVTP_HIDE7_TU1] |= 0x01;
					else pHdrV0[HIDX_AVTP_HIDE7_TU1] &= ~0x01;

					// - 4 bytes	avtp_timestamp
					U32 avtpTs = (U32)(packetTsNs & 0xFFFFFFFF);
					*pHdr++ = htonl(avtpTs);

					if (pPvtData->txLeadLogEveryPackets > 0) {
						pPvtData->txLeadLogCounter++;
						if ((pPvtData->txLeadLogCounter % pPvtData->txLeadLogEveryPackets) == 0) {
							S32 baseLeadUsec = openavbAvtpTimeUsecDelta(pMediaQItem->pAvtpTime);
							S64 packetOffsetUsec = (S64)((intervalNs * (U64)packetIndex) / NANOSECONDS_PER_USEC);
							S64 captureToSelectedUsec = 0;
							S64 captureToCadenceUsec = 0;
							S64 finalLeadUsec;
							captureToSelectedUsec = (sourceBaseNs >= itemBaseNs)
								? (S64)((sourceBaseNs - itemBaseNs) / NANOSECONDS_PER_USEC)
								: -((S64)((itemBaseNs - sourceBaseNs) / NANOSECONDS_PER_USEC));
							captureToCadenceUsec = (baseNs >= itemBaseNs)
								? (S64)((baseNs - itemBaseNs) / NANOSECONDS_PER_USEC)
								: -((S64)((itemBaseNs - baseNs) / NANOSECONDS_PER_USEC));
							if (pPvtData->txMinLeadUsec > 0) {
								if (nowNs == 0) {
									CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
								}
								finalLeadUsec = (packetTsNs >= nowNs)
									? (S64)((packetTsNs - nowNs) / NANOSECONDS_PER_USEC)
									: -((S64)((nowNs - packetTsNs) / NANOSECONDS_PER_USEC));
							}
							else {
								finalLeadUsec = (S64)baseLeadUsec + (S64)pPvtData->maxTransitUsec + packetOffsetUsec;
							}
							AVB_LOGF_INFO("AAF TX lead: stream=%u name=%s final=%lldus base=%dus transit=%uus packet=%u offset=%lldus tx_interval=%u launch_lead=%u clamp=%u capture_to_selected=%lldus capture_to_cadence=%lldus",
									pMediaQ->debug_stream_uid,
									pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
							(long long)finalLeadUsec,
							baseLeadUsec,
							pPvtData->maxTransitUsec,
							packetIndex,
							(long long)packetOffsetUsec,
							pPvtData->txInterval,
							pPvtData->txMinLeadUsec,
							clamped ? 1U : 0U,
							(long long)captureToSelectedUsec,
							(long long)captureToCadenceUsec);
						}
					}
				}

#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
				if (!dropThisPacketForWarmup) {
					pPvtData->lastLaunchTimeNs = launchTimeNs;
					pPvtData->lastLaunchTimeValid = TRUE;
				}
				else {
					pPvtData->lastLaunchTimeNs = 0;
					pPvtData->lastLaunchTimeValid = FALSE;
				}
#endif
			}

			if (dropThisPacketForWarmup) {
				dropForWarmup = TRUE;
			}

			if (!dropThisPacketForWarmup) {
				// - 4 bytes	format info (format, sample rate, channels per frame, bit depth)
				tmp32 = pPvtData->aaf_format << 24;
				tmp32 |= pPvtData->aaf_rate  << 20;
				tmp32 |= pPubMapInfo->audioChannels << 8;
				tmp32 |= pPvtData->aaf_bit_depth;
				*pHdr++ = htonl(tmp32);

				// - 4 bytes	packet info (data length, evt field)
				tmp32 = pPvtData->payloadSize << 16;
				tmp32 |= pPvtData->aaf_event_field << 8;
				*pHdr++ = htonl(tmp32);

				// Set (clear) sparse mode flag
				if (pPvtData->sparseMode == TS_SPARSE_MODE_ENABLED) {
					pHdrV0[HIDX_AVTP_HIDE7_SP] |= SP_M0_BIT;
				} else {
					pHdrV0[HIDX_AVTP_HIDE7_SP] &= ~SP_M0_BIT;
				}
			}

			if ((pMediaQItem->dataLen - pMediaQItem->readIdx) < pPvtData->payloadSize) {
				// This should not happen so we will just toss it away.
				AVB_LOG_ERROR("Not enough data in media queue item for packet");
				openavbMediaQTailPull(pMediaQ);
				AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
				return TX_CB_RET_PACKET_NOT_READY;
			}

			if (!dropThisPacketForWarmup) {
				if (muteThisPacketForSettle) {
					memset(pPayload, 0, pPvtData->payloadSize);
				}
				else {
					/*
					 * AAF audio samples are serialized on the wire in network byte
					 * order. The MediaQ item keeps the interface's native sample
					 * order, so little-endian ALSA capture must be swapped here.
					 */
					mapAafCopyPayloadToNetworkOrder(
						pPayload,
						(uint8_t *)pMediaQItem->pPubData + pMediaQItem->readIdx,
						pPvtData->payloadSize,
						pPubMapInfo,
						pPvtData);
				}
				pPayload += pPvtData->payloadSize;
			}
			bytesProcessed += pPvtData->payloadSize;
			pMediaQItem->readIdx += pPvtData->payloadSize;
			if (pMediaQItem->readIdx >= pMediaQItem->dataLen) {
				// Finished reading the entire item
				openavbAvtpTimeSetTimestampValid(pMediaQItem->pAvtpTime, FALSE);
				openavbMediaQTailPull(pMediaQ);
			}
			else {
				// More to read next interval
				openavbMediaQTailUnlock(pMediaQ);
			}
			if (dropThisPacketForWarmup) {
				pPvtData->selectedClockWarmupDropCount++;
			}
			if (muteThisPacketForSettle) {
				pPvtData->selectedClockMutePacketCount++;
			}
		}
		else {
			openavbMediaQTailPull(pMediaQ);
		}
	}

	// Set out bound data length (entire packet length)
	*dataLen = bytesNeeded + TOTAL_HEADER_SIZE;
	if (dropForWarmup) {
#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
		pPvtData->lastLaunchTimeNs = 0;
		pPvtData->lastLaunchTimeValid = FALSE;
#endif
		*dataLen = 0;
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return TX_CB_RET_PACKET_NOT_READY;
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return TX_CB_RET_PACKET_READY;
}

#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
bool openavbMapAVTPAudioLTCalcCB(media_q_t *pMediaQ, U64 *lt)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);

	if (!pMediaQ || !lt) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
	if (!pPvtData || !pPvtData->lastLaunchTimeValid) {
		AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
		return FALSE;
	}

	*lt = pPvtData->lastLaunchTimeNs;
	if (pPvtData->txLaunchSkewUsec != 0) {
		S64 skewNs = (S64)pPvtData->txLaunchSkewUsec * (S64)NANOSECONDS_PER_USEC;
		if (skewNs >= 0) {
			*lt += (U64)skewNs;
		}
		else {
			U64 absSkewNs = (U64)(-skewNs);
			*lt = (*lt > absSkewNs) ? (*lt - absSkewNs) : 0;
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return TRUE;
}
#endif

// A call to this callback indicates that this mapping module will be
// a listener. Any listener initialization can be done in this function.
void openavbMapAVTPAudioRxInitCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return;
		}
		pPvtData->isTalker = FALSE;
		if (pPvtData->audioMcr != AVB_MCR_NONE) {
			HAL_INIT_MCR_V2(pPvtData->txInterval, pPvtData->packingFactor, pPvtData->mcrTimestampInterval, pPvtData->mcrRecoveryInterval);
		}
		bool badPckFctrValue = FALSE;
		if (pPvtData->sparseMode == TS_SPARSE_MODE_ENABLED) {
			// sparse mode enabled so check packing factor
			// listener should work correct for packing_factors:
			// 1, 2, 4, 8, 16, 24, 32, 40, 48, (+8) ...
			if (pPvtData->packingFactor == 0) {
				badPckFctrValue = TRUE;
			}
			else if (pPvtData->packingFactor < 8) {
				// check if power of 2
				if ((pPvtData->packingFactor & (pPvtData->packingFactor - 1)) != 0) {
					badPckFctrValue = TRUE;
				}
			}
			else {
				// check if multiple of 8
				if (pPvtData->packingFactor % 8 != 0) {
					badPckFctrValue = TRUE;
				}
			}
			if (badPckFctrValue) {
				AVB_LOGF_WARNING("Wrong packing factor value set (%d) for sparse timestamping mode", pPvtData->packingFactor);
			}
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

// This callback occurs when running as a listener and data is available.
bool openavbMapAVTPAudioRxCB(media_q_t *pMediaQ, U8 *pData, U32 dataLen)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP_DETAIL);
	if (pMediaQ && pData) {
		U8 *pHdrV0 = pData;
		U32 *pHdr = (U32 *)(pData + AVTP_V0_HEADER_SIZE);
		U8  *pPayload = pData + TOTAL_HEADER_SIZE;
		media_q_pub_map_aaf_audio_info_t *pPubMapInfo = pMediaQ->pPubMapInfo;
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return FALSE;
		}

		aaf_sample_format_t incoming_aaf_format;
		U8 incoming_bit_depth;
		int tmp;
		bool dataValid = TRUE;
		bool dataConversionEnabled = FALSE;

		U32 timestamp = ntohl(*pHdr++);
		U32 format_info = ntohl(*pHdr++);
		U32 packet_info = ntohl(*pHdr++);

		bool listenerSparseMode = (pPvtData->sparseMode == TS_SPARSE_MODE_ENABLED) ? TRUE : FALSE;
		bool streamSparseMode = (pHdrV0[HIDX_AVTP_HIDE7_SP] & SP_M0_BIT) ? TRUE : FALSE;
		U16 payloadLen = ntohs(*(U16 *)(&pHdrV0[HIDX_STREAM_DATA_LEN16]));

		if (payloadLen > dataLen - TOTAL_HEADER_SIZE) {
			if (pPvtData->dataValid)
				AVB_LOGF_ERROR("header data len %d > actual data len %d",
					       payloadLen, dataLen - TOTAL_HEADER_SIZE);
			dataValid = FALSE;
		}

		if ((incoming_aaf_format = (aaf_sample_format_t) ((format_info >> 24) & 0xFF)) != pPvtData->aaf_format) {
			// Check if we can convert the incoming data.
			if (incoming_aaf_format >= AAF_FORMAT_INT_32 && incoming_aaf_format <= AAF_FORMAT_INT_16 &&
					pPvtData->aaf_format >= AAF_FORMAT_INT_32 && pPvtData->aaf_format <= AAF_FORMAT_INT_16) {
				// Integer conversion should be supported.
				dataConversionEnabled = TRUE;
			}
			else {
				if (pPvtData->dataValid)
					AVB_LOGF_ERROR("Listener format %d doesn't match received data (%d)",
						pPvtData->aaf_format, incoming_aaf_format);
				dataValid = FALSE;
			}
		}
		if ((tmp = ((format_info >> 20) & 0x0F)) != pPvtData->aaf_rate) {
			if (pPvtData->dataValid)
				AVB_LOGF_ERROR("Listener sample rate (%d) doesn't match received data (%d)",
					pPvtData->aaf_rate, tmp);
			dataValid = FALSE;
		}
		if ((tmp = ((format_info >> 8) & 0x3FF)) != pPubMapInfo->audioChannels) {
			if (pPvtData->dataValid)
				AVB_LOGF_ERROR("Listener channel count (%d) doesn't match received data (%d)",
					pPubMapInfo->audioChannels, tmp);
			dataValid = FALSE;
		}
		if ((incoming_bit_depth = (U8) (format_info & 0xFF)) == 0) {
			if (pPvtData->dataValid)
				AVB_LOGF_ERROR("Listener bit depth (%d) not valid",
					incoming_bit_depth);
			dataValid = FALSE;
		}
		if ((tmp = ((packet_info >> 16) & 0xFFFF)) != pPvtData->payloadSize) {
			if (!dataConversionEnabled) {
				if (pPvtData->dataValid)
					AVB_LOGF_ERROR("Listener payload size (%d) doesn't match received data (%d)",
						pPvtData->payloadSize, tmp);
				dataValid = FALSE;
			}
			else {
				int nInSampleLength = 6 - incoming_aaf_format; // Calculate the number of integer bytes per sample received
				int nOutSampleLength = 6 - pPvtData->aaf_format; // Calculate the number of integer bytes per sample we want
				if (tmp / nInSampleLength != pPvtData->payloadSize / nOutSampleLength) {
					if (pPvtData->dataValid)
						AVB_LOGF_ERROR("Listener payload samples (%d) doesn't match received data samples (%d)",
							pPvtData->payloadSize / nOutSampleLength, tmp / nInSampleLength);
					dataValid = FALSE;
				}
			}
		}
		if ((tmp = ((packet_info >> 8) & 0x0F)) != pPvtData->aaf_event_field) {
			if (pPvtData->dataValid)
				AVB_LOGF_ERROR("Listener event field (%d) doesn't match received data (%d)",
					pPvtData->aaf_event_field, tmp);
		}
		if (streamSparseMode && !listenerSparseMode) {
			AVB_LOG_INFO("Listener enabling sparse mode to match incoming stream");
			pPvtData->sparseMode = TS_SPARSE_MODE_ENABLED;
			listenerSparseMode = TRUE;
		}
		if (!streamSparseMode && listenerSparseMode) {
			AVB_LOG_INFO("Listener disabling sparse mode to match incoming stream");
			pPvtData->sparseMode = TS_SPARSE_MODE_DISABLED;
			listenerSparseMode = FALSE;
		}

		if (dataValid) {
			if (!pPvtData->dataValid) {
				AVB_LOG_INFO("RX data valid, stream un-muted");
				pPvtData->dataValid = TRUE;
			}

			// Get item pointer in media queue
			media_q_item_t *pMediaQItem = openavbMediaQHeadLock(pMediaQ);
			if (pMediaQItem) {
				// set timestamp if first data written to item
				if (pMediaQItem->dataLen == 0) {

					// Set timestamp valid flag
					openavbAvtpTimeSetTimestampValid(pMediaQItem->pAvtpTime, (pHdrV0[HIDX_AVTP_HIDE7_TV1] & 0x01) ? TRUE : FALSE);

					if (openavbAvtpTimeTimestampIsValid(pMediaQItem->pAvtpTime)) {
						// Get the timestamp and place it in the media queue item.
						openavbAvtpTimeSetToTimestamp(pMediaQItem->pAvtpTime, timestamp);

						openavbAvtpTimeSubUSec(pMediaQItem->pAvtpTime, pPubMapInfo->presentationLatencyUSec);

						// Set timestamp uncertain flag
						openavbAvtpTimeSetTimestampUncertain(pMediaQItem->pAvtpTime, (pHdrV0[HIDX_AVTP_HIDE7_TU1] & 0x01) ? TRUE : FALSE);
						// Set flag to inform that MediaQ is synchronized with timestamped packets
						 pPvtData->mediaQItemSyncTS = TRUE;
					}
					else if (!pPvtData->mediaQItemSyncTS) {
						//we need packet with valid TS for first data written to item
						AVB_LOG_DEBUG("Timestamp not valid for MediaQItem - initial packets dropped");
						IF_LOG_INTERVAL(1000) AVB_LOG_ERROR("Timestamp not valid for MediaQItem - initial packets dropped");
						dataValid = FALSE;
					}
				}
				if (dataValid) {
					if (!dataConversionEnabled) {
						// Just use the raw incoming data, and ignore the incoming bit_depth.
						if (pPubMapInfo->intf_rx_translate_cb) {
							pPubMapInfo->intf_rx_translate_cb(pMediaQ, pPayload, pPvtData->payloadSize);
						}

						memcpy((uint8_t *)pMediaQItem->pPubData + pMediaQItem->dataLen, pPayload, pPvtData->payloadSize);
					}
					else {
						static U8 s_audioBuffer[1500];
						U8 *pInData = pPayload;
						U8 *pInDataEnd = pPayload + payloadLen;
						U8 *pOutData = s_audioBuffer;
						int nInSampleLength = 6 - incoming_aaf_format; // Calculate the number of integer bytes per sample received
						int nOutSampleLength = 6 - pPvtData->aaf_format; // Calculate the number of integer bytes per sample we want
						int i;
						if (nInSampleLength < nOutSampleLength) {
							// We need to pad the data supplied.
							while (pInData < pInDataEnd) {
								for (i = 0; i < nInSampleLength; ++i) {
									*pOutData++ = *pInData++;
								}
								for ( ; i < nOutSampleLength; ++i) {
									*pOutData++ = 0; // Value specified in Clause 7.3.4.
								}
							}
						}
						else {
							// We need to truncate the data supplied.
							while (pInData < pInDataEnd) {
								for (i = 0; i < nOutSampleLength; ++i) {
									*pOutData++ = *pInData++;
								}
								pInData += (nInSampleLength - nOutSampleLength);
							}
						}
						if (pOutData - s_audioBuffer != pPvtData->payloadSize) {
							AVB_LOGF_ERROR("Output not expected size (%d instead of %d)", pOutData - s_audioBuffer, pPvtData->payloadSize);
						}

						if (pPubMapInfo->intf_rx_translate_cb) {
							pPubMapInfo->intf_rx_translate_cb(pMediaQ, s_audioBuffer, pPvtData->payloadSize);
						}

						memcpy((uint8_t *)pMediaQItem->pPubData + pMediaQItem->dataLen, s_audioBuffer, pPvtData->payloadSize);
					}

					pMediaQItem->dataLen += pPvtData->payloadSize;
				}

				if (pMediaQItem->dataLen < pMediaQItem->itemSize) {
					// More data can be written to the item
					openavbMediaQHeadUnlock(pMediaQ);
				}
				else {
					// The item is full push it.
					openavbMediaQHeadPush(pMediaQ);
				}

				AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
				return TRUE;    // Normal exit
			}
			else {
				IF_LOG_INTERVAL(1000) AVB_LOG_ERROR("Media queue full");
				AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
				return FALSE;   // Media queue full
			}
		}
		else {
			if (pPvtData->dataValid) {
				AVB_LOG_INFO("RX data invalid, stream muted");
				pPvtData->dataValid = FALSE;
			}
		}
	}
	AVB_TRACE_EXIT(AVB_TRACE_MAP_DETAIL);
	return FALSE;
}

// This callback will be called when the mapping module needs to be closed.
// All cleanup should occur in this function.
void openavbMapAVTPAudioEndCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);

	if (pMediaQ) {
		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;
		if (!pPvtData) {
			AVB_LOG_ERROR("Private mapping module data not allocated.");
			return;
		}

		if (pPvtData->audioMcr != AVB_MCR_NONE) {
			HAL_CLOSE_MCR_V2();
		}

		pPvtData->mediaQItemSyncTS = FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

void openavbMapAVTPAudioGenEndCB(media_q_t *pMediaQ)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);
	AVB_TRACE_EXIT(AVB_TRACE_MAP);
}

// Initialization entry point into the mapping module. Will need to be included in the .ini file.
extern DLL_EXPORT bool openavbMapAVTPAudioInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec)
{
	AVB_TRACE_ENTRY(AVB_TRACE_MAP);

	if (pMediaQ) {
		pMediaQ->pMediaQDataFormat = strdup(MapAVTPAudioMediaQDataFormat);
		pMediaQ->pPubMapInfo = calloc(1, sizeof(media_q_pub_map_aaf_audio_info_t));      // Memory freed by the media queue when the media queue is destroyed.
		pMediaQ->pPvtMapInfo = calloc(1, sizeof(pvt_data_t));                            // Memory freed by the media queue when the media queue is destroyed.

		if (!pMediaQ->pMediaQDataFormat || !pMediaQ->pPubMapInfo || !pMediaQ->pPvtMapInfo) {
			AVB_LOG_ERROR("Unable to allocate memory for mapping module");
			return FALSE;
		}

		pvt_data_t *pPvtData = pMediaQ->pPvtMapInfo;

		pMapCB->map_cfg_cb = openavbMapAVTPAudioCfgCB;
		pMapCB->map_subtype_cb = openavbMapAVTPAudioSubtypeCB;
		pMapCB->map_avtp_version_cb = openavbMapAVTPAudioAvtpVersionCB;
		pMapCB->map_max_data_size_cb = openavbMapAVTPAudioMaxDataSizeCB;
		pMapCB->map_transmit_interval_cb = openavbMapAVTPAudioTransmitIntervalCB;
		pMapCB->map_gen_init_cb = openavbMapAVTPAudioGenInitCB;
		pMapCB->map_tx_init_cb = openavbMapAVTPAudioTxInitCB;
		pMapCB->map_tx_cb = openavbMapAVTPAudioTxCB;
		pMapCB->map_rx_init_cb = openavbMapAVTPAudioRxInitCB;
		pMapCB->map_rx_cb = openavbMapAVTPAudioRxCB;
		pMapCB->map_end_cb = openavbMapAVTPAudioEndCB;
		pMapCB->map_gen_end_cb = openavbMapAVTPAudioGenEndCB;
#if ATL_LAUNCHTIME_ENABLED || IGB_LAUNCHTIME_ENABLED || SOCKET_LAUNCHTIME_ENABLED
		pMapCB->map_lt_calc_cb = openavbMapAVTPAudioLTCalcCB;
#endif

		pPvtData->itemCount = 20;
		pPvtData->txInterval = 4000;  // default to something that wont cause divide by zero
		pPvtData->packingFactor = 1;
		pPvtData->maxTransitUsec = inMaxTransitUsec;
		pPvtData->sparseMode = TS_SPARSE_MODE_DISABLED;
		pPvtData->mcrTimestampInterval = 144;
		pPvtData->mcrRecoveryInterval = 512;
		pPvtData->aaf_event_field = AAF_STATIC_CHANNELS_LAYOUT;
		pPvtData->intervalCounter = 0;
		pPvtData->txLeadLogEveryPackets = 4000;
		pPvtData->txLeadLogCounter = 0;
		pPvtData->txPhaseDiagEveryPackets = 0;
		pPvtData->txMinLeadUsec = 1000;
		pPvtData->txDisableNetworkSwap = FALSE;
		pPvtData->selectedClockWarmupUsec = 0;
		pPvtData->selectedClockMuteUsec = 0;
		pPvtData->selectedClockPresentationOffsetUsec = 0;
		pPvtData->selectedClockFollowUpdates = TRUE;
		pPvtData->selectedClockPreferLocalMediaPhase = TRUE;
		pPvtData->useSelectedClockTimestamps = TRUE;
		pPvtData->itemTimestampIsPresentation = FALSE;
		pPvtData->mediaQItemSyncTS = FALSE;
		mapAafResetTxPhaseDiag(pPvtData);
		openavbMediaQSetMaxLatency(pMediaQ, inMaxTransitUsec);
		AVB_LOGF_INFO(
			"AAF map init: uid=%u name=%s transit=%u tx_lead=%u",
			pMediaQ->debug_stream_uid,
			pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
			inMaxTransitUsec,
			pPvtData->txMinLeadUsec);
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return TRUE;
}
