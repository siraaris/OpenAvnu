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

#define	AVB_LOG_COMPONENT	"AVDECC"
#include "openavb_log.h"
#include "openavb_trace_pub.h"

#include "openavb_aem_types_pub.h"
#include "openavb_avdecc_pub.h"
#include "openavb_avdecc_pipeline_interaction_pub.h"
#include "openavb_avdecc_msg_server.h"
#include "openavb_descriptor_avb_interface_pub.h"
#include "openavb_grandmaster_osal_pub.h"

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

extern openavb_avdecc_cfg_t gAvdeccCfg;

#define OPENAVB_AVDECC_MAX_COUNTER_AVB_INTERFACES (8)

static bool s_clock_domain_counters_init = false;
static bool s_clock_domain_locked = false;
static U32 s_clock_domain_locked_count = 0;
static U32 s_clock_domain_unlocked_count = 0;

typedef struct {
	bool initialized;
	bool link_up;
	U32 link_up_count;
	U32 link_down_count;
	bool grandmaster_initialized;
	U8 grandmaster_id[8];
	U8 grandmaster_domain_number;
	U32 grandmaster_changed_count;
} openavb_avdecc_avb_interface_counter_state_t;

static openavb_avdecc_avb_interface_counter_state_t s_avb_interface_counter_states[OPENAVB_AVDECC_MAX_COUNTER_AVB_INTERFACES];

static openavb_avdecc_avb_interface_counter_state_t *openavbAvdeccGetAvbInterfaceCounterState(
	const openavb_aem_descriptor_avb_interface_t *pDescriptor)
{
	if (!pDescriptor) {
		return NULL;
	}
	if (pDescriptor->descriptor_index >= OPENAVB_AVDECC_MAX_COUNTER_AVB_INTERFACES) {
		return NULL;
	}

	return &s_avb_interface_counter_states[pDescriptor->descriptor_index];
}

