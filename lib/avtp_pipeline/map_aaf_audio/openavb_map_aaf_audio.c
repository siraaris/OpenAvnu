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
	U32 selectedClockWarmupUsec;

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
	U64 selectedClockCadenceBaseNs;
	U32 selectedClockCadenceGeneration;
	U32 selectedClockCadenceCrfGeneration;
	bool selectedClockFollowUpdates;
	bool selectedClockWarmupActive;
	bool selectedClockWarmupLogged;
	U64 selectedClockWarmupUntilNs;
	U64 selectedClockWarmupDropCount;
	U64 txCadenceSlewEventCount;
	U64 txCadenceHardRebaseCount;

	// TX timestamp continuity diagnostics.
	bool txDiagPrevPacketTsValid;
	U64 txDiagPrevPacketTsNs;
	U64 txDiagPacketCount;
	U64 txDiagAnomalyCount;

	// TX phase diagnostics against the currently selected stream/media clock.
	U32 txPhaseDiagEveryPackets;
	U64 txPhaseDiagPacketCounter;
	bool txPhaseDiagWindowValid;
	S64 txPhaseDiagErrMinNs;
	S64 txPhaseDiagErrMaxNs;
	S64 txPhaseDiagErrSumNs;
	S64 txPhaseDiagErrAbsMaxNs;
	U64 txPhaseDiagClampMaxNs;
	U64 txPhaseDiagWindowPackets;
	U64 txPhaseDiagLogCount;
	U32 txPhaseDiagSourceGeneration;
	bool txPhaseDiagUsingLocalMediaClock;

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
	pPvtData->txPhaseDiagErrMinNs = 0;
	pPvtData->txPhaseDiagErrMaxNs = 0;
	pPvtData->txPhaseDiagErrSumNs = 0;
	pPvtData->txPhaseDiagErrAbsMaxNs = 0;
	pPvtData->txPhaseDiagClampMaxNs = 0;
	pPvtData->txPhaseDiagWindowPackets = 0;
	pPvtData->txPhaseDiagLogCount = 0;
	pPvtData->txPhaseDiagSourceGeneration = 0;
	pPvtData->txPhaseDiagUsingLocalMediaClock = FALSE;
}

