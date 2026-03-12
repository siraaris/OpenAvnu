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
 * MODULE : AVDECC - Top level 1722.1 implementation
 * MODULE SUMMARY : Top level 1722.1 implementation
 ******************************************************************
 */

#include "openavb_rawsock.h"
#include "openavb_avtp.h"
#include <stdio.h>

#define	AVB_LOG_COMPONENT	"AVDECC"
#include "openavb_log.h"

#include "openavb_aem.h"
#include "openavb_adp.h"
#include "openavb_acmp.h"
#include "openavb_aecp.h"

#include "openavb_endpoint.h"

#define ADDR_PTR(A) (U8*)(&((A)->ether_addr_octet))

openavb_avdecc_cfg_t gAvdeccCfg;
openavb_tl_data_cfg_t * streamList = NULL;
openavb_aem_descriptor_configuration_t *pConfiguration = NULL;

static openavb_avdecc_configuration_cfg_t *pFirstConfigurationCfg = NULL;
static U8 talker_stream_sources = 0;
static U8 listener_stream_sources = 0;
static bool first_talker = 1;
static bool first_listener = 1;
static U16 talker_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
static U16 listener_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
static U16 audio_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
static U16 s_identifyControlIndex = OPENAVB_AEM_DESCRIPTOR_INVALID;
static U8 *s_entityLogoData = NULL;
static U64 s_entityLogoLength = 0;

#define OPENAVB_AVDECC_ENTITY_LOGO_PATH "/root/src/OpenAvnu/avnu_logo.png"
#define OPENAVB_AVDECC_ENTITY_LOGO_START_ADDRESS (0x4f41564c4f474f00ULL)

static void openavbAvdeccFreeEntityLogo(void)
{
	if (s_entityLogoData) {
		free(s_entityLogoData);
		s_entityLogoData = NULL;
	}
	s_entityLogoLength = 0;
}

static bool openavbAvdeccLoadEntityLogo(void)
{
	FILE *fp = NULL;
	long fileSize = 0;
	U8 *pData = NULL;
	size_t bytesRead = 0;

	openavbAvdeccFreeEntityLogo();

	fp = fopen(OPENAVB_AVDECC_ENTITY_LOGO_PATH, "rb");
	if (!fp) {
		AVB_LOGF_WARNING("Entity logo not loaded: %s", OPENAVB_AVDECC_ENTITY_LOGO_PATH);
		return FALSE;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		AVB_LOGF_WARNING("Entity logo load failed seeking %s", OPENAVB_AVDECC_ENTITY_LOGO_PATH);
		return FALSE;
	}

	fileSize = ftell(fp);
	if (fileSize < 0) {
		fclose(fp);
		AVB_LOGF_WARNING("Entity logo load failed sizing %s", OPENAVB_AVDECC_ENTITY_LOGO_PATH);
		return FALSE;
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		AVB_LOGF_WARNING("Entity logo load failed rewinding %s", OPENAVB_AVDECC_ENTITY_LOGO_PATH);
		return FALSE;
	}

	if (fileSize > 0) {
		pData = malloc((size_t)fileSize);
		if (!pData) {
			fclose(fp);
			AVB_LOG_ERROR("Entity logo load out of memory");
			return FALSE;
		}

		bytesRead = fread(pData, 1, (size_t)fileSize, fp);
		if (bytesRead != (size_t)fileSize) {
			fclose(fp);
			free(pData);
			AVB_LOGF_WARNING("Entity logo load failed reading %s", OPENAVB_AVDECC_ENTITY_LOGO_PATH);
			return FALSE;
		}
	}

	fclose(fp);
	s_entityLogoData = pData;
	s_entityLogoLength = (U64)fileSize;
	AVB_LOGF_INFO("Loaded entity logo: %s (%llu bytes)",
		OPENAVB_AVDECC_ENTITY_LOGO_PATH,
		(unsigned long long)s_entityLogoLength);
	return TRUE;
}

static bool openavbAvdeccIsCrfStream(const openavb_tl_data_cfg_t *stream)
{
	if (!stream) {
		return FALSE;
	}
	return (strcmp(stream->map_fn, "openavbMapCrfInitialize") == 0);
}

static bool openavbAvdeccIsAudioMappedStream(const openavb_tl_data_cfg_t *stream)
{
	if (!stream) {
		return FALSE;
	}

	return (strcmp(stream->map_fn, "openavbMapAVTPAudioInitialize") == 0 ||
		strcmp(stream->map_fn, "openavbMapUncmpAudioInitialize") == 0 ||
		strcmp(stream->map_fn, "openavbMapNullInitialize") == 0);
}

static bool openavbAvdeccUsesSharedAudioClockDomain(const openavb_avdecc_configuration_cfg_t *pCfg)
{
	return (pCfg &&
		pCfg->stream &&
		!pCfg->stream_is_crf &&
		openavbAvdeccIsAudioMappedStream(pCfg->stream));
}

static void openavbAvdeccFreeConfigurationCfgList(void)
{
	while (pFirstConfigurationCfg) {
		openavb_avdecc_configuration_cfg_t *pDel = pFirstConfigurationCfg;
		pFirstConfigurationCfg = pFirstConfigurationCfg->next;
		free(pDel);
	}
}

static void openavbAvdeccRefreshClockDomain(
	U16 nConfigIdx,
	U16 clockDomainIdx,
	const openavb_avdecc_configuration_cfg_t *pCfg)
{
	if (clockDomainIdx == OPENAVB_AEM_DESCRIPTOR_INVALID) {
		return;
	}

	openavb_aem_descriptor_clock_domain_t *pClockDomain =
		openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN, clockDomainIdx);
	if (!pClockDomain) {
		AVB_LOGF_WARNING("Unable to refresh missing clock domain idx=%u", clockDomainIdx);
		return;
	}

	if (!openavbAemDescriptorClockDomainInitialize(pClockDomain, nConfigIdx, pCfg)) {
		AVB_LOGF_WARNING("Unable to refresh clock domain idx=%u", clockDomainIdx);
	}
}