static bool openavbAvdeccGetInterfaceLinkState(const char *ifname, bool *pLinkUp)
{
	int sk = -1;
	struct ifreq ifr;
	bool linkUp = false;
	size_t ifname_len = 0;

	if (!ifname || !ifname[0] || !pLinkUp) {
		return false;
	}

	sk = socket(AF_INET, SOCK_DGRAM, 0);
	if (sk == -1) {
		return false;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifname_len = strnlen(ifname, sizeof(ifr.ifr_name) - 1);
	memcpy(ifr.ifr_name, ifname, ifname_len);
	if (ioctl(sk, SIOCGIFFLAGS, &ifr) == 0) {
		linkUp = ((ifr.ifr_flags & IFF_UP) != 0) &&
			((ifr.ifr_flags & IFF_RUNNING) != 0);
	}
	else {
		close(sk);
		return false;
	}

	close(sk);
	*pLinkUp = linkUp;
	return true;
}

static void openavbAvdeccUpdateAvbInterfaceGrandmasterCounter(
	openavb_avdecc_avb_interface_counter_state_t *pState)
{
	U8 grandmaster_id[8] = { 0 };
	U8 grandmaster_domain_number = 0;

	if (!pState) {
		return;
	}

	if (!osalAVBGrandmasterGetCurrent(grandmaster_id, &grandmaster_domain_number)) {
		return;
	}

	if (!pState->grandmaster_initialized) {
		memcpy(pState->grandmaster_id, grandmaster_id, sizeof(pState->grandmaster_id));
		pState->grandmaster_domain_number = grandmaster_domain_number;
		pState->grandmaster_initialized = true;
		return;
	}

	if (memcmp(pState->grandmaster_id, grandmaster_id, sizeof(pState->grandmaster_id)) != 0 ||
			pState->grandmaster_domain_number != grandmaster_domain_number) {
		memcpy(pState->grandmaster_id, grandmaster_id, sizeof(pState->grandmaster_id));
		pState->grandmaster_domain_number = grandmaster_domain_number;
		pState->grandmaster_changed_count++;
	}
}

static bool openavbAvdeccUpdateAvbInterfaceCounters(
	const openavb_aem_descriptor_avb_interface_t *pDescriptor)
{
	openavb_avdecc_avb_interface_counter_state_t *pState;
	bool linkUp = false;

	pState = openavbAvdeccGetAvbInterfaceCounterState(pDescriptor);
	if (!pState) {
		return false;
	}

	if (!openavbAvdeccGetInterfaceLinkState((const char *)gAvdeccCfg.ifname, &linkUp)) {
		openavbAvdeccUpdateAvbInterfaceGrandmasterCounter(pState);
		return pState->initialized;
	}

	if (!pState->initialized) {
		pState->initialized = true;
		pState->link_up = linkUp;
		if (linkUp) {
			pState->link_up_count = 1;
		}
		openavbAvdeccUpdateAvbInterfaceGrandmasterCounter(pState);
		return true;
	}

	if (linkUp != pState->link_up) {
		pState->link_up = linkUp;
		if (linkUp) {
			pState->link_up_count++;
		}
		else {
			pState->link_down_count++;
		}
	}

	openavbAvdeccUpdateAvbInterfaceGrandmasterCounter(pState);
	return true;
}

static bool openavbClockDomainGetLockedState(bool *locked)
{
	uint8_t gm_id[8] = { 0 };
	uint8_t domain = 0;

	if (!locked) {
		return false;
	}

	if (!osalAVBGrandmasterGetCurrent(gm_id, &domain)) {
		return false;
	}

	*locked = (memcmp(gm_id, "\0\0\0\0\0\0\0\0", sizeof(gm_id)) != 0);
	return true;
}

static bool openavbClockDomainUpdateCounters(void)
{
	bool locked = false;

	if (!openavbClockDomainGetLockedState(&locked)) {
		return false;
	}

	if (!s_clock_domain_counters_init) {
		s_clock_domain_counters_init = true;
		s_clock_domain_locked = locked;
		if (locked) {
			s_clock_domain_locked_count = 1;
		} else {
			s_clock_domain_unlocked_count = 1;
		}
		return true;
	}

	if (locked != s_clock_domain_locked) {
		s_clock_domain_locked = locked;
		if (locked) {
			s_clock_domain_locked_count++;
		} else {
			s_clock_domain_unlocked_count++;
		}
	}

	return true;
}

static bool openavbAvdeccMsgHandleAlreadyNotified(int avdeccMsgHandle, int notifiedHandles[], U16 notifiedCount)
{
	U16 i;
	for (i = 0; i < notifiedCount; ++i) {
		if (notifiedHandles[i] == avdeccMsgHandle) {
			return true;
		}
	}
	return false;
}

static void openavbAVDECCNotifyClockDomainStreamsByType(
	U16 configIdx,
	U16 descriptorType,
	const openavb_aem_descriptor_clock_domain_t *pDescriptorClockDomain,
	const openavb_aem_descriptor_clock_source_t *pDescriptorClockSource,
	int notifiedHandles[],
	U16 *pNotifiedCount,
	U16 *pSentCount,
	U16 *pFailCount)
{
	U16 descriptorIdx = 0;
	while (1) {
		openavb_aem_descriptor_stream_io_t *pDescriptorStream =
			openavbAemGetDescriptor(configIdx, descriptorType, descriptorIdx);
		if (!pDescriptorStream) {
			break;
		}
		descriptorIdx++;

		if (pDescriptorStream->clock_domain_index != pDescriptorClockDomain->descriptor_index) {
			continue;
		}
		if (!pDescriptorStream->stream || !pDescriptorStream->stream->client) {
			continue;
		}

		int avdeccMsgHandle = pDescriptorStream->stream->client->avdeccMsgHandle;
		if (avdeccMsgHandle == AVB_AVDECC_MSG_HANDLE_INVALID) {
			continue;
		}
		if (openavbAvdeccMsgHandleAlreadyNotified(avdeccMsgHandle, notifiedHandles, *pNotifiedCount)) {
			continue;
		}

		if (*pNotifiedCount < MAX_AVDECC_MSG_CLIENTS) {
			notifiedHandles[*pNotifiedCount] = avdeccMsgHandle;
			(*pNotifiedCount)++;
		}

		if (openavbAvdeccMsgSrvrClockSourceUpdate(
				avdeccMsgHandle,
				pDescriptorClockDomain->descriptor_index,
				pDescriptorClockSource->descriptor_index,
				pDescriptorClockSource->clock_source_flags,
				pDescriptorClockSource->clock_source_type,
				pDescriptorClockSource->clock_source_location_type,
				pDescriptorClockSource->clock_source_location_index)) {
			(*pSentCount)++;
		}
		else {
			(*pFailCount)++;
			AVB_LOGF_WARNING("SET_CLOCK_SOURCE notify failed: handle=%d domain=%u source=%u",
				avdeccMsgHandle,
				pDescriptorClockDomain->descriptor_index,
				pDescriptorClockSource->descriptor_index);
		}
	}
}


bool openavbAVDECCRunListener(openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput, U16 configIdx, openavb_acmp_ListenerStreamInfo_t *pListenerStreamInfo)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);
	U8 srClass = SR_CLASS_A;

	// Sanity tests.
	if (!pDescriptorStreamInput) {
		AVB_LOG_ERROR("openavbAVDECCRunListener Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pListenerStreamInfo) {
		AVB_LOG_ERROR("openavbAVDECCRunListener Invalid streaminfo");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamInput->stream) {
		AVB_LOG_ERROR("openavbAVDECCRunListener Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamInput->stream->client) {
		AVB_LOG_ERROR("openavbAVDECCRunListener Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Some controllers/talkers leave the class flag unset in the listener-facing ACMP path.
	// Preserve the listener's configured class unless the talker explicitly declares Class B.
	srClass = pDescriptorStreamInput->stream->sr_class;
	if (srClass != SR_CLASS_A && srClass != SR_CLASS_B) {
		srClass = SR_CLASS_A;
	}
	if ((pListenerStreamInfo->flags & OPENAVB_ACMP_FLAG_CLASS_B) != 0) {
		srClass = SR_CLASS_B;
	}

	AVB_LOGF_INFO("Run listener request: handle=%d class=%c stream=" ETH_FORMAT "/%u dest=" ETH_FORMAT " vlan=%u flags=0x%04x",
		pDescriptorStreamInput->stream->client->avdeccMsgHandle,
		AVB_CLASS_LABEL(srClass),
		ETH_OCTETS(pListenerStreamInfo->stream_id),
		(((U16)pListenerStreamInfo->stream_id[6]) << 8) | (U16)pListenerStreamInfo->stream_id[7],
		ETH_OCTETS(pListenerStreamInfo->stream_dest_mac),
		pListenerStreamInfo->stream_vlan_id,
		pListenerStreamInfo->flags);

	// Send the Stream ID to the client.
	// The client will stop a running Listener if the settings differ from its current values.
	if (!openavbAvdeccMsgSrvrListenerStreamID(pDescriptorStreamInput->stream->client->avdeccMsgHandle,
			srClass,
			pListenerStreamInfo->stream_id, /* The first 6 bytes of the steam_id are the source MAC Address */
			(((U16) pListenerStreamInfo->stream_id[6]) << 8 | (U16) pListenerStreamInfo->stream_id[7]),
			pListenerStreamInfo->stream_dest_mac,
			pListenerStreamInfo->stream_vlan_id)) {
		AVB_LOG_ERROR("Error send Stream ID to Listener");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Tell the client to start running.
	if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptorStreamInput->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_RUNNING)) {
		AVB_LOG_ERROR("Error requesting Listener change to Running");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	AVB_LOG_INFO("Listener state change to Running requested");

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

bool openavbAVDECCRunTalker(openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput, U16 configIdx, openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStreamOutput) {
		AVB_LOG_ERROR("openavbAVDECCRunTalker Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pTalkerStreamInfo) {
		AVB_LOG_ERROR("openavbAVDECCRunTalker Invalid streaminfo");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream) {
		AVB_LOG_ERROR("openavbAVDECCRunTalker Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream->client) {
		AVB_LOG_ERROR("openavbAVDECCRunTalker Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	if (pDescriptorStreamOutput->stream->defer_start_until_selected_clock &&
			pDescriptorStreamOutput->stream->client->lastRequestedState == OPENAVB_AVDECC_MSG_UNKNOWN &&
			pDescriptorStreamOutput->stream->client->lastReportedState != OPENAVB_AVDECC_MSG_RUNNING) {
		AVB_LOGF_INFO("Deferring talker run request until selected clock is stable: handle=%d wait=%uus name=%s uid=%u",
			pDescriptorStreamOutput->stream->client->avdeccMsgHandle,
			pDescriptorStreamOutput->stream->defer_start_stable_usec,
			pDescriptorStreamOutput->stream->friendly_name,
			pDescriptorStreamOutput->stream->stream_uid);
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return TRUE;
	}

	// Tell the client to start running.
	// Note that that Talker may already be running; this call ensures that it really is.
	if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptorStreamOutput->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_RUNNING)) {
		AVB_LOG_ERROR("Error requesting Talker change to Running");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	AVB_LOG_INFO("Talker state change to Running requested");

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

bool openavbAVDECCStopListener(openavb_aem_descriptor_stream_io_t *pDescriptorStreamInput, U16 configIdx, openavb_acmp_ListenerStreamInfo_t *pListenerStreamInfo)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStreamInput) {
		AVB_LOG_ERROR("openavbAVDECCStopListener Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pListenerStreamInfo) {
		AVB_LOG_ERROR("openavbAVDECCStopListener Invalid streaminfo");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamInput->stream) {
		AVB_LOG_ERROR("openavbAVDECCStopListener Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamInput->stream->client) {
		AVB_LOG_ERROR("openavbAVDECCStopListener Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Don't request if already stopped.
	if (pDescriptorStreamInput->stream->client->lastReportedState == OPENAVB_AVDECC_MSG_STOPPED) {
		AVB_LOG_INFO("Listener state change to Stopped ignored, as Listener already Stopped");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return TRUE;
	}

	// Send the request to the client.
	if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptorStreamInput->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_STOPPED)) {
		AVB_LOG_ERROR("Error requesting Listener change to Stopped");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	AVB_LOG_INFO("Listener state change to Stopped requested");

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

bool openavbAVDECCStopTalker(openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput, U16 configIdx, openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStreamOutput) {
		AVB_LOG_ERROR("openavbAVDECCStopTalker Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pTalkerStreamInfo) {
		AVB_LOG_ERROR("openavbAVDECCStopTalker Invalid streaminfo");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream) {
		AVB_LOG_ERROR("openavbAVDECCStopTalker Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream->client) {
		AVB_LOG_ERROR("openavbAVDECCStopTalker Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Don't request if already stopped.
	if (pDescriptorStreamOutput->stream->client->lastReportedState == OPENAVB_AVDECC_MSG_STOPPED) {
		AVB_LOG_INFO("Talker state change to Stopped ignored, as Talker already Stopped");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return TRUE;
	}

	// Send the request to the client.
	if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptorStreamOutput->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_STOPPED)) {
		AVB_LOG_ERROR("Error requesting Talker change to Stopped");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	AVB_LOG_INFO("Talker state change to Stopped requested");

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

bool openavbAVDECCGetTalkerStreamInfo(openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput, U16 configIdx, openavb_acmp_TalkerStreamInfo_t *pTalkerStreamInfo)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStreamOutput) {
		AVB_LOG_ERROR("openavbAVDECCGetTalkerStreamInfo Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pTalkerStreamInfo) {
		AVB_LOG_ERROR("openavbAVDECCGetTalkerStreamInfo Invalid streaminfo");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream) {
		AVB_LOG_ERROR("openavbAVDECCGetTalkerStreamInfo Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Get the destination MAC Address. A zero MAC is a valid "pending MAAP"
	// state for talkers whose multicast destination has not been allocated yet.
	const U8 *destMac = pDescriptorStreamOutput->stream->dest_addr.buffer.ether_addr_octet;
	memcpy(pTalkerStreamInfo->stream_dest_mac, destMac, ETH_ALEN);
	AVB_LOGF_DEBUG("Talker stream_dest_mac:  " ETH_FORMAT,
		ETH_OCTETS(pTalkerStreamInfo->stream_dest_mac));

	// Get the Stream ID.
	const U8 *streamSrcMac = pDescriptorStreamOutput->stream->stream_addr.buffer.ether_addr_octet;
	if (!pDescriptorStreamOutput->stream->stream_addr.mac ||
			memcmp(streamSrcMac, "\x00\x00\x00\x00\x00\x00", ETH_ALEN) == 0)
	{
		if (memcmp(gAvdeccCfg.ifmac, "\x00\x00\x00\x00\x00\x00", ETH_ALEN) == 0) {
			AVB_LOG_ERROR("openavbAVDECCGetTalkerStreamInfo Invalid stream stream_addr");
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}
		streamSrcMac = gAvdeccCfg.ifmac;
		AVB_LOGF_WARNING("Talker stream stream_addr unset; defaulting to " ETH_FORMAT,
			ETH_OCTETS(streamSrcMac));
	}
	memcpy(pTalkerStreamInfo->stream_id, streamSrcMac, ETH_ALEN);
	U8 *pStreamUID = pTalkerStreamInfo->stream_id + 6;
	*(U16 *)(pStreamUID) = htons(pDescriptorStreamOutput->stream->stream_uid);
	AVB_LOGF_DEBUG("Talker stream_id:  " ETH_FORMAT "/%u",
		ETH_OCTETS(pTalkerStreamInfo->stream_id),
		(((U16) pTalkerStreamInfo->stream_id[6]) << 8) | (U16) pTalkerStreamInfo->stream_id[7]);

	// Get the VLAN ID.
	pTalkerStreamInfo->stream_vlan_id = pDescriptorStreamOutput->stream->vlan_id;

	AVB_LOGF_INFO("Talker stream info: uid=%u stream_id=" ETH_FORMAT "/%u dest=" ETH_FORMAT " vlan=%u",
		pDescriptorStreamOutput->stream->stream_uid,
		ETH_OCTETS(pTalkerStreamInfo->stream_id),
		(((U16) pTalkerStreamInfo->stream_id[6]) << 8) | (U16) pTalkerStreamInfo->stream_id[7],
		ETH_OCTETS(pTalkerStreamInfo->stream_dest_mac),
		pTalkerStreamInfo->stream_vlan_id);

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

bool openavbAVDECCSetTalkerStreamInfo(openavb_aem_descriptor_stream_io_t *pDescriptorStreamOutput,
	U8 sr_class, U8 stream_id_valid, const U8 stream_src_mac[6], U16 stream_uid, U8 stream_dest_valid, const U8 stream_dest_mac[6], U8 stream_vlan_id_valid, U16 stream_vlan_id)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStreamOutput) {
		AVB_LOG_ERROR("openavbAVDECCSetTalkerStreamInfo Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream) {
		AVB_LOG_ERROR("openavbAVDECCSetTalkerStreamInfo Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}
	if (!pDescriptorStreamOutput->stream->client) {
		AVB_LOG_ERROR("openavbAVDECCSetTalkerStreamInfo Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// Send the information to the client.
	if (!openavbAvdeccMsgSrvrTalkerStreamID(pDescriptorStreamOutput->stream->client->avdeccMsgHandle,
			sr_class, stream_id_valid, stream_src_mac, stream_uid, stream_dest_valid, stream_dest_mac, stream_vlan_id_valid, stream_vlan_id)) {
		AVB_LOG_ERROR("Error sending stream info updates to Talker");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

bool openavbAVDECCSetClockSource(
	openavb_aem_descriptor_clock_domain_t *pDescriptorClockDomain,
	openavb_aem_descriptor_clock_source_t *pDescriptorClockSource,
	U16 configIdx)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	if (!pDescriptorClockDomain || !pDescriptorClockSource) {
		AVB_LOG_ERROR("openavbAVDECCSetClockSource invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	// For stream-referenced clock sources, validate the stream descriptor exists.
	if ((pDescriptorClockSource->clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID) != 0) {
		if (pDescriptorClockSource->clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT &&
				pDescriptorClockSource->clock_source_location_type != OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {
			AVB_LOGF_ERROR("SET_CLOCK_SOURCE unsupported location type=0x%04x",
				pDescriptorClockSource->clock_source_location_type);
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}

		openavb_aem_descriptor_stream_io_t *pStreamDescriptor =
			openavbAemGetDescriptor(configIdx,
				pDescriptorClockSource->clock_source_location_type,
				pDescriptorClockSource->clock_source_location_index);
		if (!pStreamDescriptor || !pStreamDescriptor->stream) {
			AVB_LOGF_ERROR("SET_CLOCK_SOURCE missing stream descriptor type=0x%04x idx=%u",
				pDescriptorClockSource->clock_source_location_type,
				pDescriptorClockSource->clock_source_location_index);
			AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
			return FALSE;
		}

		AVB_LOGF_INFO("SET_CLOCK_SOURCE applied: domain=%u source=%u stream=%s idx=%u map=%s uid=%u",
			pDescriptorClockDomain->descriptor_index,
			pDescriptorClockSource->descriptor_index,
			(pDescriptorClockSource->clock_source_location_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) ? "input" : "output",
			pDescriptorClockSource->clock_source_location_index,
			(pStreamDescriptor->stream->map_fn ? pStreamDescriptor->stream->map_fn : "<none>"),
			pStreamDescriptor->stream->stream_uid);
	}
	else {
		AVB_LOGF_INFO("SET_CLOCK_SOURCE applied: domain=%u source=%u type=0x%04x flags=0x%04x",
			pDescriptorClockDomain->descriptor_index,
			pDescriptorClockSource->descriptor_index,
			pDescriptorClockSource->clock_source_type,
			pDescriptorClockSource->clock_source_flags);
	}

	// Notify all stream clients in this clock domain so runtime can update.
	{
		int notifiedHandles[MAX_AVDECC_MSG_CLIENTS] = { 0 };
		U16 notifiedCount = 0;
		U16 sentCount = 0;
		U16 failCount = 0;

		openavbAVDECCNotifyClockDomainStreamsByType(
			configIdx,
			OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT,
			pDescriptorClockDomain,
			pDescriptorClockSource,
			notifiedHandles,
			&notifiedCount,
			&sentCount,
			&failCount);
		openavbAVDECCNotifyClockDomainStreamsByType(
			configIdx,
			OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT,
			pDescriptorClockDomain,
			pDescriptorClockSource,
			notifiedHandles,
			&notifiedCount,
			&sentCount,
			&failCount);

		AVB_LOGF_INFO("SET_CLOCK_SOURCE notify: domain=%u source=%u clients=%u sent=%u failed=%u",
			pDescriptorClockDomain->descriptor_index,
			pDescriptorClockSource->descriptor_index,
			notifiedCount,
			sentCount,
			failCount);
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return TRUE;
}

openavbAvdeccMsgStateType_t openavbAVDECCGetRequestedState(openavb_aem_descriptor_stream_io_t *pDescriptorStream, U16 configIdx)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStream) {
		AVB_LOG_ERROR("openavbAVDECCGetRequestedState Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return OPENAVB_AVDECC_MSG_UNKNOWN;
	}
	if (!pDescriptorStream->stream) {
		AVB_LOG_ERROR("openavbAVDECCGetRequestedState Invalid descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return OPENAVB_AVDECC_MSG_UNKNOWN;
	}
	if (!pDescriptorStream->stream->client) {
		AVB_LOG_DEBUG("openavbAVDECCGetRequestedState Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return OPENAVB_AVDECC_MSG_UNKNOWN;
	}

	// Return the current state.
	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return pDescriptorStream->stream->client->lastRequestedState;
}

openavbAvdeccMsgStateType_t openavbAVDECCGetStreamingState(openavb_aem_descriptor_stream_io_t *pDescriptorStream, U16 configIdx)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity tests.
	if (!pDescriptorStream) {
		AVB_LOG_ERROR("openavbAVDECCGetStreamingState Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return OPENAVB_AVDECC_MSG_UNKNOWN;
	}
	if (!pDescriptorStream->stream) {
		AVB_LOG_ERROR("openavbAVDECCGetStreamingState Invalid descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return OPENAVB_AVDECC_MSG_UNKNOWN;
	}
	if (!pDescriptorStream->stream->client) {
		AVB_LOG_DEBUG("openavbAVDECCGetStreamingState Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return OPENAVB_AVDECC_MSG_UNKNOWN;
	}

	// Return the current state.
	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return pDescriptorStream->stream->client->lastReportedState;
}

void openavbAVDECCPauseStream(openavb_aem_descriptor_stream_io_t *pDescriptor, bool bPause)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	// Sanity test.
	if (!pDescriptor) {
		AVB_LOG_ERROR("openavbAVDECCPauseStream Invalid descriptor");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return;
	}
	if (!pDescriptor->stream) {
		AVB_LOG_ERROR("openavbAVDECCPauseStream Invalid StreamInput descriptor stream");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return;
	}
	if (!pDescriptor->stream->client) {
		AVB_LOG_ERROR("openavbAVDECCPauseStream Invalid stream client pointer");
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return;
	}

	if (pDescriptor->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT) {

		if (bPause) {
			// If the client is not running (or already paused), ignore this command.
			if (pDescriptor->stream->client->lastReportedState != OPENAVB_AVDECC_MSG_RUNNING) {
				AVB_LOG_DEBUG("Listener state change to pause ignored, as Listener not running");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			// Send the request to the client.
			if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptor->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_PAUSED)) {
				AVB_LOG_ERROR("Error requesting Listener change to Paused");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			AVB_LOG_INFO("Listener state change from Running to Paused requested");
		}
		else {
			// If the client is not paused, ignore this command.
			if (pDescriptor->stream->client->lastReportedState != OPENAVB_AVDECC_MSG_PAUSED) {
				AVB_LOG_DEBUG("Listener state change to pause ignored, as Listener not paused");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			// Send the request to the client.
			if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptor->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_RUNNING)) {
				AVB_LOG_ERROR("Error requesting Listener change to Running");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			AVB_LOG_INFO("Listener state change from Paused to Running requested");
		}
	}
	else if (pDescriptor->descriptor_type == OPENAVB_AEM_DESCRIPTOR_STREAM_OUTPUT) {

		if (bPause) {
			// If the client is not running (or already paused), ignore this command.
			if (pDescriptor->stream->client->lastReportedState != OPENAVB_AVDECC_MSG_RUNNING) {
				AVB_LOG_DEBUG("Talker state change to pause ignored, as Talker not running");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			// Send the request to the client.
			if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptor->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_PAUSED)) {
				AVB_LOG_ERROR("Error requesting Talker change to Paused");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			AVB_LOG_INFO("Talker state change from Running to Paused requested");
		}
		else {
			// If the client is not paused, ignore this command.
			if (pDescriptor->stream->client->lastReportedState != OPENAVB_AVDECC_MSG_PAUSED) {
				AVB_LOG_DEBUG("Talker state change to pause ignored, as Talker not paused");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			// Send the request to the client.
			if (!openavbAvdeccMsgSrvrChangeRequest(pDescriptor->stream->client->avdeccMsgHandle, OPENAVB_AVDECC_MSG_RUNNING)) {
				AVB_LOG_ERROR("Error requesting Talker change to Running");
				AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
				return;
			}

			AVB_LOG_INFO("Talker state change from Paused to Running requested");
		}
	}
	else {
		AVB_LOG_ERROR("openavbAVDECCPauseStream unsupported descriptor");
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
}

// Get the current counter value in pValue.  Returns TRUE if the counter is supported, FALSE otherwise.
bool openavbAVDECCGetCounterValue(void *pDescriptor, U16 descriptorType, U32 counterFlag, U32 *pValue)
{
	AVB_TRACE_ENTRY(AVB_TRACE_AVDECC);

	if (!pDescriptor) {
		/* Asked for a non-existing descriptor. */
		AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
		return FALSE;
	}

	switch (descriptorType) {
	case OPENAVB_AEM_DESCRIPTOR_ENTITY:
		// The only counters are entity-specific.
		break;

	case OPENAVB_AEM_DESCRIPTOR_AVB_INTERFACE:
		{
			openavb_aem_descriptor_avb_interface_t *pDescriptorAvbInterface =
				(openavb_aem_descriptor_avb_interface_t *)pDescriptor;
			openavb_avdecc_avb_interface_counter_state_t *pState =
				openavbAvdeccGetAvbInterfaceCounterState(pDescriptorAvbInterface);

			if (!pState) {
				break;
			}
			(void)openavbAvdeccUpdateAvbInterfaceCounters(pDescriptorAvbInterface);

			switch (counterFlag) {
			case OPENAVB_AEM_GET_COUNTERS_COMMAND_AVB_INTERFACE_COUNTER_LINK_UP:
				if (pValue) { *pValue = pState->link_up_count; }
				return TRUE;

			case OPENAVB_AEM_GET_COUNTERS_COMMAND_AVB_INTERFACE_COUNTER_LINK_DOWN:
				if (pValue) { *pValue = pState->link_down_count; }
				return TRUE;

			case OPENAVB_AEM_GET_COUNTERS_COMMAND_AVB_INTERFACE_COUNTER_GPTP_GM_CHANGED:
				if (pValue) { *pValue = pState->grandmaster_changed_count; }
				return TRUE;

			default:
				break;
			}
			break;
		}

	case OPENAVB_AEM_DESCRIPTOR_CLOCK_DOMAIN:
		{
			switch (counterFlag) {
			case OPENAVB_AEM_GET_COUNTERS_COMMAND_CLOCK_DOMAIN_COUNTER_LOCKED:
				if (!openavbClockDomainUpdateCounters()) {
					break;
				}
				if (pValue) { *pValue = s_clock_domain_locked_count; }
				return TRUE;

			case OPENAVB_AEM_GET_COUNTERS_COMMAND_CLOCK_DOMAIN_COUNTER_UNLOCKED:
				if (!openavbClockDomainUpdateCounters()) {
					break;
				}
				if (pValue) { *pValue = s_clock_domain_unlocked_count; }
				return TRUE;

			default:
				break;
			}
			break;
		}

	case OPENAVB_AEM_DESCRIPTOR_STREAM_INPUT:
		// AVDECC_TODO - Get the MEDIA_LOCKED, MEDIA_UNLOCKED, STREAM_RESET, SEQ_NUM_MISMATCH, MEDIA_RESET,
		//     TIMESTAMP_UNCERTAIN, TIMESTAMP_VALID, TIMESTAMP_NOT_VALID, UNSUPPORTED_FORMAT,
		//     LATE_TIMESTAMP, EARLY_TIMESTAMP, FRAMES_RX, and FRAMES_TX counts.
		break;

	default:
		break;
	}

	AVB_TRACE_EXIT(AVB_TRACE_AVDECC);
	return FALSE;
}