static void mapAafUpdateTxPhaseDiag(
	media_q_t *pMediaQ,
	media_q_pub_map_aaf_audio_info_t *pPubMapInfo,
	pvt_data_t *pPvtData,
	U64 sourcePacketTsNs,
	U64 unclampedPacketTsNs,
	U64 clampDeltaNs,
	const openavb_clock_source_runtime_t *pClockSelection,
	U32 crfGeneration,
	bool usingLocalMediaClock)
{
	if (!pMediaQ || !pPubMapInfo || !pPvtData || !pClockSelection) {
		return;
	}

	if (!pPvtData->txPhaseDiagWindowValid ||
			pPvtData->txPhaseDiagSourceGeneration != pClockSelection->generation ||
			pPvtData->txPhaseDiagUsingLocalMediaClock != usingLocalMediaClock) {
		pPvtData->txPhaseDiagWindowValid = TRUE;
		pPvtData->txPhaseDiagErrMinNs = 0;
		pPvtData->txPhaseDiagErrMaxNs = 0;
		pPvtData->txPhaseDiagErrSumNs = 0;
		pPvtData->txPhaseDiagErrAbsMaxNs = 0;
		pPvtData->txPhaseDiagClampMaxNs = 0;
		pPvtData->txPhaseDiagWindowPackets = 0;
		pPvtData->txPhaseDiagSourceGeneration = pClockSelection->generation;
		pPvtData->txPhaseDiagUsingLocalMediaClock = usingLocalMediaClock;
	}

	S64 phaseErrNs = (unclampedPacketTsNs >= sourcePacketTsNs)
		? (S64)(unclampedPacketTsNs - sourcePacketTsNs)
		: -((S64)(sourcePacketTsNs - unclampedPacketTsNs));
	S64 absErrNs = llabs(phaseErrNs);

	if (pPvtData->txPhaseDiagWindowPackets == 0) {
		pPvtData->txPhaseDiagErrMinNs = phaseErrNs;
		pPvtData->txPhaseDiagErrMaxNs = phaseErrNs;
	}
	else {
		if (phaseErrNs < pPvtData->txPhaseDiagErrMinNs) {
			pPvtData->txPhaseDiagErrMinNs = phaseErrNs;
		}
		if (phaseErrNs > pPvtData->txPhaseDiagErrMaxNs) {
			pPvtData->txPhaseDiagErrMaxNs = phaseErrNs;
		}
	}

	pPvtData->txPhaseDiagErrSumNs += phaseErrNs;
	if (absErrNs > pPvtData->txPhaseDiagErrAbsMaxNs) {
		pPvtData->txPhaseDiagErrAbsMaxNs = absErrNs;
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
		S64 avgErrNs = pPvtData->txPhaseDiagErrSumNs / (S64)pPvtData->txPhaseDiagWindowPackets;
		S64 avgErrMilliSamples = (avgErrNs * (S64)pPubMapInfo->audioRate * 1000LL) / (S64)NANOSECONDS_PER_SECOND;
		S64 absMaxMilliSamples = (pPvtData->txPhaseDiagErrAbsMaxNs * (S64)pPubMapInfo->audioRate * 1000LL) / (S64)NANOSECONDS_PER_SECOND;
		S64 avgWholeSamples = avgErrMilliSamples / 1000LL;
		S64 avgFracSamples = llabs(avgErrMilliSamples % 1000LL);
		S64 absWholeSamples = absMaxMilliSamples / 1000LL;
		S64 absFracSamples = llabs(absMaxMilliSamples % 1000LL);

		pPvtData->txPhaseDiagLogCount++;
		AVB_LOGF_INFO(
			"AAF TX phase delta: stream=%u name=%s avg=%lldns (%lld.%03lld samples) min=%lldns max=%lldns abs_max=%lldns (%lld.%03lld samples) clamp_max=%lluns packets=%llu source=%s gen=%u crf_gen=%u windows=%llu",
			pMediaQ->debug_stream_uid,
			pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
			(long long)avgErrNs,
			(long long)avgWholeSamples,
			(long long)avgFracSamples,
			(long long)pPvtData->txPhaseDiagErrMinNs,
			(long long)pPvtData->txPhaseDiagErrMaxNs,
			(long long)pPvtData->txPhaseDiagErrAbsMaxNs,
			(long long)absWholeSamples,
			(long long)absFracSamples,
			(unsigned long long)pPvtData->txPhaseDiagClampMaxNs,
			(unsigned long long)pPvtData->txPhaseDiagWindowPackets,
			usingLocalMediaClock ? "local_media" : "projected_crf",
			pClockSelection->generation,
			crfGeneration,
			(unsigned long long)pPvtData->txPhaseDiagLogCount);

		pPvtData->txPhaseDiagErrMinNs = 0;
		pPvtData->txPhaseDiagErrMaxNs = 0;
		pPvtData->txPhaseDiagErrSumNs = 0;
		pPvtData->txPhaseDiagErrAbsMaxNs = 0;
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
		else if (strcmp(name, "map_nv_selected_clock_follow_updates") == 0 ||
				strcmp(name, "map_nv_clock_follow_selected_stream") == 0) {
			char *pEnd;
			long tmp = strtol(value, &pEnd, 10);
			if (*pEnd == '\0') {
				pPvtData->selectedClockFollowUpdates = (tmp != 0);
			}
		}
		else if (strcmp(name, "map_nv_selected_clock_warmup_usec") == 0 ||
				strcmp(name, "map_nv_clock_warmup_usec") == 0) {
			char *pEnd;
			pPvtData->selectedClockWarmupUsec = strtol(value, &pEnd, 10);
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
		if (pPvtData) {
			pPvtData->isTalker = TRUE;
			pPvtData->selectedClockPendingGeneration = 0;
			pPvtData->selectedClockPendingLogged = FALSE;
			pPvtData->selectedClockCadenceValid = FALSE;
			pPvtData->selectedClockCadenceUsingMediaClock = FALSE;
			pPvtData->selectedClockCadenceBaseNs = 0;
			pPvtData->selectedClockCadenceGeneration = 0;
			pPvtData->selectedClockCadenceCrfGeneration = 0;
			pPvtData->selectedClockWarmupActive = FALSE;
			pPvtData->selectedClockWarmupLogged = FALSE;
			pPvtData->selectedClockWarmupUntilNs = 0;
			pPvtData->selectedClockWarmupDropCount = 0;
			pPvtData->txCadenceSlewEventCount = 0;
			pPvtData->txCadenceHardRebaseCount = 0;
			pPvtData->txDiagPrevPacketTsValid = FALSE;
			pPvtData->txDiagPrevPacketTsNs = 0;
			pPvtData->txDiagPacketCount = 0;
			pPvtData->txDiagAnomalyCount = 0;
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

static bool mapAafSelectedStreamClockPending(openavb_clock_source_runtime_t *pSelection)
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

	if (openavbClockSourceRuntimeGetMediaClockForLocation(
			selection.clock_source_location_type,
			selection.clock_source_location_index,
			&ignoredNs,
			&ignoredUncertain,
			&ignoredGeneration)) {
		return FALSE;
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
	if (mapAafSelectedStreamClockPending(&pendingSelection)) {
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
				U64 baseNs = openavbAvtpTimeGetAvtpTimeNS(pMediaQItem->pAvtpTime);
				U64 sourceBaseNs = baseNs;
				U64 intervalNs = 0;
				U64 itemIntervalNs = 0;
				U64 packetTsNs;
				U64 launchTimeNs = 0;
				U32 packetIndex = 0;
				U64 nowNs = 0;
				bool clamped = FALSE;
				bool timestampUncertain = openavbAvtpTimeTimestampIsUncertain(pMediaQItem->pAvtpTime);
				openavb_clock_source_runtime_t clockSelection = {0};
				U32 crfGeneration = 0;
				U32 recoveryGeneration = osalClockGetWalltimeRecoveryGeneration();
				bool usingLocalMediaClock = FALSE;
				bool usingSelectedInputClock =
					mapAafGetSelectedStreamClock(
						&sourceBaseNs,
						&timestampUncertain,
						&clockSelection,
						&crfGeneration,
						&usingLocalMediaClock);
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
							usingLocalMediaClock ? "local media phase" : "projected CRF time");
					}
					else {
						AVB_LOG_INFO("AAF TX clock source: using interface-provided media timestamp");
					}
					pPvtData->usingSelectedInputClock = usingSelectedInputClock;
					pPvtData->selectedClockGeneration = clockSelection.generation;
					pPvtData->selectedCrfGeneration = crfGeneration;
					pPvtData->selectedClockRecoveryGeneration = recoveryGeneration;
					pPvtData->txDiagPrevPacketTsValid = FALSE;
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
					}
				}

				// Selected CRF clocks can be produced at lower packet rates (e.g. 500 pps)
				// than AAF (e.g. 8000 pps). Synthesize an AAF-rate timeline from the selected
				// clock, and only rebase when the selected clock meaningfully diverges.
				if (usingSelectedInputClock && intervalNs > 0) {
					bool sourceSelectionChanged = (!pPvtData->selectedClockCadenceValid) ||
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
							 * safest behavior is to avoid introducing any runtime packet-step
							 * perturbation from fresh local phase samples.
							 */
							pPvtData->selectedClockCadenceBaseNs = nextCadenceNs;
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
							S64 cadenceErrNs = (baseNs >= nextCadenceNs)
								? (S64)(baseNs - nextCadenceNs)
								: -((S64)(nextCadenceNs - baseNs));
							// Pull toward CRF with bounded slew to avoid frequent hard phase jumps.
							// Hard rebase is only for very large discontinuities.
							S64 hardRebaseNs = (S64)(itemIntervalNs * 256ULL);
							S64 maxSlewNs = (S64)(itemIntervalNs * 4ULL);
							S64 phaseAdjustNs = cadenceErrNs / 8;
							if (phaseAdjustNs > maxSlewNs) {
								phaseAdjustNs = maxSlewNs;
							}
							else if (phaseAdjustNs < -maxSlewNs) {
								phaseAdjustNs = -maxSlewNs;
							}

							if (llabs(cadenceErrNs) > hardRebaseNs) {
								pPvtData->txCadenceHardRebaseCount++;
								if (pPvtData->txCadenceHardRebaseCount <= 16 ||
										(pPvtData->txCadenceHardRebaseCount % 2000) == 0) {
									AVB_LOGF_WARNING(
										"AAF TX clock cadence hard rebase: err=%lldns expected_step=%lluns gen=%u crf_gen=%u rebases=%llu",
										(long long)cadenceErrNs,
										(unsigned long long)itemIntervalNs,
										clockSelection.generation,
										crfGeneration,
										(unsigned long long)pPvtData->txCadenceHardRebaseCount);
								}
								pPvtData->selectedClockCadenceBaseNs = baseNs;
							}
							else {
								if (llabs(cadenceErrNs) > maxSlewNs) {
									pPvtData->txCadenceSlewEventCount++;
									if (pPvtData->txCadenceSlewEventCount <= 16 ||
											(pPvtData->txCadenceSlewEventCount % 5000) == 0) {
										AVB_LOGF_INFO(
											"AAF TX clock cadence slew: err=%lldns adj=%lldns expected_step=%lluns gen=%u crf_gen=%u slews=%llu",
											(long long)cadenceErrNs,
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
					baseNs = pPvtData->selectedClockCadenceBaseNs;
				}
				else {
					pPvtData->selectedClockCadenceValid = FALSE;
					pPvtData->selectedClockCadenceUsingMediaClock = FALSE;
				}
				launchTimeNs = baseNs + (intervalNs * packetIndex);
				if (usingSelectedInputClock && pPvtData->txMinLeadUsec > 0) {
					launchTimeNs += ((U64)pPvtData->txMinLeadUsec * NANOSECONDS_PER_USEC);
				}
				packetTsNs = launchTimeNs +
					((U64)pPvtData->maxTransitUsec * NANOSECONDS_PER_USEC);

				U64 unclampedPacketTsNs = packetTsNs;
				if (!usingSelectedInputClock && pPvtData->txMinLeadUsec > 0) {
					U64 minTsNs = 0;
					CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &nowNs);
					minTsNs = nowNs + ((U64)pPvtData->txMinLeadUsec * NANOSECONDS_PER_USEC);
					if (packetTsNs < minTsNs) {
						packetTsNs = minTsNs;
						clamped = TRUE;
					}
				}
				// If we clamped to maintain minimum lead, also pull the synthesized
				// cadence anchor forward. Without this, repeated clamping can force
				// per-packet timestamps to track callback jitter instead of txInterval.
				if (clamped && usingSelectedInputClock && pPvtData->selectedClockCadenceValid &&
						packetTsNs > unclampedPacketTsNs) {
					U64 clampDeltaNs = packetTsNs - unclampedPacketTsNs;
					pPvtData->selectedClockCadenceBaseNs += clampDeltaNs;
				}
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
					}
				}
				if (usingSelectedInputClock) {
					U64 sourcePacketTsNs = sourceBaseNs +
						((U64)pPvtData->maxTransitUsec * NANOSECONDS_PER_USEC) +
						(intervalNs * packetIndex);
					if (!dropThisPacketForWarmup) {
						U64 clampDeltaNs = (packetTsNs > unclampedPacketTsNs)
							? (packetTsNs - unclampedPacketTsNs)
							: 0;
						mapAafUpdateTxPhaseDiag(
							pMediaQ,
							pPubMapInfo,
							pPvtData,
							sourcePacketTsNs + ((usingSelectedInputClock && pPvtData->txMinLeadUsec > 0)
								? ((U64)pPvtData->txMinLeadUsec * NANOSECONDS_PER_USEC)
								: 0),
							unclampedPacketTsNs,
							clampDeltaNs,
							&clockSelection,
							crfGeneration,
							usingLocalMediaClock);
					}
				}

				// Detect timestamp cadence anomalies (e.g., 2ms steps on 8kpps stream).
				if (!dropThisPacketForWarmup && intervalNs > 0) {
					pPvtData->txDiagPacketCount++;
					if (pPvtData->txDiagPrevPacketTsValid) {
						S64 deltaNs = (packetTsNs >= pPvtData->txDiagPrevPacketTsNs)
							? (S64)(packetTsNs - pPvtData->txDiagPrevPacketTsNs)
							: -((S64)(pPvtData->txDiagPrevPacketTsNs - packetTsNs));
						S64 errNs = deltaNs - (S64)intervalNs;
						S64 thresholdNs = (S64)(intervalNs / 8); // 12.5% tolerance
						if (llabs(errNs) > thresholdNs) {
							pPvtData->txDiagAnomalyCount++;
							if (pPvtData->txDiagAnomalyCount <= 16 ||
									(pPvtData->txDiagAnomalyCount % 2000) == 0) {
								AVB_LOGF_WARNING(
									"AAF TX timestamp cadence anomaly: delta=%lldns expected=%lluns err=%lldns ts=%llu prev=%llu packets=%llu anomalies=%llu",
									(long long)deltaNs,
									(unsigned long long)intervalNs,
									(long long)errNs,
									(unsigned long long)packetTsNs,
									(unsigned long long)pPvtData->txDiagPrevPacketTsNs,
									(unsigned long long)pPvtData->txDiagPacketCount,
									(unsigned long long)pPvtData->txDiagAnomalyCount);
							}
						}
					}
					pPvtData->txDiagPrevPacketTsNs = packetTsNs;
					pPvtData->txDiagPrevPacketTsValid = TRUE;
				}
				else if (dropThisPacketForWarmup) {
					pPvtData->txDiagPrevPacketTsValid = FALSE;
				}

				if (!dropThisPacketForWarmup) {
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
							S64 finalLeadUsec;
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
							AVB_LOGF_INFO("AAF TX lead: stream=%u name=%s final=%lldus base=%dus transit=%uus packet=%u offset=%lldus tx_interval=%u launch_lead=%u clamp=%u",
									pMediaQ->debug_stream_uid,
									pMediaQ->debug_friendly_name[0] ? pMediaQ->debug_friendly_name : "<unknown>",
									(long long)finalLeadUsec,
									baseLeadUsec,
									pPvtData->maxTransitUsec,
									packetIndex,
									(long long)packetOffsetUsec,
									pPvtData->txInterval,
									pPvtData->txMinLeadUsec,
									clamped ? 1U : 0U);
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
				memcpy(pPayload, (uint8_t *)pMediaQItem->pPubData + pMediaQItem->readIdx, pPvtData->payloadSize);
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
		pPvtData->selectedClockWarmupUsec = 0;
		pPvtData->selectedClockFollowUpdates = TRUE;
		pPvtData->mediaQItemSyncTS = FALSE;
		mapAafResetTxPhaseDiag(pPvtData);
		openavbMediaQSetMaxLatency(pMediaQ, inMaxTransitUsec);
	}

	AVB_TRACE_EXIT(AVB_TRACE_MAP);
	return TRUE;
}