static void openavbAvdeccPreferClockDomainSource(
	U16 nConfigIdx,
	U16 clockDomainIdx,
	U16 preferredClockSourceIdx,
	bool forceSelection)
{
	openavb_aem_descriptor_clock_domain_t *pClockDomain;
	openavb_aem_descriptor_clock_source_t *pCurrentClockSource = NULL;
	bool currentIsStreamSource = FALSE;

	if (clockDomainIdx == OPENAVB_AEM_DESCRIPTOR_INVALID ||
			preferredClockSourceIdx == OPENAVB_AEM_DESCRIPTOR_INVALID) {
		return;
	}

	pClockDomain = openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN, clockDomainIdx);
	if (!pClockDomain) {
		AVB_LOGF_WARNING("Unable to prefer source %u on missing clock domain %u",
			preferredClockSourceIdx, clockDomainIdx);
		return;
	}

	if (pClockDomain->clock_source_index != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		pCurrentClockSource = openavbAemGetDescriptor(
			nConfigIdx,
			OPENAVB_AEM_DESCRIPTOR_CLOCK_SOURCE,
			pClockDomain->clock_source_index);
		if (pCurrentClockSource &&
				(pCurrentClockSource->clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID)) {
			currentIsStreamSource = TRUE;
		}
	}

	if (forceSelection || !currentIsStreamSource) {
		pClockDomain->clock_source_index = preferredClockSourceIdx;
		AVB_LOGF_INFO("Clock domain %u default source set to CRF source %u",
			clockDomainIdx, preferredClockSourceIdx);
	}
}

static bool openavbAvdeccAddClockSource(
	U16 nConfigIdx,
	openavb_avdecc_configuration_cfg_t *pCfg,
	U16 *pOutClockSourceIdx)
{
	U16 nResultIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavb_aem_descriptor_clock_source_t *pNewClockSource = openavbAemDescriptorClockSourceNew();
	if (!openavbAemAddDescriptor(pNewClockSource, nConfigIdx, &nResultIdx) ||
			!openavbAemDescriptorClockSourceInitialize(pNewClockSource, nConfigIdx, pCfg)) {
		AVB_LOG_ERROR("Error adding AVDECC Clock Source to configuration");
		return FALSE;
	}
	if (pOutClockSourceIdx) {
		*pOutClockSourceIdx = nResultIdx;
	}

	// Keep existing clock domains in sync with newly-added sources (e.g. CRF).
	openavbAvdeccRefreshClockDomain(nConfigIdx, talker_clock_domain_idx, pCfg);
	openavbAvdeccRefreshClockDomain(nConfigIdx, listener_clock_domain_idx, pCfg);
	openavbAvdeccRefreshClockDomain(nConfigIdx, audio_clock_domain_idx, pCfg);
	return TRUE;
}

static bool openavbAvdeccEnsureSharedAudioClockDomain(
	U16 nConfigIdx,
	openavb_avdecc_configuration_cfg_t *pCfg)
{
	U16 nResultIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavb_aem_descriptor_clock_domain_t *pNewClockDomain = NULL;

	if (audio_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		return TRUE;
	}

	pNewClockDomain = openavbAemDescriptorClockDomainNew();
	if (!openavbAemAddDescriptor(pNewClockDomain, nConfigIdx, &nResultIdx) ||
			!openavbAemDescriptorClockDomainInitialize(pNewClockDomain, nConfigIdx, pCfg)) {
		AVB_LOG_ERROR("Error adding shared AVDECC audio clock domain to configuration");
		return FALSE;
	}

	audio_clock_domain_idx = nResultIdx;
	return TRUE;
}

static void openavbAvdeccLogStreamDescriptorSummary(
	U16 nConfigIdx,
	const openavb_avdecc_configuration_cfg_t *pCfg)
{
	openavb_aem_descriptor_stream_io_t *pStreamDescriptor;
	U8 currentFormat[8];
	U8 supportedFormat[8];
	const char *descriptorLabel;
	const char *roleLabel;

	if (!pCfg) {
		return;
	}

	pStreamDescriptor = openavbAemGetDescriptor(
		nConfigIdx,
		pCfg->stream_descriptor_type,
		pCfg->stream_descriptor_index);
	if (!pStreamDescriptor) {
		return;
	}

	memset(currentFormat, 0, sizeof(currentFormat));
	memset(supportedFormat, 0, sizeof(supportedFormat));
	openavbAemStreamFormatToBuf(&pStreamDescriptor->current_format, currentFormat);
	if (pStreamDescriptor->number_of_formats > 0) {
		openavbAemStreamFormatToBuf(&pStreamDescriptor->stream_formats[0], supportedFormat);
	}

	descriptorLabel =
		(pCfg->stream_descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) ?
			"STREAM_INPUT" : "STREAM_OUTPUT";
	roleLabel = (pCfg->stream && pCfg->stream->role == AVB_ROLE_LISTENER) ? "listener" : "talker";

	AVB_LOGF_INFO(
		"%s %u summary: role=%s map=%s crf=%u domain=%u formats=%u current=%02x%02x%02x%02x%02x%02x%02x%02x supported0=%02x%02x%02x%02x%02x%02x%02x%02x",
		descriptorLabel,
		pCfg->stream_descriptor_index,
		roleLabel,
		(pCfg->stream && pCfg->stream->map_fn[0]) ? pCfg->stream->map_fn : "<unknown>",
		pCfg->stream_is_crf ? 1 : 0,
		pStreamDescriptor->clock_domain_index,
		pStreamDescriptor->number_of_formats,
		currentFormat[0], currentFormat[1], currentFormat[2], currentFormat[3],
		currentFormat[4], currentFormat[5], currentFormat[6], currentFormat[7],
		supportedFormat[0], supportedFormat[1], supportedFormat[2], supportedFormat[3],
		supportedFormat[4], supportedFormat[5], supportedFormat[6], supportedFormat[7]);
}

static bool openavbAvdeccAddGptpTimingChain(U16 nConfigIdx)
{
	U16 timingIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	U16 ptpInstanceIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	U16 ptpPortIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	U16 gptpClockSourceIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavb_aem_descriptor_timing_t *pTiming = NULL;
	openavb_aem_descriptor_ptp_instance_t *pPtpInstance = NULL;
	openavb_aem_descriptor_ptp_port_t *pPtpPort = NULL;
	openavb_aem_descriptor_clock_source_t *pClockSource = NULL;

	pTiming = openavbAemDescriptorTimingNew();
	pPtpInstance = openavbAemDescriptorPtpInstanceNew();
	pPtpPort = openavbAemDescriptorPtpPortNew();
	pClockSource = openavbAemDescriptorClockSourceNew();
	if (!pTiming || !pPtpInstance || !pPtpPort || !pClockSource) {
		AVB_LOG_ERROR("Error allocating Milan gPTP timing descriptors");
		return FALSE;
	}

	if (!openavbAemAddDescriptor(pTiming, nConfigIdx, &timingIdx) ||
			!openavbAemAddDescriptor(pPtpInstance, nConfigIdx, &ptpInstanceIdx) ||
			!openavbAemAddDescriptor(pPtpPort, nConfigIdx, &ptpPortIdx) ||
			!openavbAemAddDescriptor(pClockSource, nConfigIdx, &gptpClockSourceIdx)) {
		AVB_LOG_ERROR("Error adding Milan gPTP timing descriptors to configuration");
		return FALSE;
	}

	if (!openavbAemDescriptorTimingInitialize(pTiming, ptpInstanceIdx) ||
			!openavbAemDescriptorPtpInstanceInitialize(pPtpInstance, ptpPortIdx) ||
			!openavbAemDescriptorPtpPortInitialize(pPtpPort, 0) ||
			!openavbAemDescriptorClockSourceInitializeForTiming(pClockSource, timingIdx)) {
		AVB_LOG_ERROR("Error initializing Milan gPTP timing descriptors");
		return FALSE;
	}

	return TRUE;
}

static bool openavbAvdeccAddIdentifyControl(
	U16 nConfigIdx,
	const openavb_avdecc_configuration_cfg_t *pCfg)
{
	U16 identifyControlIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavb_aem_descriptor_control_t *pControl = openavbAemDescriptorControlNew();
	if (!pControl) {
		AVB_LOG_ERROR("Error allocating AVDECC IDENTIFY control");
		return FALSE;
	}

	if (!openavbAemAddDescriptor(pControl, nConfigIdx, &identifyControlIdx) ||
			!openavbAemDescriptorDescriptorControlInitialize(pControl, nConfigIdx, pCfg)) {
		AVB_LOG_ERROR("Error adding AVDECC IDENTIFY control");
		return FALSE;
	}

	openavbAemSetString(pControl->object_name, "Identify");
	pControl->control_value_type = OPENAVB_AEM_CONTROL_VALUE_TYPE_CONTROL_LINEAR_UINT8;
	pControl->control_type = OPENAVB_AEM_CONTROL_TYPE_IDENTIFY;
	pControl->number_of_values = 1;
	pControl->signal_type = OPENAVB_AEM_DESCRIPTOR_INVALID;
	pControl->signal_index = 0;
	pControl->signal_output = 0;
	pControl->value_details.linear_uint8[0].minimum = 0;
	pControl->value_details.linear_uint8[0].maximum = 255;
	pControl->value_details.linear_uint8[0].step = 255;
	pControl->value_details.linear_uint8[0].default_value = 0;
	pControl->value_details.linear_uint8[0].current = 0;
	pControl->value_details.linear_uint8[0].unit = 0;
	pControl->value_details.linear_uint8[0].string.offset = OPENAVB_AEM_NO_STRING_OFFSET;
	pControl->value_details.linear_uint8[0].string.index = OPENAVB_AEM_NO_STRING_INDEX;

	s_identifyControlIndex = identifyControlIdx;
	return TRUE;
}

static bool openavbAvdeccAddEntityLogoMemoryObject(U16 nConfigIdx)
{
	U16 memoryObjectIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavb_aem_descriptor_memory_object_t *pMemoryObject;

	if (!openavbAvdeccHasEntityLogo()) {
		return TRUE;
	}

	pMemoryObject = openavbAemDescriptorMemoryObjectNew();
	if (!pMemoryObject) {
		AVB_LOG_ERROR("Error allocating AVDECC entity logo memory object");
		return FALSE;
	}

	if (!openavbAemAddDescriptor(pMemoryObject, nConfigIdx, &memoryObjectIdx) ||
			!openavbAemDescriptorMemoryObjectInitialize(
				pMemoryObject,
				OPENAVB_AEM_MEMORY_OBJECT_TYPE_PNG_ENTITY,
				OPENAVB_AEM_DESCRIPTOR_ENTITY,
				0,
				OPENAVB_AVDECC_ENTITY_LOGO_START_ADDRESS,
				s_entityLogoLength,
				s_entityLogoLength,
				"Entity Logo")) {
		AVB_LOG_ERROR("Error adding AVDECC entity logo memory object");
		return FALSE;
	}

	return TRUE;
}

static void openavbAvdeccLogConfigurationDescriptorCounts(U16 nConfigIdx)
{
	openavb_aem_descriptor_configuration_t *pConfiguration;
	U16 i1;

	pConfiguration = openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_CONFIGURATION, nConfigIdx);
	if (!pConfiguration) {
		AVB_LOGF_WARNING("Unable to log descriptor counts for configuration %u", nConfigIdx);
		return;
	}

	AVB_LOGF_INFO("Configuration %u descriptor_counts_count=%u",
		nConfigIdx,
		pConfiguration->descriptor_counts_count);
	for (i1 = 0; i1 < pConfiguration->descriptor_counts_count; ++i1) {
		AVB_LOGF_INFO("Configuration %u descriptor_count[%u]: type=0x%04x count=%u",
			nConfigIdx,
			i1,
			pConfiguration->descriptor_counts[i1].descriptor_type,
			pConfiguration->descriptor_counts[i1].count);
	}
}

static openavb_avdecc_configuration_cfg_t *openavbAvdeccFindRepresentativeAudioConfig(avb_role_t role)
{
	openavb_avdecc_configuration_cfg_t *pCfg = pFirstConfigurationCfg;

	while (pCfg) {
		if (pCfg->stream &&
				pCfg->stream->role == role &&
				!pCfg->stream_is_crf &&
				openavbAvdeccIsAudioMappedStream(pCfg->stream)) {
			return pCfg;
		}
		pCfg = pCfg->next;
	}

	return NULL;
}

static U16 openavbAvdeccGetAudioChannelCount(const openavb_aem_descriptor_stream_io_t *pStreamDescriptor)
{
	if (!pStreamDescriptor) {
		return 0;
	}

	switch (pStreamDescriptor->current_format.subtype) {
		case OPENAVB_AEM_STREAM_FORMAT_AVTP_AUDIO_SUBTYPE:
			return (pStreamDescriptor->current_format.subtypes.avtp_audio.channels_per_frame != 0) ?
				pStreamDescriptor->current_format.subtypes.avtp_audio.channels_per_frame : 2;

		case OPENAVB_AEM_STREAM_FORMAT_61883_IIDC_SUBTYPE:
			if (pStreamDescriptor->current_format.subtypes.iec_61883.fmt == OPENAVB_AEM_STREAM_FORMAT_FMT_61883_6) {
				return (pStreamDescriptor->current_format.subtypes.iec_61883_6.dbs != 0) ?
					pStreamDescriptor->current_format.subtypes.iec_61883_6.dbs : 2;
			}
			break;

		default:
			break;
	}

	return 0;
}

static void openavbAvdeccPopulateDynamicAudioMappings(
	openavb_aem_descriptor_stream_port_io_t *pStreamPort,
	U16 channelCount)
{
	U16 mappingCount;
	U16 i;

	if (!pStreamPort) {
		return;
	}

	memset(pStreamPort->dynamic_mappings, 0, sizeof(pStreamPort->dynamic_mappings));
	pStreamPort->dynamic_mappings_supported = (channelCount != 0);
	pStreamPort->dynamic_number_of_maps = (channelCount != 0) ? 1 : 0;
	pStreamPort->dynamic_stream_channels = channelCount;
	pStreamPort->dynamic_cluster_channels = channelCount;

	if (channelCount == 0) {
		pStreamPort->dynamic_number_of_mappings = 0;
		return;
	}

	mappingCount = channelCount;
	if (mappingCount > OPENAVB_DESCRIPTOR_AUDIO_MAP_MAX_MAPPINGS) {
		AVB_LOGF_WARNING("Clipping dynamic audio mappings from %u to %u",
			mappingCount,
			OPENAVB_DESCRIPTOR_AUDIO_MAP_MAX_MAPPINGS);
		mappingCount = OPENAVB_DESCRIPTOR_AUDIO_MAP_MAX_MAPPINGS;
	}

	pStreamPort->dynamic_number_of_mappings = mappingCount;
	for (i = 0; i < mappingCount; i++) {
		pStreamPort->dynamic_mappings[i].mapping_stream_index = 0;
		pStreamPort->dynamic_mappings[i].mapping_stream_channel = i;
		pStreamPort->dynamic_mappings[i].mapping_cluster_offset = 0;
		pStreamPort->dynamic_mappings[i].mapping_cluster_channel = i;
	}
}

static bool openavbAvdeccConfigureAudioStreamPort(U16 nConfigIdx, avb_role_t role)
{
	openavb_avdecc_configuration_cfg_t *pCfg;
	openavb_aem_descriptor_stream_port_io_t *pStreamPort;
	openavb_aem_descriptor_stream_io_t *pStreamDescriptor;
	openavb_aem_descriptor_audio_cluster_t *pCluster;
	U16 streamPortType;
	U16 clockDomainIdx;
	U16 clusterIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	U16 channelCount;

	pCfg = openavbAvdeccFindRepresentativeAudioConfig(role);
	streamPortType = (role == AVB_ROLE_TALKER) ?
		OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_OUTPUT :
		OPENAVB_AEM_DESCRIPTOR_STREAM_PORT_INPUT;
	if (audio_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		clockDomainIdx = audio_clock_domain_idx;
	}
	else {
		clockDomainIdx = (role == AVB_ROLE_TALKER) ?
			talker_clock_domain_idx :
			listener_clock_domain_idx;
	}

	pStreamPort = openavbAemGetDescriptor(nConfigIdx, streamPortType, 0);
	if (!pStreamPort) {
		return TRUE;
	}

	pStreamPort->clock_domain_index = clockDomainIdx;
	pStreamPort->number_of_controls = 0;
	pStreamPort->base_control = OPENAVB_AEM_DESCRIPTOR_INVALID;
	pStreamPort->number_of_clusters = 0;
	pStreamPort->base_cluster = OPENAVB_AEM_DESCRIPTOR_INVALID;
	pStreamPort->number_of_maps = 0;
	pStreamPort->base_map = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavbAvdeccPopulateDynamicAudioMappings(pStreamPort, 0);

	if (!pCfg) {
		return TRUE;
	}

	pStreamDescriptor = openavbAemGetDescriptor(nConfigIdx, pCfg->stream_descriptor_type, pCfg->stream_descriptor_index);
	if (!pStreamDescriptor) {
		AVB_LOG_WARNING("Representative stream descriptor missing for dynamic audio mappings");
		return FALSE;
	}

	channelCount = openavbAvdeccGetAudioChannelCount(pStreamDescriptor);
	if (channelCount == 0) {
		return TRUE;
	}

	pCluster = openavbAemDescriptorAudioClusterNew();
	if (!pCluster || !openavbAemAddDescriptor(pCluster, OPENAVB_AEM_DESCRIPTOR_INVALID, &clusterIdx)) {
		AVB_LOG_ERROR("Error adding AVDECC Audio Cluster for dynamic audio mappings");
		return FALSE;
	}

	if (role == AVB_ROLE_TALKER) {
		openavbAemSetString(pCluster->object_name, "Output Audio Cluster");
	}
	else {
		openavbAemSetString(pCluster->object_name, "Input Audio Cluster");
	}
	pCluster->signal_type = streamPortType;
	pCluster->signal_index = 0;
	pCluster->signal_output = 0;
	pCluster->channel_count = channelCount;
	pCluster->format = 0;

	pStreamPort->number_of_clusters = 1;
	pStreamPort->base_cluster = clusterIdx;
	openavbAvdeccPopulateDynamicAudioMappings(pStreamPort, channelCount);
	return TRUE;
}

static void openavbAvdeccFinalizeAudioUnit(U16 nConfigIdx)
{
	openavb_aem_descriptor_audio_unit_t *pAudioUnit =
		openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_AUDIO_UNIT, 0);

	if (!pAudioUnit) {
		return;
	}

	if (audio_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		pAudioUnit->clock_domain_index = audio_clock_domain_idx;
	}
	else if (talker_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		pAudioUnit->clock_domain_index = talker_clock_domain_idx;
	}
	else if (listener_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
		pAudioUnit->clock_domain_index = listener_clock_domain_idx;
	}

	pAudioUnit->number_of_stream_input_ports = gAvdeccCfg.bListener ? 1 : 0;
	pAudioUnit->base_stream_input_port = gAvdeccCfg.bListener ? 0 : OPENAVB_AEM_DESCRIPTOR_INVALID;
	pAudioUnit->number_of_stream_output_ports = gAvdeccCfg.bTalker ? 1 : 0;
	pAudioUnit->base_stream_output_port = gAvdeccCfg.bTalker ? 0 : OPENAVB_AEM_DESCRIPTOR_INVALID;
}

bool openavbAvdeccStartAdp()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	openavbRC rc = openavbAdpStart();
	if (IS_OPENAVB_FAILURE(rc)) {
		openavbAdpStop();
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

void openavbAvdeccStopAdp()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);
	openavbAdpStop();
	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
}

bool openavbAvdeccStartCmp()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	openavbRC rc = openavbAcmpStart();
	if (IS_OPENAVB_FAILURE(rc)) {
		openavbAcmpStop();
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

void openavbAvdeccStopCmp()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);
	openavbAcmpStop();
	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
}

bool openavbAvdeccStartEcp()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	openavbRC rc = openavbAecpStart();
	if (IS_OPENAVB_FAILURE(rc)) {
		openavbAecpStop();
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

void openavbAvdeccStopEcp()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);
	openavbAecpStop();
	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
}

void openavbAvdeccFindMacAddr(void)
{
	// Open a rawsock may be the easiest cross platform way to get the MAC address.
	void *txSock = openavbRawsockOpen(gAvdeccCfg.ifname, FALSE, TRUE, ETHERTYPE_AVTP, 100, 1);
	if (txSock) {
		openavbRawsockGetAddr(txSock, gAvdeccCfg.ifmac);
		openavbRawsockClose(txSock);
		txSock = NULL;
	}
}

bool openavbAvdeccAddConfiguration(openavb_tl_data_cfg_t *stream)
{
	bool first_time = 0;
	// Create a new config to hold the configuration information.
	openavb_avdecc_configuration_cfg_t *pCfg = malloc(sizeof(openavb_avdecc_configuration_cfg_t));
	if (!pCfg) {
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	memset(pCfg, 0, sizeof(openavb_avdecc_configuration_cfg_t));

	// Add a pointer to the supplied stream information.
	pCfg->stream = stream;
	pCfg->stream_is_crf = openavbAvdeccIsCrfStream(stream);
	pCfg->stream_descriptor_type = OPENAVB_AEM_DESCRIPTOR_INVALID;
	pCfg->stream_descriptor_index = OPENAVB_AEM_DESCRIPTOR_INVALID;

	// Add the new config to the end of the list of configurations.
	if (pFirstConfigurationCfg == NULL) {
		pFirstConfigurationCfg = pCfg;
	} else {
		openavb_avdecc_configuration_cfg_t *pLast = pFirstConfigurationCfg;
		while (pLast->next != NULL) {
			pLast = pLast->next;
		}
		pLast->next = pCfg;
	}

	// Create a new configuration.
	U16 nConfigIdx = 0;
	if (pConfiguration == NULL)
	{
		first_time = 1;
		pConfiguration = openavbAemDescriptorConfigurationNew();
		if (!openavbAemAddDescriptor(pConfiguration, OPENAVB_AEM_DESCRIPTOR_INVALID, &nConfigIdx)) {
			AVB_LOG_ERROR("Error adding AVDECC configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
	}
	// Specify a default user-friendly name to use.
	// AVDECC_TODO - Allow the user to specify a friendly name, or use the name if the .INI file.
	if (pCfg->friendly_name[0] == '\0') {
		snprintf((char *) pCfg->friendly_name, OPENAVB_AEM_STRLEN_MAX, "Configuration %u", nConfigIdx);
	}

	// Save the stream information in the configuration.
	if (!openavbAemDescriptorConfigurationInitialize(pConfiguration, nConfigIdx, pCfg)) {
		AVB_LOG_ERROR("Error initializing AVDECC configuration");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Add the descriptors needed for both talkers and listeners.
	U16 nResultIdx;
	if (first_time)
	{
		openavb_aem_descriptor_avb_interface_t *pNewAvbInterface = openavbAemDescriptorAvbInterfaceNew();
		if (!openavbAemAddDescriptor(pNewAvbInterface, nConfigIdx, &nResultIdx) ||
				!openavbAemDescriptorAvbInterfaceInitialize(pNewAvbInterface, nConfigIdx, pCfg)) {
			AVB_LOG_ERROR("Error adding AVDECC AVB Interface to configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		openavb_aem_descriptor_audio_unit_t *pNewAudioUnit = openavbAemDescriptorAudioUnitNew();
		if (!openavbAemAddDescriptor(pNewAudioUnit, nConfigIdx, &nResultIdx) ||
				!openavbAemDescriptorAudioUnitInitialize(pNewAudioUnit, nConfigIdx, pCfg)) {
			AVB_LOG_ERROR("Error adding AVDECC Audio Unit to configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		if (!openavbAvdeccAddGptpTimingChain(nConfigIdx)) {
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		if (!openavbAvdeccAddIdentifyControl(nConfigIdx, pCfg)) {
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		if (!openavbAvdeccAddEntityLogoMemoryObject(nConfigIdx)) {
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
	}
	else
	{
		openavb_aem_descriptor_audio_unit_t *pAudioUnitDescriptor =
		(openavb_aem_descriptor_audio_unit_t *) openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_AUDIO_UNIT, 0);
		if (pAudioUnitDescriptor != NULL)
		{
			if (!openavbAemDescriptorAudioUnitInitialize(pAudioUnitDescriptor, nConfigIdx, pCfg)) {
				AVB_LOG_ERROR("Error updating AVDECC Audio Unit to configuration");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return FALSE;
			}
		}
		else
		{
			AVB_LOG_ERROR("Error getting AVDECC Audio Unit descriptor");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
	}

	// AVDECC_TODO:  Add other descriptors as needed.  Future options include:
	//  VIDEO_UNIT
	//  SENSOR_UNIT
	//  CONTROL

	if (stream->role == AVB_ROLE_TALKER) {
		gAvdeccCfg.bTalker = TRUE;

		openavb_aem_descriptor_stream_io_t *pNewStreamOutput = openavbAemDescriptorStreamOutputNew();
		if (!openavbAemAddDescriptor(pNewStreamOutput, nConfigIdx, &nResultIdx) ||
				!openavbAemDescriptorStreamOutputInitialize(pNewStreamOutput, nConfigIdx, pCfg)) {
			AVB_LOG_ERROR("Error adding AVDECC Stream Output to configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		pCfg->stream_descriptor_type = OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT;
		pCfg->stream_descriptor_index = nResultIdx;

			if (openavbAvdeccUsesSharedAudioClockDomain(pCfg) ||
					(pCfg->stream_is_crf && audio_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID)) {
				U16 crfClockSourceIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
				if (!openavbAvdeccEnsureSharedAudioClockDomain(nConfigIdx, pCfg)) {
					AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
					return FALSE;
				}
				pNewStreamOutput->clock_domain_index = audio_clock_domain_idx;
				if (pCfg->stream_is_crf) {
					if (!openavbAvdeccAddClockSource(nConfigIdx, pCfg, &crfClockSourceIdx)) {
						AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
						return FALSE;
					}
					openavbAvdeccPreferClockDomainSource(
						nConfigIdx,
						audio_clock_domain_idx,
						crfClockSourceIdx,
						TRUE);
				}
			}
			else {
			if (first_talker)
			{
				first_talker = 0;
				openavb_aem_descriptor_clock_domain_t *pNewClockDomain = openavbAemDescriptorClockDomainNew();
				if (!openavbAemAddDescriptor(pNewClockDomain, nConfigIdx, &nResultIdx) ||
						!openavbAemDescriptorClockDomainInitialize(pNewClockDomain, nConfigIdx, pCfg)) {
					AVB_LOG_ERROR("Error adding AVDECC Clock Domain to configuration");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return FALSE;
				}
				talker_clock_domain_idx = nResultIdx;
				}
				if (pCfg->stream_is_crf) {
				U16 crfClockSourceIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
				if (!openavbAvdeccAddClockSource(nConfigIdx, pCfg, &crfClockSourceIdx)) {
					AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
					return FALSE;
				}
				if (talker_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID &&
						crfClockSourceIdx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
					openavb_aem_descriptor_clock_domain_t *pClockDomain =
						openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN, talker_clock_domain_idx);
					if (pClockDomain) {
						pClockDomain->clock_source_index = crfClockSourceIdx;
						AVB_LOGF_INFO("Clock domain %u default source set to CRF talker source %u",
							talker_clock_domain_idx, crfClockSourceIdx);
					}
					}
				}
			if (talker_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
				pNewStreamOutput->clock_domain_index = talker_clock_domain_idx;
			}
			}

		openavbAvdeccLogStreamDescriptorSummary(nConfigIdx, pCfg);

			// AVDECC_TODO:  Add other descriptors as needed.  Future options include:
			//  JACK_INPUT
		talker_stream_sources++;

		// Add the class specific to the talker.
		if (stream->sr_class == SR_CLASS_A) { gAvdeccCfg.bClassASupported = TRUE; }
		if (stream->sr_class == SR_CLASS_B) { gAvdeccCfg.bClassBSupported = TRUE; }

		AVB_LOG_DEBUG("AVDECC talker configuration added");
	}
	if (stream->role == AVB_ROLE_LISTENER) {
		gAvdeccCfg.bListener = TRUE;

		openavb_aem_descriptor_stream_io_t *pNewStreamInput = openavbAemDescriptorStreamInputNew();
		if (!openavbAemAddDescriptor(pNewStreamInput, nConfigIdx, &nResultIdx) ||
				!openavbAemDescriptorStreamInputInitialize(pNewStreamInput, nConfigIdx, pCfg)) {
			AVB_LOG_ERROR("Error adding AVDECC Stream Input to configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		pCfg->stream_descriptor_type = OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT;
		pCfg->stream_descriptor_index = nResultIdx;

			if (openavbAvdeccUsesSharedAudioClockDomain(pCfg) ||
					(pCfg->stream_is_crf && audio_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID)) {
				U16 crfClockSourceIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
				if (!openavbAvdeccEnsureSharedAudioClockDomain(nConfigIdx, pCfg)) {
					AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
					return FALSE;
				}
				pNewStreamInput->clock_domain_index = audio_clock_domain_idx;
				if (pCfg->stream_is_crf) {
					if (!openavbAvdeccAddClockSource(nConfigIdx, pCfg, &crfClockSourceIdx)) {
						AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
						return FALSE;
					}
					openavbAvdeccPreferClockDomainSource(
						nConfigIdx,
						audio_clock_domain_idx,
						crfClockSourceIdx,
						FALSE);
				}
			}
			else {
			if (first_listener)
			{
				first_listener = 0;
				openavb_aem_descriptor_clock_domain_t *pNewClockDomain = openavbAemDescriptorClockDomainNew();
				if (!openavbAemAddDescriptor(pNewClockDomain, nConfigIdx, &nResultIdx) ||
						!openavbAemDescriptorClockDomainInitialize(pNewClockDomain, nConfigIdx, pCfg)) {
					AVB_LOG_ERROR("Error adding AVDECC Clock Domain to configuration");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return FALSE;
				}
				listener_clock_domain_idx = nResultIdx;
				}
				if (pCfg->stream_is_crf) {
				U16 crfClockSourceIdx = OPENAVB_AEM_DESCRIPTOR_INVALID;
				if (!openavbAvdeccAddClockSource(nConfigIdx, pCfg, &crfClockSourceIdx)) {
					AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
					return FALSE;
				}
				if (listener_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID &&
						crfClockSourceIdx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
					openavb_aem_descriptor_clock_domain_t *pClockDomain =
						openavbAemGetDescriptor(nConfigIdx, OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN, listener_clock_domain_idx);
					if (pClockDomain) {
						pClockDomain->clock_source_index = crfClockSourceIdx;
						AVB_LOGF_INFO("Clock domain %u default source set to CRF listener source %u",
							listener_clock_domain_idx, crfClockSourceIdx);
					}
					}
				}
			if (listener_clock_domain_idx != OPENAVB_AEM_DESCRIPTOR_INVALID) {
				pNewStreamInput->clock_domain_index = listener_clock_domain_idx;
			}
			}

		openavbAvdeccLogStreamDescriptorSummary(nConfigIdx, pCfg);

			// AVDECC_TODO:  Add other descriptors as needed.  Future options include:
			//  JACK_OUTPUT
		listener_stream_sources++;

		// Listeners support both Class A and Class B.
		gAvdeccCfg.bClassASupported = TRUE;
		gAvdeccCfg.bClassBSupported = TRUE;

		AVB_LOG_DEBUG("AVDECC listener configuration added");
	}

	if (first_time)
	{
		// Add the localized strings to the configuration.
		if (!openavbAemDescriptorLocaleStringsHandlerAddToConfiguration(gAvdeccCfg.pAemDescriptorLocaleStringsHandler, nConfigIdx)) {
			AVB_LOG_ERROR("Error adding AVDECC locale strings to configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
	}

	openavbAvdeccLogConfigurationDescriptorCounts(nConfigIdx);

	return TRUE;
}


////////////////////////////////
// Public functions
////////////////////////////////
extern DLL_EXPORT bool openavbAvdeccInitialize()
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// If initialize is called again in-process, force a fresh descriptor/model
	// build to avoid stale configuration pointers.
	openavbAvdeccFreeConfigurationCfgList();
	pConfiguration = NULL;

	// Ensure model-scoped state starts cleanly for each initialize.
	talker_stream_sources = 0;
	listener_stream_sources = 0;
	first_talker = 1;
	first_listener = 1;
	talker_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	listener_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	audio_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	s_identifyControlIndex = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavbAvdeccLoadEntityLogo();

	gAvdeccCfg.pDescriptorEntity = openavbAemDescriptorEntityNew();
	if (!gAvdeccCfg.pDescriptorEntity) {
		AVB_LOG_ERROR("Failed to allocate an AVDECC descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	openavbAvdeccFindMacAddr();

	if (!openavbAemDescriptorEntitySet_entity_id(gAvdeccCfg.pDescriptorEntity, NULL, gAvdeccCfg.ifmac, gAvdeccCfg.avdeccId)) {
		AVB_LOG_ERROR("Failed to set the AVDECC descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Create the Entity Model
	openavbRC rc = openavbAemCreate(gAvdeccCfg.pDescriptorEntity);
	if (IS_OPENAVB_FAILURE(rc)) {
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Copy the supplied non-localized strings to the descriptor.
	openavbAemDescriptorEntitySet_entity_model_id(gAvdeccCfg.pDescriptorEntity, gAvdeccCfg.entity_model_id);
	openavbAemDescriptorEntitySet_entity_name(gAvdeccCfg.pDescriptorEntity, gAvdeccCfg.entity_name);
	openavbAemDescriptorEntitySet_firmware_version(gAvdeccCfg.pDescriptorEntity, gAvdeccCfg.firmware_version);
	openavbAemDescriptorEntitySet_group_name(gAvdeccCfg.pDescriptorEntity, gAvdeccCfg.group_name);
	openavbAemDescriptorEntitySet_serial_number(gAvdeccCfg.pDescriptorEntity, gAvdeccCfg.serial_number);

	// Initialize the localized strings support.
	gAvdeccCfg.pAemDescriptorLocaleStringsHandler = openavbAemDescriptorLocaleStringsHandlerNew();
	if (gAvdeccCfg.pAemDescriptorLocaleStringsHandler) {
		// Add the strings to the locale strings hander.
		openavbAemDescriptorLocaleStringsHandlerSet_local_string(
			gAvdeccCfg.pAemDescriptorLocaleStringsHandler, gAvdeccCfg.locale_identifier, gAvdeccCfg.vendor_name, LOCALE_STRING_VENDOR_NAME_INDEX);
		openavbAemDescriptorLocaleStringsHandlerSet_local_string(
			gAvdeccCfg.pAemDescriptorLocaleStringsHandler, gAvdeccCfg.locale_identifier, gAvdeccCfg.model_name, LOCALE_STRING_MODEL_NAME_INDEX);

		// Have the descriptor entity reference the locale strings.
		openavbAemDescriptorEntitySet_vendor_name(gAvdeccCfg.pDescriptorEntity, 0, LOCALE_STRING_VENDOR_NAME_INDEX);
		openavbAemDescriptorEntitySet_model_name(gAvdeccCfg.pDescriptorEntity, 0, LOCALE_STRING_MODEL_NAME_INDEX);
	}

	gAvdeccCfg.bTalker = gAvdeccCfg.bListener = FALSE;

	// Add a configuration for each talker or listener stream.
	openavb_tl_data_cfg_t *current_stream = streamList;
	while (current_stream != NULL) {
		// Create a new configuration with the information from this stream.
		if (!openavbAvdeccAddConfiguration(current_stream)) {
			AVB_LOG_ERROR("Error adding AVDECC configuration");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}

		// Proceed to the next stream.
		current_stream = current_stream->next;
	}

	if (!gAvdeccCfg.bTalker && !gAvdeccCfg.bListener) {
		AVB_LOG_ERROR("No AVDECC Configurations -- Aborting");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Add non-top-level descriptors.  These are independent of the configurations.
	// STRINGS are handled by gAvdeccCfg.pAemDescriptorLocaleStringsHandler, so not included here.
	if (gAvdeccCfg.bTalker) {
		U16 nResultIdx;
		if (!openavbAemAddDescriptor(openavbAemDescriptorStreamPortOutputNew(), OPENAVB_AEM_DESCRIPTOR_INVALID, &nResultIdx)) {
			AVB_LOG_ERROR("Error adding AVDECC Output Stream Port");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
	}

	if (gAvdeccCfg.bListener) {
		U16 nResultIdx;
		if (!openavbAemAddDescriptor(openavbAemDescriptorStreamPortInputNew(), OPENAVB_AEM_DESCRIPTOR_INVALID, &nResultIdx)) {
			AVB_LOG_ERROR("Error adding AVDECC Input Stream Port");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
	}
	if (!openavbAvdeccConfigureAudioStreamPort(0, AVB_ROLE_LISTENER) ||
			!openavbAvdeccConfigureAudioStreamPort(0, AVB_ROLE_TALKER)) {
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	openavbAvdeccFinalizeAudioUnit(0);

	// AVDECC_TODO:  Add other descriptors as needed.  Future options include:
	//  EXTERNAL_PORT_INPUT
	//  EXTERNAL_PORT_OUTPUT
	//  INTERNAL_PORT_INPUT
	//  INTERNAL_PORT_OUTPUT
	//  VIDEO_CLUSTER
	//  SENSOR_CLUSTER
	//  VIDEO_MAP
	//  SENSOR_MAP

	// Fill in the descriptor capabilities.
	// AVDECC_TODO:  Set these based on the available capabilities.
	if (!gAvdeccCfg.bClassASupported && !gAvdeccCfg.bClassBSupported) {
		// If the user didn't specify a traffic class, assume both are supported.
		gAvdeccCfg.bClassASupported = gAvdeccCfg.bClassBSupported = TRUE;
	}
	openavbAemDescriptorEntitySet_entity_capabilities(gAvdeccCfg.pDescriptorEntity,
		OPENAVB_ADP_ENTITY_CAPABILITIES_AEM_SUPPORTED |
		OPENAVB_ADP_ENTITY_CAPABILITIES_VENDOR_UNIQUE_SUPPORTED |
		OPENAVB_ADP_ENTITY_CAPABILITIES_AEM_INTERFACE_INDEX_VALID |
		(openavbAvdeccHasEntityLogo() ? OPENAVB_ADP_ENTITY_CAPABILITIES_ADDRESS_ACCESS_SUPPORTED : 0) |
		(gAvdeccCfg.bClassASupported ? OPENAVB_ADP_ENTITY_CAPABILITIES_CLASS_A_SUPPORTED : 0) |
		(gAvdeccCfg.bClassBSupported ? OPENAVB_ADP_ENTITY_CAPABILITIES_CLASS_B_SUPPORTED : 0) |
		(s_identifyControlIndex != OPENAVB_AEM_DESCRIPTOR_INVALID ? OPENAVB_ADP_ENTITY_CAPABILITIES_AEM_IDENTIFY_CONTROL_INDEX_VALID : 0) |
		OPENAVB_ADP_ENTITY_CAPABILITIES_GPTP_SUPPORTED);

	if (gAvdeccCfg.bTalker) {
		// AVDECC_TODO:  Set these based on the available capabilities.
		openavbAemDescriptorEntitySet_talker_capabilities(gAvdeccCfg.pDescriptorEntity, talker_stream_sources,
			OPENAVB_ADP_TALKER_CAPABILITIES_IMPLEMENTED |
			OPENAVB_ADP_TALKER_CAPABILITIES_AUDIO_SOURCE |
			OPENAVB_ADP_TALKER_CAPABILITIES_MEDIA_CLOCK_SOURCE);
	}
	if (gAvdeccCfg.bListener) {
		// AVDECC_TODO:  Set these based on the available capabilities.
		openavbAemDescriptorEntitySet_listener_capabilities(gAvdeccCfg.pDescriptorEntity, listener_stream_sources,
			OPENAVB_ADP_LISTENER_CAPABILITIES_IMPLEMENTED |
			OPENAVB_ADP_LISTENER_CAPABILITIES_AUDIO_SINK);
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

openavb_tl_data_cfg_t *openavbAvdeccGetStreamCfg(U16 streamIndex)
{
	openavb_avdecc_configuration_cfg_t *pCfg = pFirstConfigurationCfg;
	while (pCfg) {
		if (streamIndex == 0) {
			return pCfg->stream;
		}
		streamIndex--;
		pCfg = pCfg->next;
	}
	return NULL;
}

U16 openavbAvdeccGetIdentifyControlIndex(void)
{
	return s_identifyControlIndex;
}

bool openavbAvdeccHasEntityLogo(void)
{
	return (s_entityLogoData != NULL && s_entityLogoLength != 0);
}

U64 openavbAvdeccGetEntityLogoStartAddress(void)
{
	return OPENAVB_AVDECC_ENTITY_LOGO_START_ADDRESS;
}

U64 openavbAvdeccGetEntityLogoLength(void)
{
	return s_entityLogoLength;
}

bool openavbAvdeccReadEntityLogo(U64 address, U16 requestedLength, U8 *pBuf, U16 *pReadLength, U16 *pStatus)
{
	U64 offset;

	if (pReadLength) {
		*pReadLength = 0;
	}

	if (!pStatus) {
		return FALSE;
	}

	if (!openavbAvdeccHasEntityLogo()) {
		*pStatus = OPENAVB_AECP_AA_STATUS_UNSUPPORTED;
		return FALSE;
	}

	if (address < OPENAVB_AVDECC_ENTITY_LOGO_START_ADDRESS) {
		*pStatus = OPENAVB_AECP_AA_STATUS_ADDRESS_TOO_LOW;
		return FALSE;
	}

	offset = address - OPENAVB_AVDECC_ENTITY_LOGO_START_ADDRESS;
	if (offset > s_entityLogoLength) {
		*pStatus = OPENAVB_AECP_AA_STATUS_ADDRESS_TOO_HIGH;
		return FALSE;
	}
	if ((U64)requestedLength > (s_entityLogoLength - offset)) {
		*pStatus = OPENAVB_AECP_AA_STATUS_ADDRESS_TOO_HIGH;
		return FALSE;
	}
	if (requestedLength != 0 && !pBuf) {
		*pStatus = OPENAVB_AECP_AA_STATUS_DATA_INVALID;
		return FALSE;
	}

	if (requestedLength != 0) {
		memcpy(pBuf, s_entityLogoData + offset, requestedLength);
	}
	if (pReadLength) {
		*pReadLength = requestedLength;
	}
	*pStatus = OPENAVB_AECP_AA_STATUS_SUCCESS;
	return TRUE;
}

// Start the AVDECC protocols.
extern DLL_EXPORT bool openavbAvdeccStart()
{
	if (!openavbAvdeccStartCmp()) {
		AVB_LOG_ERROR("openavbAvdeccStartCmp() failure!");
		return FALSE;
	}
	if (!openavbAvdeccStartEcp()) {
		AVB_LOG_ERROR("openavbAvdeccStartEcp() failure!");
		return FALSE;
	}
	if (!openavbAvdeccStartAdp()) {
		AVB_LOG_ERROR("openavbAvdeccStartAdp() failure!");
		return FALSE;
	}

	return TRUE;
}

// Stop the AVDECC protocols.
extern DLL_EXPORT void openavbAvdeccStop(void)
{
	openavbAvdeccStopCmp();
	openavbAvdeccStopEcp();
	openavbAvdeccStopAdp();
}

extern DLL_EXPORT bool openavbAvdeccCleanup(void)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	openavbRC rc = openavbAemDestroy();

	openavbAvdeccFreeConfigurationCfgList();
	pConfiguration = NULL;
	talker_stream_sources = 0;
	listener_stream_sources = 0;
	first_talker = 1;
	first_listener = 1;
	talker_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	listener_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	audio_clock_domain_idx = OPENAVB_AEM_DESCRIPTOR_INVALID;
	s_identifyControlIndex = OPENAVB_AEM_DESCRIPTOR_INVALID;
	openavbAvdeccFreeEntityLogo();

	if (IS_OPENAVB_FAILURE(rc)) {
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}
