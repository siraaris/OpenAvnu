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

#include "sendmmsg_rawsock.h"

#include "simple_rawsock.h"
#include "openavb_time_osal_pub.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/errqueue.h>
#include <linux/filter.h>
#include <linux/net_tstamp.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#define AVB_LOG_LEVEL AVB_LOG_LEVEL_INFO

#include "openavb_trace.h"
#include "avb_sched.h"

#define	AVB_LOG_COMPONENT	"Raw Socket"
#include "openavb_log.h"

#define SR_CLASS_A_DEFAULT_PRIORITY 3
#define SR_CLASS_B_DEFAULT_PRIORITY 2
#define SENDMMSG_LAUNCH_OFFSET_REFRESH_NS (10000000ULL)
#define SENDMMSG_LAUNCH_OFFSET_LOG_LIMIT 16
#define SENDMMSG_CRF_SUBTYPE 0x04
#define SENDMMSG_CRF_SEQ_SHIFT 8
#define SENDMMSG_CRF_STREAM_UID 8
#define SENDMMSG_DIAG_MIN_LEAD_WARN_NS 300000ULL
#define SENDMMSG_CRF_MIN_LEAD_WARN_NS 1000000ULL
#define SENDMMSG_CRF_SEND_DIAG_INTERVAL 2048ULL

static bool sendmmsgRawsockGetTaiTime(U64 *pTaiTimeNs)
{
	struct timespec ts;

	if (!pTaiTimeNs) {
		return FALSE;
	}

	if (clock_gettime(CLOCK_TAI, &ts) != 0) {
		return FALSE;
	}

	*pTaiTimeNs = ((U64)ts.tv_sec * NANOSECONDS_PER_SECOND) + (U64)ts.tv_nsec;
	return TRUE;
}

static bool sendmmsgRawsockRefreshLaunchOffset(sendmmsg_rawsock_t *rawsock, bool forceRefresh)
{
	U64 monoNowNs = 0;
	U64 wallNowNs = 0;
	U64 taiNowNs = 0;
	S64 wallToTaiOffsetNs = 0;

	if (!rawsock) {
		return FALSE;
	}

	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_MONOTONIC, &monoNowNs)) {
		return FALSE;
	}

	if (!forceRefresh && rawsock->launchTimeClockOffsetValid &&
			monoNowNs >= rawsock->launchTimeOffsetLastUpdateMonoNs &&
			(monoNowNs - rawsock->launchTimeOffsetLastUpdateMonoNs) < SENDMMSG_LAUNCH_OFFSET_REFRESH_NS) {
		return TRUE;
	}

	if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &wallNowNs)) {
		return FALSE;
	}

	if (!sendmmsgRawsockGetTaiTime(&taiNowNs)) {
		return FALSE;
	}

	wallToTaiOffsetNs = (S64)taiNowNs - (S64)wallNowNs;
	rawsock->launchTimeClockOffsetValid = TRUE;
	rawsock->launchTimeWallToTaiOffsetNs = wallToTaiOffsetNs;
	rawsock->launchTimeOffsetLastUpdateMonoNs = monoNowNs;

	if (rawsock->launchTimeOffsetLogCount < SENDMMSG_LAUNCH_OFFSET_LOG_LIMIT) {
		rawsock->launchTimeOffsetLogCount++;
		AVB_LOGF_INFO("TX clock alignment: wall=%llu tai=%llu offset=%lldns sample=%u",
			(unsigned long long)wallNowNs,
			(unsigned long long)taiNowNs,
			(long long)wallToTaiOffsetNs,
			rawsock->launchTimeOffsetLogCount);
	}

	return TRUE;
}

static U64 sendmmsgRawsockTranslateLaunchTime(sendmmsg_rawsock_t *rawsock, U64 wallLaunchTimeNs)
{
	S64 taiLaunchTimeNs;

	if (!rawsock) {
		return wallLaunchTimeNs;
	}

	if (!sendmmsgRawsockRefreshLaunchOffset(rawsock, FALSE)) {
		return wallLaunchTimeNs;
	}

	taiLaunchTimeNs = (S64)wallLaunchTimeNs + rawsock->launchTimeWallToTaiOffsetNs;
	if (taiLaunchTimeNs < 0) {
		return 0;
	}

	return (U64)taiLaunchTimeNs;
}

static bool sendmmsgRawsockSetPriority(sendmmsg_rawsock_t *rawsock, U32 priority, const char *source)
{
	if (priority > 7) {
		AVB_LOGF_WARNING("SO_PRIORITY source=%s requested out-of-range value=%u, clamping to 7",
			source ? source : "unknown", priority);
		priority = 7;
	}

	if (rawsock->socketPriorityValid && rawsock->socketPriority == priority) {
		AVB_LOGF_DEBUG("SO_PRIORITY=%u unchanged from %s", priority, source ? source : "unknown");
		return TRUE;
	}

	int sockPriority = (int)priority;
	if (setsockopt(rawsock->sock, SOL_SOCKET, SO_PRIORITY, &sockPriority, sizeof(sockPriority)) < 0) {
		AVB_LOGF_ERROR("Setting TX priority from %s failed (%d: %s)",
			source ? source : "unknown", errno, strerror(errno));
		return FALSE;
	}

	rawsock->socketPriorityValid = TRUE;
	rawsock->socketPriority = priority;
	AVB_LOGF_DEBUG("SO_PRIORITY=%d set from %s", sockPriority, source ? source : "unknown");
	return TRUE;
}

static U32 sendmmsgRawsockEnvU32(const char *name, U32 defaultValue)
{
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long parsed;

	if (!value || !*value) {
		return defaultValue;
	}

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || (end && *end != '\0') || parsed > UINT32_MAX) {
		AVB_LOGF_WARNING("Ignoring invalid %s=%s", name, value);
		return defaultValue;
	}

	return (U32)parsed;
}

static bool sendmmsgRawsockClassMarkToPriority(int mark, U32 *pPriority)
{
	int fwmarkClass = TC_AVB_MARK_CLASS(mark);
	if (fwmarkClass == SR_CLASS_A) {
		*pPriority = SR_CLASS_A_DEFAULT_PRIORITY;
		return TRUE;
	}
	if (fwmarkClass == SR_CLASS_B) {
		*pPriority = SR_CLASS_B_DEFAULT_PRIORITY;
		return TRUE;
	}
	return FALSE;
}

static void sendmmsgRawsockForceQdiscPath(sendmmsg_rawsock_t *rawsock)
{
#ifdef PACKET_QDISC_BYPASS
	int bypass = 0;
	if (setsockopt(rawsock->sock, SOL_PACKET, PACKET_QDISC_BYPASS, &bypass, sizeof(bypass)) < 0) {
		AVB_LOGF_WARNING("PACKET_QDISC_BYPASS=0 failed (%d: %s); TX queue steering may bypass mqprio/CBS/ETF",
			errno, strerror(errno));
	}
	else {
		AVB_LOG_DEBUG("PACKET_QDISC_BYPASS=0 (qdisc path enabled)");
	}
#endif
}


#if USE_LAUNCHTIME

#ifndef SO_TXTIME
#define SO_TXTIME 61
#endif

#ifndef SCM_TXTIME
#define SCM_TXTIME SO_TXTIME
#endif

#ifndef SOF_TXTIME_REPORT_ERRORS
#define SOF_TXTIME_REPORT_ERRORS (1U << 1)
#endif

static bool launchTimeErrnoIsUnsupported(int err)
{
	switch (err) {
		case EINVAL:
		case EOPNOTSUPP:
#if ENOTSUP != EOPNOTSUPP
		case ENOTSUP:
#endif
		case EPERM:
			return true;
		default:
			return false;
	}
}

static bool sendmmsgRawsockEnableLaunchTime(sendmmsg_rawsock_t *rawsock)
{
	struct sock_txtime txtimeCfg;
	const char *disableLaunchTime = getenv("OPENAVB_DISABLE_SO_TXTIME");
	memset(&txtimeCfg, 0, sizeof(txtimeCfg));

	if (disableLaunchTime && atoi(disableLaunchTime) > 0) {
		AVB_LOGF_INFO("SO_TXTIME disabled by OPENAVB_DISABLE_SO_TXTIME on %s",
			rawsock->base.ifInfo.name);
		rawsock->launchTimeEnabled = false;
		return false;
	}

	// AVTP launch timestamps are in wall-time domain; CLOCK_TAI is the Linux wall clock
	// that tracks gPTP/802.1AS with leap-second-free semantics.
	txtimeCfg.clockid = CLOCK_TAI;
	txtimeCfg.flags = SOF_TXTIME_REPORT_ERRORS;

	if (setsockopt(rawsock->sock, SOL_SOCKET, SO_TXTIME, &txtimeCfg, sizeof(txtimeCfg)) < 0) {
		AVB_LOGF_WARNING("SO_TXTIME unsupported (errno=%d: %s); disabling launch-time on %s",
			errno, strerror(errno), rawsock->base.ifInfo.name);
		rawsock->launchTimeEnabled = false;
		return false;
	}

	return true;
}

#endif /* if USE_LAUNCHTIME */


static void fillmsghdr(struct msghdr *msg, struct iovec *iov,
#if USE_LAUNCHTIME
					   bool useLaunchTime, unsigned char *cmsgbuf, uint64_t time,
#endif
					   void *pktdata, size_t pktlen)
{
	msg->msg_name = NULL;
	msg->msg_namelen = 0;

	iov->iov_base = pktdata;
	iov->iov_len = pktlen;
	msg->msg_iov = iov;
	msg->msg_iovlen = 1;

#if USE_LAUNCHTIME
	if (useLaunchTime) {
		struct cmsghdr *cmsg;
		uint64_t *tsptr;

		msg->msg_control = cmsgbuf;
		msg->msg_controllen = CMSG_LEN(sizeof time);

		cmsg = CMSG_FIRSTHDR(msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_TXTIME;
		cmsg->cmsg_len = CMSG_LEN(sizeof time);

		tsptr = (uint64_t *)CMSG_DATA(cmsg);
		*tsptr = time;
	}
	else {
		msg->msg_control = NULL;
		msg->msg_controllen = 0;
	}
#else
	msg->msg_control = NULL;
	msg->msg_controllen = 0;
#endif

	msg->msg_flags = 0;
}

static void sendmmsgRawsockResetTxState(sendmmsg_rawsock_t *rawsock, const char *reason)
{
	AVB_LOGF_WARNING("Resetting sendmmsg TX state (%s): out=%d ready=%d",
		reason ? reason : "unknown", rawsock->buffersOut, rawsock->buffersReady);
	rawsock->buffersOut = 0;
	rawsock->buffersReady = 0;
}

static int sendmmsgRawsockSendBurst(sendmmsg_rawsock_t *rawsock, int *pErrno)
{
	int attempt;
	int sz;

	for (attempt = 0; attempt <= (int)rawsock->enobufsRetries; ++attempt) {
		sz = sendmmsg(rawsock->sock, rawsock->mmsg, rawsock->buffersReady, 0);
		if (sz >= 0) {
			if (attempt > 0) {
				AVB_LOGF_WARNING("sendmmsg recovered after %d ENOBUFS/EAGAIN retries on %s",
					attempt, rawsock->base.ifInfo.name);
			}
			if (pErrno) {
				*pErrno = 0;
			}
			return sz;
		}

		if (errno != ENOBUFS && errno != EAGAIN) {
			if (pErrno) {
				*pErrno = errno;
			}
			return sz;
		}

		if (attempt == (int)rawsock->enobufsRetries) {
			if (pErrno) {
				*pErrno = errno;
			}
			return sz;
		}

		if (rawsock->enobufsSleepUsec > 0) {
			usleep(rawsock->enobufsSleepUsec);
		}
	}

	if (pErrno) {
		*pErrno = errno;
	}
	return -1;
}

static bool sendmmsgRawsockExtractTxtimeNs(struct msghdr const *msg, U64 *pTxtimeNs)
{
#if USE_LAUNCHTIME
	struct cmsghdr *cmsg;

	if (!msg || !pTxtimeNs || !msg->msg_control || msg->msg_controllen < CMSG_LEN(sizeof(uint64_t))) {
		return FALSE;
	}

	for (cmsg = CMSG_FIRSTHDR((struct msghdr *)msg); cmsg; cmsg = CMSG_NXTHDR((struct msghdr *)msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TXTIME &&
				cmsg->cmsg_len >= CMSG_LEN(sizeof(uint64_t))) {
			memcpy(pTxtimeNs, CMSG_DATA(cmsg), sizeof(*pTxtimeNs));
			return TRUE;
		}
	}
#else
	(void)msg;
	(void)pTxtimeNs;
#endif
	return FALSE;
}

static void sendmmsgRawsockDescribePacket(U8 const *pkt, size_t pktLen, char *buffer, size_t bufferLen)
{
	size_t l2Len = ETH_HLEN;
	U16 etherType = 0;
	U8 subtype = 0;
	U16 streamUid = 0;
	U8 const *streamId = NULL;

	if (!buffer || bufferLen == 0) {
		return;
	}
	buffer[0] = '\0';

	if (!pkt || pktLen < ETH_HLEN) {
		snprintf(buffer, bufferLen, "pkt_len=%zu", pktLen);
		return;
	}

	memcpy(&etherType, pkt + 12, sizeof(etherType));
	etherType = ntohs(etherType);
	if ((etherType == ETHERTYPE_VLAN || etherType == 0x88A8) && pktLen >= (ETH_HLEN + 4U)) {
		l2Len += 4U;
		memcpy(&etherType, pkt + 16, sizeof(etherType));
		etherType = ntohs(etherType);
	}

	if (pktLen < l2Len + 12U) {
		snprintf(buffer, bufferLen, "eth=0x%04x l2=%zu pkt_len=%zu", etherType, l2Len, pktLen);
		return;
	}

	subtype = pkt[l2Len] & 0x7FU;
	streamId = pkt + l2Len + 4U;
	streamUid = (U16)(((U16)streamId[6] << 8) | (U16)streamId[7]);

	snprintf(buffer, bufferLen,
		"eth=0x%04x l2=%zu subtype=0x%02x stream_id=%02x:%02x:%02x:%02x:%02x:%02x/%u pkt_len=%zu",
		etherType,
		l2Len,
		subtype,
		streamId[0], streamId[1], streamId[2], streamId[3], streamId[4], streamId[5],
		streamUid,
		pktLen);
}

static void sendmmsgRawsockClearTxMeta(sendmmsg_rawsock_t *rawsock, int index)
{
	if (!rawsock || index < 0 || index >= MSG_COUNT) {
		return;
	}

	rawsock->txMetaValid[index] = false;
	rawsock->txSubtype[index] = 0;
	rawsock->txStreamUid[index] = 0;
	rawsock->txSeq[index] = 0;
	rawsock->txSeqValid[index] = false;
}

static bool sendmmsgRawsockExtractTxMeta(U8 const *pkt, size_t pktLen,
	U8 *pSubtype, U16 *pStreamUid, U8 *pSeq, bool *pSeqValid)
{
	size_t l2Len = ETH_HLEN;
	U16 etherType = 0;
	U32 subtypeData = 0;
	U8 const *pAvtp = NULL;
	U8 const *pStreamId = NULL;

	if (!pkt || pktLen < ETH_HLEN || !pSubtype || !pStreamUid || !pSeq || !pSeqValid) {
		return FALSE;
	}

	memcpy(&etherType, pkt + 12, sizeof(etherType));
	etherType = ntohs(etherType);
	if ((etherType == ETHERTYPE_VLAN || etherType == 0x88A8) && pktLen >= (ETH_HLEN + 4U)) {
		l2Len += 4U;
		memcpy(&etherType, pkt + 16, sizeof(etherType));
		etherType = ntohs(etherType);
	}

	if (pktLen < l2Len + 12U) {
		return FALSE;
	}

	pAvtp = pkt + l2Len;
	pStreamId = pAvtp + 4U;
	*pSubtype = pAvtp[0] & 0x7FU;
	*pStreamUid = (U16)(((U16)pStreamId[6] << 8) | (U16)pStreamId[7]);
	*pSeqValid = TRUE;
	if (*pSubtype == SENDMMSG_CRF_SUBTYPE) {
		memcpy(&subtypeData, pAvtp, sizeof(subtypeData));
		subtypeData = ntohl(subtypeData);
		*pSeq = (U8)((subtypeData >> SENDMMSG_CRF_SEQ_SHIFT) & 0xFF);
	}
	else {
		*pSeq = pAvtp[2];
	}

	return TRUE;
}

static bool sendmmsgRawsockTrackCrf(U8 subtype, U16 streamUid, bool seqValid)
{
	return (seqValid && subtype == SENDMMSG_CRF_SUBTYPE && streamUid == SENDMMSG_CRF_STREAM_UID);
}

static void sendmmsgRawsockLogCrfSeqRow(sendmmsg_rawsock_t *rawsock, const char *stage, const char *event,
	U32 *pLogCount, int slot, int sent, int ready, U8 seq, U8 expectedSeq, U64 requestedLaunchNs, U64 kernelLaunchNs)
{
	if (!rawsock || !stage || !event || !pLogCount) {
		return;
	}

	(*pLogCount)++;
	if (*pLogCount <= 16U || ((*pLogCount % 256U) == 0U)) {
		AVB_LOGF_WARNING(
			"TX OUTLIER ROW flags=..Z stage=%s event=%s stream=%u slot=%d sent=%d ready=%d seq=%u expected=%u requested_launch=%" PRIu64 " kernel_launch=%" PRIu64 " rows=%u",
			stage,
			event,
			(unsigned)SENDMMSG_CRF_STREAM_UID,
			slot,
			sent,
			ready,
			(unsigned)seq,
			(unsigned)expectedSeq,
			requestedLaunchNs,
			kernelLaunchNs,
			*pLogCount);
	}
}

static void sendmmsgRawsockLogCrfLeadRow(sendmmsg_rawsock_t *rawsock, const char *stage, const char *event,
	U32 *pLogCount, int slot, U8 seq, U64 requestedLaunchNs, U64 kernelLaunchNs,
	U64 readyWallNs, U64 readyTaiNs, U64 sendCallTaiNs, U64 sendReturnTaiNs,
	S64 readyWallLeadNs, S64 readyKernelLeadNs, S64 sendCallLeadNs, S64 sendReturnLeadNs,
	U64 minLeadNs)
{
	if (!rawsock || !stage || !event || !pLogCount) {
		return;
	}

	(*pLogCount)++;
	if (*pLogCount <= 16U || ((*pLogCount % 256U) == 0U)) {
		AVB_LOGF_WARNING(
			"TX OUTLIER ROW flags=..Z stage=%s event=%s stream=%u slot=%d seq=%u requested_launch=%" PRIu64 " kernel_launch=%" PRIu64 " ready_wall=%" PRIu64 " ready_tai=%" PRIu64 " send_call=%" PRIu64 " send_return=%" PRIu64 " lead_ready_wall=%" PRId64 " lead_ready_kernel=%" PRId64 " lead_send_call=%" PRId64 " lead_send_return=%" PRId64 " min_lead_ns=%" PRIu64 " rows=%u",
			stage,
			event,
			(unsigned)SENDMMSG_CRF_STREAM_UID,
			slot,
			(unsigned)seq,
			requestedLaunchNs,
			kernelLaunchNs,
			readyWallNs,
			readyTaiNs,
			sendCallTaiNs,
			sendReturnTaiNs,
			readyWallLeadNs,
			readyKernelLeadNs,
			sendCallLeadNs,
			sendReturnLeadNs,
			minLeadNs,
			*pLogCount);
	}
}

static void sendmmsgRawsockLogCrfLaunchStepRow(sendmmsg_rawsock_t *rawsock, const char *event,
	U32 *pLogCount, int slot, U8 seq, U8 prevSeq, U64 requestedLaunchNs, U64 prevRequestedLaunchNs,
	U64 kernelLaunchNs, U64 prevKernelLaunchNs, S64 requestedStepNs, S64 kernelStepNs)
{
	if (!rawsock || !event || !pLogCount) {
		return;
	}

	(*pLogCount)++;
	if (*pLogCount <= 16U || ((*pLogCount % 256U) == 0U)) {
		AVB_LOGF_WARNING(
			"TX OUTLIER ROW flags=..Z stage=send_queue_launch event=%s stream=%u slot=%d seq=%u prev_seq=%u requested_launch=%" PRIu64 " prev_requested_launch=%" PRIu64 " kernel_launch=%" PRIu64 " prev_kernel_launch=%" PRIu64 " requested_step_ns=%" PRId64 " kernel_step_ns=%" PRId64 " rows=%u",
			event,
			(unsigned)SENDMMSG_CRF_STREAM_UID,
			slot,
			(unsigned)seq,
			(unsigned)prevSeq,
			requestedLaunchNs,
			prevRequestedLaunchNs,
			kernelLaunchNs,
			prevKernelLaunchNs,
			requestedStepNs,
			kernelStepNs,
			*pLogCount);
	}
}

static void sendmmsgRawsockLogBurstFailure(sendmmsg_rawsock_t *rawsock, int savedErrno, bool hadLaunchTimeCmsg)
{
	U64 taiNowNs = 0;
	U64 firstTxtimeNs = 0;
	U64 lastTxtimeNs = 0;
	S64 firstLeadNs = 0;
	S64 lastLeadNs = 0;
	bool haveTaiNow = FALSE;
	bool haveFirstTxtime = FALSE;
	bool haveLastTxtime = FALSE;
	int outqBytes = -1;
	int sndbufBytes = -1;
	socklen_t sndbufLen = sizeof(sndbufBytes);
	char pktDesc[192];

	if (!rawsock) {
		return;
	}

	haveTaiNow = sendmmsgRawsockGetTaiTime(&taiNowNs);
	if (rawsock->buffersReady > 0) {
		haveFirstTxtime = sendmmsgRawsockExtractTxtimeNs(&rawsock->mmsg[0].msg_hdr, &firstTxtimeNs);
		haveLastTxtime = sendmmsgRawsockExtractTxtimeNs(&rawsock->mmsg[rawsock->buffersReady - 1].msg_hdr, &lastTxtimeNs);
		sendmmsgRawsockDescribePacket(rawsock->pktbuf[0], rawsock->miov[0].iov_len, pktDesc, sizeof(pktDesc));
	}
	else {
		snprintf(pktDesc, sizeof(pktDesc), "no-packets");
	}

	if (haveTaiNow && haveFirstTxtime) {
		firstLeadNs = (firstTxtimeNs >= taiNowNs)
			? (S64)(firstTxtimeNs - taiNowNs)
			: -((S64)(taiNowNs - firstTxtimeNs));
	}
	if (haveTaiNow && haveLastTxtime) {
		lastLeadNs = (lastTxtimeNs >= taiNowNs)
			? (S64)(lastTxtimeNs - taiNowNs)
			: -((S64)(taiNowNs - lastTxtimeNs));
	}

	if (ioctl(rawsock->sock, TIOCOUTQ, &outqBytes) < 0) {
		outqBytes = -1;
	}
	if (getsockopt(rawsock->sock, SOL_SOCKET, SO_SNDBUF, &sndbufBytes, &sndbufLen) < 0) {
		sndbufBytes = -1;
	}

	AVB_LOGF_WARNING(
		"sendmmsg errno=%d (%s) sock=%d ready=%d out=%d launchEnabled=%d sockConfigured=%d hadLaunchCmsg=%d tai_now=%" PRIu64 " first_txtime=%" PRIu64 " first_lead=%" PRId64 "ns last_txtime=%" PRIu64 " last_lead=%" PRId64 "ns wall_to_tai=%" PRId64 "ns outq=%d sndbuf=%d %s",
		savedErrno,
		strerror(savedErrno),
		rawsock->sock,
		rawsock->buffersReady,
		rawsock->buffersOut,
		rawsock->launchTimeEnabled ? 1 : 0,
		rawsock->launchTimeSockConfigured ? 1 : 0,
		hadLaunchTimeCmsg ? 1 : 0,
		haveTaiNow ? taiNowNs : 0ULL,
		haveFirstTxtime ? firstTxtimeNs : 0ULL,
		(haveTaiNow && haveFirstTxtime) ? firstLeadNs : 0LL,
		haveLastTxtime ? lastTxtimeNs : 0ULL,
		(haveTaiNow && haveLastTxtime) ? lastLeadNs : 0LL,
		rawsock->launchTimeClockOffsetValid ? rawsock->launchTimeWallToTaiOffsetNs : 0LL,
		outqBytes,
		sndbufBytes,
		pktDesc);
}

static const char *sendmmsgRawsockErrqueueEventName(U8 origin, U8 code)
{
	if (origin == SO_EE_ORIGIN_TXTIME) {
		switch (code) {
			case SO_EE_CODE_TXTIME_INVALID_PARAM:
				return "txtime_invalid_param";
			case SO_EE_CODE_TXTIME_MISSED:
				return "txtime_missed";
			default:
				return "txtime_other";
		}
	}

	switch (origin) {
		case SO_EE_ORIGIN_LOCAL:
			return "local";
		case SO_EE_ORIGIN_TXSTATUS:
			return "txstatus";
		default:
			return "other";
	}
}

static void sendmmsgRawsockDrainErrqueue(sendmmsg_rawsock_t *rawsock)
{
#if USE_LAUNCHTIME
	unsigned char pktbuf[MAX_FRAME_SIZE];
	unsigned char control[512];
	struct iovec iov;
	struct msghdr msg;

	if (!rawsock || rawsock->sock < 0 || !rawsock->launchTimeSockConfigured) {
		return;
	}

	for (;;) {
		int rc;
		bool logged = FALSE;
		U8 subtype = 0;
		U16 streamUid = 0;
		U8 seq = 0;
		bool seqValid = FALSE;
		bool haveMeta = FALSE;
		char pktDesc[192];
		struct cmsghdr *cmsg;

		memset(&msg, 0, sizeof(msg));
		memset(&iov, 0, sizeof(iov));
		memset(control, 0, sizeof(control));
		memset(pktbuf, 0, sizeof(pktbuf));
		iov.iov_base = pktbuf;
		iov.iov_len = sizeof(pktbuf);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);

		rc = recvmsg(rawsock->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
		if (rc < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			rawsock->crfErrqueueLogCount++;
			if (rawsock->crfErrqueueLogCount <= 8U || ((rawsock->crfErrqueueLogCount % 256U) == 0U)) {
				AVB_LOGF_WARNING(
					"TX OUTLIER ROW flags=..Z stage=send_errqueue event=recvmsg_failed stream=0 sock=%d mark=%d errno=%d rows=%u",
					rawsock->sock,
					rawsock->socketMarkValid ? rawsock->socketMark : 0,
					errno,
					rawsock->crfErrqueueLogCount);
			}
			break;
		}

		rawsock->diagErrqueueEvents++;
		haveMeta = sendmmsgRawsockExtractTxMeta(pktbuf, (size_t)rc, &subtype, &streamUid, &seq, &seqValid);
		sendmmsgRawsockDescribePacket(pktbuf, (size_t)rc, pktDesc, sizeof(pktDesc));

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			struct sock_extended_err *see;
			const char *event;

			if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct sock_extended_err))) {
				continue;
			}

			see = (struct sock_extended_err *)CMSG_DATA(cmsg);
			if (!see) {
				continue;
			}

			if (see->ee_origin == SO_EE_ORIGIN_TXTIME && see->ee_code == SO_EE_CODE_TXTIME_MISSED) {
				rawsock->diagErrqueueMissed++;
			}

			event = sendmmsgRawsockErrqueueEventName(see->ee_origin, see->ee_code);
			rawsock->crfErrqueueLogCount++;
			if (rawsock->crfErrqueueLogCount <= 32U || ((rawsock->crfErrqueueLogCount % 256U) == 0U)) {
				AVB_LOGF_WARNING(
					"TX OUTLIER ROW flags=..Z stage=send_errqueue event=%s stream=%u sock=%d mark=%d origin=%u code=%u ee_errno=%u ee_info=%u ee_data=%u cmsg_level=%d cmsg_type=%d pkt_len=%d subtype=0x%02x seq=%u seq_valid=%u desc=%s rows=%u",
					event,
					haveMeta ? (unsigned)streamUid : 0U,
					rawsock->sock,
					rawsock->socketMarkValid ? rawsock->socketMark : 0,
					(unsigned)see->ee_origin,
					(unsigned)see->ee_code,
					(unsigned)see->ee_errno,
					(unsigned)see->ee_info,
					(unsigned)see->ee_data,
					cmsg->cmsg_level,
					cmsg->cmsg_type,
					rc,
					haveMeta ? subtype : 0U,
					haveMeta ? (unsigned)seq : 0U,
					haveMeta ? 1U : 0U,
					pktDesc,
					rawsock->crfErrqueueLogCount);
			}
			logged = TRUE;
		}

		if (!logged) {
			rawsock->crfErrqueueLogCount++;
			if (rawsock->crfErrqueueLogCount <= 16U || ((rawsock->crfErrqueueLogCount % 256U) == 0U)) {
				AVB_LOGF_WARNING(
					"TX OUTLIER ROW flags=..Z stage=send_errqueue event=no_extended_err stream=%u sock=%d mark=%d pkt_len=%d flags=0x%x desc=%s rows=%u",
					haveMeta ? (unsigned)streamUid : 0U,
					rawsock->sock,
					rawsock->socketMarkValid ? rawsock->socketMark : 0,
					rc,
					msg.msg_flags,
					pktDesc,
					rawsock->crfErrqueueLogCount);
			}
		}
	}
#else
	(void)rawsock;
#endif
}

static void sendmmsgRawsockDiagReset(sendmmsg_rawsock_t *rawsock)
{
	if (!rawsock) {
		return;
	}

	rawsock->diagBurstCount = 0;
	rawsock->diagPacketCount = 0;
	rawsock->diagLaunchMetricPacketCount = 0;
	rawsock->diagNoLaunchTimeCount = 0;
	rawsock->diagLateAtReadyCount = 0;
	rawsock->diagLateAtSendCallCount = 0;
	rawsock->diagLateAtSendReturnCount = 0;
	rawsock->diagErrqueueEvents = 0;
	rawsock->diagErrqueueMissed = 0;
	rawsock->diagOutlierRowCount = 0;
	rawsock->diagQueueBeforeSendMinNs = 0;
	rawsock->diagQueueBeforeSendMaxNs = 0;
	rawsock->diagQueueBeforeSendSumNs = 0;
	rawsock->diagSubmitLatencyMinNs = 0;
	rawsock->diagSubmitLatencyMaxNs = 0;
	rawsock->diagSubmitLatencySumNs = 0;
	rawsock->diagReadyWallLeadMinNs = 0;
	rawsock->diagReadyWallLeadMaxNs = 0;
	rawsock->diagReadyWallLeadSumNs = 0;
	rawsock->diagReadyKernelLeadMinNs = 0;
	rawsock->diagReadyKernelLeadMaxNs = 0;
	rawsock->diagReadyKernelLeadSumNs = 0;
	rawsock->diagSendCallLeadMinNs = 0;
	rawsock->diagSendCallLeadMaxNs = 0;
	rawsock->diagSendCallLeadSumNs = 0;
	rawsock->diagSendReturnLeadMinNs = 0;
	rawsock->diagSendReturnLeadMaxNs = 0;
	rawsock->diagSendReturnLeadSumNs = 0;
}

static void sendmmsgRawsockDiagUpdateUnsigned(U64 *pMin, U64 *pMax, U64 *pSum, U64 count, U64 value)
{
	if (count == 0) {
		*pMin = value;
		*pMax = value;
		*pSum = value;
		return;
	}

	if (value < *pMin) {
		*pMin = value;
	}
	if (value > *pMax) {
		*pMax = value;
	}
	*pSum += value;
}

static void sendmmsgRawsockDiagUpdateSigned(S64 *pMin, S64 *pMax, S64 *pSum, U64 count, S64 value)
{
	if (count == 0) {
		*pMin = value;
		*pMax = value;
		*pSum = value;
		return;
	}

	if (value < *pMin) {
		*pMin = value;
	}
	if (value > *pMax) {
		*pMax = value;
	}
	*pSum += value;
}

static void sendmmsgRawsockDiagMaybeLog(sendmmsg_rawsock_t *rawsock)
{
	U64 leadCount = 0;

	if (!rawsock || !rawsock->diagTimingEnabled || rawsock->diagTimingLogInterval == 0) {
		return;
	}

	if (rawsock->diagBurstCount < rawsock->diagTimingLogInterval || rawsock->diagBurstCount == 0) {
		return;
	}

	leadCount = rawsock->diagLaunchMetricPacketCount;
	AVB_LOGF_WARNING(
		"SENDMMSG DIAG bursts=%llu packets=%llu queue_ready_to_send_call_avg=%lluns min=%lluns max=%lluns submit_latency_avg=%lluns min=%lluns max=%lluns lead_ready_wall_avg=%lldns min=%lldns max=%lldns lead_ready_kernel_avg=%lldns min=%lldns max=%lldns lead_send_call_avg=%lldns min=%lldns max=%lldns lead_send_return_avg=%lldns min=%lldns max=%lldns late_ready=%llu late_send_call=%llu late_send_return=%llu no_launch=%llu errqueue=%llu errqueue_missed=%llu",
		(unsigned long long)rawsock->diagBurstCount,
		(unsigned long long)rawsock->diagPacketCount,
		(unsigned long long)((rawsock->diagPacketCount > 0) ? (rawsock->diagQueueBeforeSendSumNs / rawsock->diagPacketCount) : 0ULL),
		(unsigned long long)rawsock->diagQueueBeforeSendMinNs,
		(unsigned long long)rawsock->diagQueueBeforeSendMaxNs,
		(unsigned long long)((rawsock->diagBurstCount > 0) ? (rawsock->diagSubmitLatencySumNs / rawsock->diagBurstCount) : 0ULL),
		(unsigned long long)rawsock->diagSubmitLatencyMinNs,
		(unsigned long long)rawsock->diagSubmitLatencyMaxNs,
		(long long)((leadCount > 0) ? (rawsock->diagReadyWallLeadSumNs / (S64)leadCount) : 0LL),
		(long long)rawsock->diagReadyWallLeadMinNs,
		(long long)rawsock->diagReadyWallLeadMaxNs,
		(long long)((leadCount > 0) ? (rawsock->diagReadyKernelLeadSumNs / (S64)leadCount) : 0LL),
		(long long)rawsock->diagReadyKernelLeadMinNs,
		(long long)rawsock->diagReadyKernelLeadMaxNs,
		(long long)((leadCount > 0) ? (rawsock->diagSendCallLeadSumNs / (S64)leadCount) : 0LL),
		(long long)rawsock->diagSendCallLeadMinNs,
		(long long)rawsock->diagSendCallLeadMaxNs,
		(long long)((leadCount > 0) ? (rawsock->diagSendReturnLeadSumNs / (S64)leadCount) : 0LL),
		(long long)rawsock->diagSendReturnLeadMinNs,
		(long long)rawsock->diagSendReturnLeadMaxNs,
		(unsigned long long)rawsock->diagLateAtReadyCount,
		(unsigned long long)rawsock->diagLateAtSendCallCount,
		(unsigned long long)rawsock->diagLateAtSendReturnCount,
		(unsigned long long)rawsock->diagNoLaunchTimeCount,
		(unsigned long long)rawsock->diagErrqueueEvents,
		(unsigned long long)rawsock->diagErrqueueMissed);

	sendmmsgRawsockDiagReset(rawsock);
}

static void sendmmsgRawsockCrfDiagReset(sendmmsg_rawsock_t *rawsock)
{
	if (!rawsock) {
		return;
	}

	rawsock->crfDiagPacketCount = 0;
	rawsock->crfDiagLeadMetricCount = 0;
	rawsock->crfDiagReadyKernelLeadMinNs = 0;
	rawsock->crfDiagReadyKernelLeadMaxNs = 0;
	rawsock->crfDiagReadyKernelLeadSumNs = 0;
	rawsock->crfDiagSendCallLeadMinNs = 0;
	rawsock->crfDiagSendCallLeadMaxNs = 0;
	rawsock->crfDiagSendCallLeadSumNs = 0;
	rawsock->crfDiagSendReturnLeadMinNs = 0;
	rawsock->crfDiagSendReturnLeadMaxNs = 0;
	rawsock->crfDiagSendReturnLeadSumNs = 0;
	rawsock->crfDiagLowLeadCount = 0;
	rawsock->crfDiagLateCount = 0;
	rawsock->crfDiagNoLaunchCount = 0;
	rawsock->crfDiagBurstReadyMax = 0;
}

static void sendmmsgRawsockCrfDiagMaybeLog(sendmmsg_rawsock_t *rawsock)
{
	U64 leadCount = 0;
	S64 readyKernelLeadAvgNs = 0;
	S64 sendCallLeadAvgNs = 0;
	S64 sendReturnLeadAvgNs = 0;

	if (!rawsock || rawsock->crfDiagPacketCount < SENDMMSG_CRF_SEND_DIAG_INTERVAL) {
		return;
	}

	leadCount = rawsock->crfDiagLeadMetricCount;
	readyKernelLeadAvgNs = (leadCount > 0) ? (rawsock->crfDiagReadyKernelLeadSumNs / (S64)leadCount) : 0LL;
	sendCallLeadAvgNs = (leadCount > 0) ? (rawsock->crfDiagSendCallLeadSumNs / (S64)leadCount) : 0LL;
	sendReturnLeadAvgNs = (leadCount > 0) ? (rawsock->crfDiagSendReturnLeadSumNs / (S64)leadCount) : 0LL;
	AVB_LOGF_WARNING(
		"TX TRACE ROW flags=..Z stage=send_window event=diag stream=%u pkts=%" PRIu64 " lead_pkts=%" PRIu64 " rk_avg=%" PRId64 " rk_min=%" PRId64 " rk_max=%" PRId64 " low=%" PRIu64 " late=%" PRIu64 " nol=%" PRIu64 " burst=%u min_lead_ns=%" PRIu64,
		(unsigned)SENDMMSG_CRF_STREAM_UID,
		rawsock->crfDiagPacketCount,
		leadCount,
		readyKernelLeadAvgNs,
		rawsock->crfDiagReadyKernelLeadMinNs,
		rawsock->crfDiagReadyKernelLeadMaxNs,
		rawsock->crfDiagLowLeadCount,
		rawsock->crfDiagLateCount,
		rawsock->crfDiagNoLaunchCount,
		rawsock->crfDiagBurstReadyMax,
		rawsock->crfMinLeadWarnNs);

	if (rawsock->crfDiagLowLeadCount > 0 || rawsock->crfDiagLateCount > 0 || rawsock->crfDiagNoLaunchCount > 0) {
		AVB_LOGF_WARNING(
			"TX TRACE ROW flags=..Z stage=send_window_detail event=diag stream=%u sc_avg=%" PRId64 " sc_min=%" PRId64 " sc_max=%" PRId64 " sr_avg=%" PRId64 " sr_min=%" PRId64 " sr_max=%" PRId64,
			(unsigned)SENDMMSG_CRF_STREAM_UID,
			sendCallLeadAvgNs,
			rawsock->crfDiagSendCallLeadMinNs,
			rawsock->crfDiagSendCallLeadMaxNs,
			sendReturnLeadAvgNs,
			rawsock->crfDiagSendReturnLeadMinNs,
			rawsock->crfDiagSendReturnLeadMaxNs);
	}

	sendmmsgRawsockCrfDiagReset(rawsock);
}

// Open a rawsock for TX or RX
void* sendmmsgRawsockOpen(sendmmsg_rawsock_t* rawsock, const char *ifname, bool rx_mode, bool tx_mode, U16 ethertype, U32 frame_size, U32 num_frames)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK);

	AVB_LOGF_DEBUG("Open, ifname=%s, rx=%d, tx=%d, ethertype=%x size=%d, num=%d",
				   ifname, rx_mode, tx_mode, ethertype, frame_size, num_frames);

	baseRawsockOpen(&rawsock->base, ifname, rx_mode, tx_mode, ethertype, frame_size, num_frames);

	rawsock->sock = -1;

	// Get info about the network device
	if (!simpleAvbCheckInterface(ifname, &(rawsock->base.ifInfo))) {
		AVB_LOGF_ERROR("Creating rawsock; bad interface name: %s", ifname);
		free(rawsock);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
		return NULL;
	}

	// Deal with frame size.
	if (rawsock->base.frameSize == 0) {
		// use interface MTU as max frames size, if none specified
		rawsock->base.frameSize = rawsock->base.ifInfo.mtu + ETH_HLEN + VLAN_HLEN;
	}
	else if (rawsock->base.frameSize > rawsock->base.ifInfo.mtu + ETH_HLEN + VLAN_HLEN) {
		AVB_LOG_ERROR("Creating raswsock; requested frame size exceeds MTU");
		free(rawsock);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
		return NULL;
	}
	rawsock->base.frameSize = TPACKET_ALIGN(rawsock->base.frameSize);
	assert(rawsock->base.frameSize <= MAX_FRAME_SIZE);

	// Prepare default Ethernet header.
	rawsock->base.ethHdrLen = sizeof(eth_hdr_t);
	memset(&(rawsock->base.ethHdr.notag.dhost), 0xFF, ETH_ALEN);
	memcpy(&(rawsock->base.ethHdr.notag.shost), &(rawsock->base.ifInfo.mac), ETH_ALEN);
	rawsock->base.ethHdr.notag.ethertype = htons(rawsock->base.ethertype);

	// Create socket
	rawsock->sock = socket(PF_PACKET, SOCK_RAW, htons(rawsock->base.ethertype));
	if (rawsock->sock == -1) {
		AVB_LOGF_ERROR("Creating rawsock; opening socket: %s", strerror(errno));
		sendmmsgRawsockClose(rawsock);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
		return NULL;
	}

	// Allow address reuse
	int temp = 1;
	if(setsockopt(rawsock->sock, SOL_SOCKET, SO_REUSEADDR, &temp, sizeof(int)) < 0) {
		AVB_LOG_ERROR("Creating rawsock; failed to set reuseaddr");
		sendmmsgRawsockClose(rawsock);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
		return NULL;
	}

	rawsock->enobufsRetries = sendmmsgRawsockEnvU32("OPENAVB_SENDMMSG_ENOBUFS_RETRIES", SENDMMSG_ENOBUFS_RETRIES);
	rawsock->enobufsSleepUsec = sendmmsgRawsockEnvU32("OPENAVB_SENDMMSG_ENOBUFS_SLEEP_USEC", SENDMMSG_ENOBUFS_SLEEP_USEC);
	rawsock->txSndbufBytes = sendmmsgRawsockEnvU32("OPENAVB_SENDMMSG_TX_SNDBUF_BYTES", SENDMMSG_TX_SNDBUF_BYTES);

	temp = (int)rawsock->txSndbufBytes;
	if (setsockopt(rawsock->sock, SOL_SOCKET, SO_SNDBUF, &temp, sizeof(temp)) < 0) {
		AVB_LOGF_WARNING("Creating rawsock; failed to set SO_SNDBUF=%d (%d: %s)",
			temp, errno, strerror(errno));
	}
	AVB_LOGF_INFO("sendmmsg TX config on %s: sndbuf=%u enobufs_retries=%u enobufs_sleep_usec=%u",
		rawsock->base.ifInfo.name,
		rawsock->txSndbufBytes,
		rawsock->enobufsRetries,
		rawsock->enobufsSleepUsec);

	sendmmsgRawsockForceQdiscPath(rawsock);

	// Bind to interface
	struct sockaddr_ll my_addr;
	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sll_family = PF_PACKET;
	my_addr.sll_protocol = htons(rawsock->base.ethertype);
	my_addr.sll_ifindex = rawsock->base.ifInfo.index;

	if (bind(rawsock->sock, (struct sockaddr*)&my_addr, sizeof(my_addr)) == -1) {
		AVB_LOGF_ERROR("Creating rawsock; bind socket: %s", strerror(errno));
		sendmmsgRawsockClose(rawsock);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
		return NULL;
	}

	// Clear our buffers and other tracking data
	memset(rawsock->mmsg, 0, sizeof(rawsock->mmsg));
	memset(rawsock->miov, 0, sizeof(rawsock->miov));
	memset(rawsock->pktbuf, 0, sizeof(rawsock->pktbuf));
	memset(rawsock->txMetaValid, 0, sizeof(rawsock->txMetaValid));
	memset(rawsock->txSubtype, 0, sizeof(rawsock->txSubtype));
	memset(rawsock->txStreamUid, 0, sizeof(rawsock->txStreamUid));
	memset(rawsock->txSeq, 0, sizeof(rawsock->txSeq));
	memset(rawsock->txSeqValid, 0, sizeof(rawsock->txSeqValid));
	rawsock->socketPriorityValid = false;
	rawsock->socketPriority = 0;
	rawsock->socketMarkValid = false;
	rawsock->socketMark = 0;
#if USE_LAUNCHTIME
	memset(rawsock->cmsgbuf, 0, sizeof(rawsock->cmsgbuf));
	rawsock->launchTimeEnabled = true;
	rawsock->launchTimeSockConfigured = false;
	rawsock->launchTimeFallbackLogged = false;
	rawsock->launchTimeClockOffsetValid = false;
	rawsock->launchTimeWallToTaiOffsetNs = 0;
	rawsock->launchTimeOffsetLastUpdateMonoNs = 0;
	rawsock->launchTimeOffsetLogCount = 0;
	memset(rawsock->txReadyWallNs, 0, sizeof(rawsock->txReadyWallNs));
	memset(rawsock->txReadyTaiNs, 0, sizeof(rawsock->txReadyTaiNs));
	memset(rawsock->txRequestedWallLaunchNs, 0, sizeof(rawsock->txRequestedWallLaunchNs));
	memset(rawsock->txKernelLaunchNs, 0, sizeof(rawsock->txKernelLaunchNs));
	rawsock->diagTimingLogInterval = 0;
	{
		const char *envDiagInterval = getenv("OPENAVB_SENDMMSG_DIAG_INTERVAL");
		if (envDiagInterval && *envDiagInterval) {
			char *endPtr = NULL;
			unsigned long parsed = strtoul(envDiagInterval, &endPtr, 10);
			if (endPtr != envDiagInterval && endPtr && *endPtr == '\0' && parsed > 0) {
				rawsock->diagTimingLogInterval = (U32)parsed;
			}
		}
	}
	rawsock->diagTimingEnabled = (rawsock->diagTimingLogInterval > 0);
	rawsock->diagMinLeadWarnNs = (U64)sendmmsgRawsockEnvU32("OPENAVB_SENDMMSG_DIAG_MIN_LEAD_NS", (U32)SENDMMSG_DIAG_MIN_LEAD_WARN_NS);
	rawsock->crfMinLeadWarnNs = (U64)sendmmsgRawsockEnvU32(
		"OPENAVB_SENDMMSG_CRF_MIN_LEAD_NS",
		(U32)((rawsock->diagMinLeadWarnNs > SENDMMSG_CRF_MIN_LEAD_WARN_NS)
			? rawsock->diagMinLeadWarnNs
			: SENDMMSG_CRF_MIN_LEAD_WARN_NS));
	sendmmsgRawsockDiagReset(rawsock);
	rawsock->crfLastQueuedSeqValid = false;
	rawsock->crfLastQueuedSeq = 0;
	rawsock->crfLastQueuedLaunchValid = false;
	rawsock->crfLastQueuedRequestedLaunchNs = 0;
	rawsock->crfLastQueuedKernelLaunchNs = 0;
	rawsock->crfLastSentSeqValid = false;
	rawsock->crfLastSentSeq = 0;
	sendmmsgRawsockCrfDiagReset(rawsock);
	rawsock->crfLowLeadLogCount = 0;
	rawsock->crfNoLaunchLogCount = 0;
	rawsock->crfLateLeadLogCount = 0;
	rawsock->crfQueueGapLogCount = 0;
	rawsock->crfLaunchStepLogCount = 0;
	rawsock->crfSendGapLogCount = 0;
	rawsock->crfErrqueueLogCount = 0;
#endif

	rawsock->buffersOut = 0;
	rawsock->buffersReady = 0;
	rawsock->frameCount = MSG_COUNT;

	// fill virtual functions table
	rawsock_cb_t *cb = &rawsock->base.cb;
	cb->close = sendmmsgRawsockClose;
	cb->getTxFrame = sendmmsgRawsockGetTxFrame;
	cb->txSetMark = sendmmsgRawsockTxSetMark;
	cb->relTxFrame = sendmmsgRawsockRelTxFrame;
	cb->txSetHdr = sendmmsgRawsockTxSetHdr;
	cb->txFrameReady = sendmmsgRawsockTxFrameReady;
	cb->send = sendmmsgRawsockSend;
	cb->txBufLevel = sendmmsgRawsockTxBufLevel;
	cb->getTXOutOfBuffers = sendmmsgRawsockGetTXOutOfBuffers;
	cb->getTXOutOfBuffersCyclic = sendmmsgRawsockGetTXOutOfBuffersCyclic;
	cb->getRxFrame = sendmmsgRawsockGetRxFrame;
	cb->rxMulticast = sendmmsgRawsockRxMulticast;
	cb->getSocket = sendmmsgRawsockGetSocket;

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
	return rawsock;
}

// Close the rawsock
void sendmmsgRawsockClose(void *pvRawsock)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;

	if (rawsock) {
		if (rawsock->sock != -1) {
			close(rawsock->sock);
			rawsock->sock = -1;
		}
	}

	baseRawsockClose(rawsock);

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
}

// Get a buffer from the ring to use for TX
U8* sendmmsgRawsockGetTxFrame(void *pvRawsock, bool blocking, unsigned int *len)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;

	if (!VALID_TX_RAWSOCK(rawsock)) {
		AVB_LOG_ERROR("Getting TX frame; bad arguments");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return NULL;
	}
	if (rawsock->buffersOut >= rawsock->frameCount) {
		rawsock->txOutOfBuffers++;
		rawsock->txOutOfBuffersCyclic++;
		AVB_LOGF_WARNING("Getting TX frame; too many TX buffers in use (out=%d ready=%d count=%d), attempting recovery",
			rawsock->buffersOut, rawsock->buffersReady, rawsock->frameCount);

		if (rawsock->buffersOut == rawsock->buffersReady && rawsock->buffersReady > 0) {
			(void)sendmmsgRawsockSend(rawsock);
		}
		else {
			// Mixed in-flight state should not persist; drop queued state to recover.
			sendmmsgRawsockResetTxState(rawsock, "buffer saturation with inconsistent in-flight state");
		}

		if (rawsock->buffersOut >= rawsock->frameCount) {
			AVB_LOG_ERROR("Getting TX frame; recovery failed, dropping this packet");
			AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
			return NULL;
		}
	}

	U8 *pBuffer = rawsock->pktbuf[rawsock->buffersOut];
	rawsock->buffersOut += 1;

	// Remind client how big the frame buffer is
	if (len)
		*len = rawsock->base.frameSize;

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return  pBuffer;
}

// Set the Firewall MARK on the socket
// The mark is used by FQTSS to identify AVTP packets in kernel.
// FQTSS creates a mark that includes the AVB class and stream index.
bool sendmmsgRawsockTxSetMark(void *pvRawsock, int mark)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	bool retval = TRUE;

	if (!VALID_TX_RAWSOCK(rawsock)) {
		AVB_LOG_ERROR("Setting TX mark; invalid argument passed");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	if (rawsock->socketMarkValid && rawsock->socketMark == mark) {
		AVB_LOGF_DEBUG("SO_MARK=%d unchanged", mark);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return TRUE;
	}

	if (setsockopt(rawsock->sock, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) < 0) {
		AVB_LOGF_ERROR("Setting TX mark; setsockopt failed: %s", strerror(errno));
		retval = FALSE;
	}
	else {
		rawsock->socketMarkValid = TRUE;
		rawsock->socketMark = mark;
		AVB_LOGF_DEBUG("SO_MARK=%d OK", mark);
		U32 classPriority = 0;
		if (sendmmsgRawsockClassMarkToPriority(mark, &classPriority)) {
			if (!sendmmsgRawsockSetPriority(rawsock, classPriority, "fwmark class")) {
				retval = FALSE;
			}
		}
		else {
			AVB_LOGF_DEBUG("SO_PRIORITY unchanged for non-SR mark=%d", mark);
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return retval;
}

// Release a TX frame without sending it
bool sendmmsgRawsockRelTxFrame(void *pvRawsock, U8 *pBuffer)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;

	if (!VALID_TX_RAWSOCK(rawsock) || !pBuffer) {
		AVB_LOG_ERROR("Releasing TX frame; invalid argument");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	if (rawsock->buffersOut <= rawsock->buffersReady) {
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	if (pBuffer != rawsock->pktbuf[rawsock->buffersOut - 1]) {
		AVB_LOGF_WARNING("Releasing TX frame out-of-order ignored (out=%d ready=%d)",
			rawsock->buffersOut, rawsock->buffersReady);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	rawsock->buffersOut--;

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return TRUE;
}

// Pre-set the ethernet header information that will be used on TX frames
bool sendmmsgRawsockTxSetHdr(void *pvRawsock, hdr_info_t *pHdr)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;

	bool ret = baseRawsockTxSetHdr(pvRawsock, pHdr);
	if (ret && pHdr->vlan) {
		if (!sendmmsgRawsockSetPriority(rawsock, pHdr->vlan_pcp, "vlan PCP")) {
			AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
			return FALSE;
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return ret;
}

// Release a TX frame, mark it ready to send
bool sendmmsgRawsockTxFrameReady(void *pvRawsock, U8 *pBuffer, unsigned int len, U64 timeNsec)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	int bufidx;
#if USE_LAUNCHTIME
	bool useLaunchTime = false;
	U64 kernelLaunchTimeNsec = timeNsec;
#endif
	U64 readyWallNs = 0;
	U64 readyTaiNs = 0;

	if (!VALID_TX_RAWSOCK(rawsock)) {
		AVB_LOG_ERROR("Marking TX frame ready; invalid argument");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	bufidx = rawsock->buffersReady;
	assert(pBuffer == rawsock->pktbuf[bufidx]);

#if USE_LAUNCHTIME
	if (rawsock->launchTimeEnabled && rawsock->launchTimeSockConfigured && !timeNsec) {
		IF_LOG_INTERVAL(1000) AVB_LOG_WARNING("launch time is configured but not passed to TxFrameReady");
	}
	if (rawsock->launchTimeEnabled && timeNsec) {
		if (!rawsock->launchTimeSockConfigured) {
			if (sendmmsgRawsockEnableLaunchTime(rawsock)) {
				rawsock->launchTimeSockConfigured = true;
			}
		}
		useLaunchTime = (rawsock->launchTimeEnabled && rawsock->launchTimeSockConfigured);
		if (useLaunchTime) {
			kernelLaunchTimeNsec = sendmmsgRawsockTranslateLaunchTime(rawsock, timeNsec);
		}
	}
	fillmsghdr(&(rawsock->mmsg[bufidx].msg_hdr), &(rawsock->miov[bufidx]), useLaunchTime,
			   rawsock->cmsgbuf[bufidx],
			   kernelLaunchTimeNsec, rawsock->pktbuf[bufidx], len);
#else
	if (timeNsec) {
		IF_LOG_INTERVAL(1000) AVB_LOG_WARNING("launch time is not enabled but was passed to TxFrameReady");
	}
	fillmsghdr(&(rawsock->mmsg[bufidx].msg_hdr), &(rawsock->miov[bufidx]), rawsock->pktbuf[bufidx], len);
#endif

	sendmmsgRawsockClearTxMeta(rawsock, bufidx);
	if (sendmmsgRawsockExtractTxMeta(rawsock->pktbuf[bufidx], len,
			&rawsock->txSubtype[bufidx],
			&rawsock->txStreamUid[bufidx],
			&rawsock->txSeq[bufidx],
			&rawsock->txSeqValid[bufidx])) {
		rawsock->txMetaValid[bufidx] = true;
		if (sendmmsgRawsockTrackCrf(rawsock->txSubtype[bufidx], rawsock->txStreamUid[bufidx], rawsock->txSeqValid[bufidx])) {
			if (rawsock->crfLastQueuedSeqValid) {
				U8 expectedSeq = (U8)(rawsock->crfLastQueuedSeq + 1U);
				if (rawsock->txSeq[bufidx] != expectedSeq) {
					sendmmsgRawsockLogCrfSeqRow(
						rawsock,
						"send_queue",
						"seq_gap",
						&rawsock->crfQueueGapLogCount,
						bufidx,
						-1,
						rawsock->buffersReady + 1,
						rawsock->txSeq[bufidx],
						expectedSeq,
						timeNsec,
						kernelLaunchTimeNsec);
				}
			}
			if (rawsock->crfLastQueuedLaunchValid) {
				S64 requestedStepNs = (timeNsec >= rawsock->crfLastQueuedRequestedLaunchNs)
					? (S64)(timeNsec - rawsock->crfLastQueuedRequestedLaunchNs)
					: -((S64)(rawsock->crfLastQueuedRequestedLaunchNs - timeNsec));
				S64 kernelStepNs = (kernelLaunchTimeNsec >= rawsock->crfLastQueuedKernelLaunchNs)
					? (S64)(kernelLaunchTimeNsec - rawsock->crfLastQueuedKernelLaunchNs)
					: -((S64)(rawsock->crfLastQueuedKernelLaunchNs - kernelLaunchTimeNsec));
				if (requestedStepNs <= 0 || kernelStepNs <= 0) {
					sendmmsgRawsockLogCrfLaunchStepRow(
						rawsock,
						"non_monotonic",
						&rawsock->crfLaunchStepLogCount,
						bufidx,
						rawsock->txSeq[bufidx],
						rawsock->crfLastQueuedSeq,
						timeNsec,
						rawsock->crfLastQueuedRequestedLaunchNs,
						kernelLaunchTimeNsec,
						rawsock->crfLastQueuedKernelLaunchNs,
						requestedStepNs,
						kernelStepNs);
				}
				else if (requestedStepNs < 1500000LL || requestedStepNs > 2500000LL ||
						kernelStepNs < 1500000LL || kernelStepNs > 2500000LL) {
					sendmmsgRawsockLogCrfLaunchStepRow(
						rawsock,
						"step_outlier",
						&rawsock->crfLaunchStepLogCount,
						bufidx,
						rawsock->txSeq[bufidx],
						rawsock->crfLastQueuedSeq,
						timeNsec,
						rawsock->crfLastQueuedRequestedLaunchNs,
						kernelLaunchTimeNsec,
						rawsock->crfLastQueuedKernelLaunchNs,
						requestedStepNs,
						kernelStepNs);
				}
			}
			rawsock->crfLastQueuedSeqValid = true;
			rawsock->crfLastQueuedSeq = rawsock->txSeq[bufidx];
			rawsock->crfLastQueuedLaunchValid = (timeNsec != 0 && kernelLaunchTimeNsec != 0);
			rawsock->crfLastQueuedRequestedLaunchNs = timeNsec;
			rawsock->crfLastQueuedKernelLaunchNs = kernelLaunchTimeNsec;
		}
	}

#if USE_LAUNCHTIME
	rawsock->txRequestedWallLaunchNs[bufidx] = timeNsec;
	rawsock->txKernelLaunchNs[bufidx] = kernelLaunchTimeNsec;
	if (rawsock->diagTimingEnabled ||
			(rawsock->txMetaValid[bufidx] &&
			 sendmmsgRawsockTrackCrf(rawsock->txSubtype[bufidx], rawsock->txStreamUid[bufidx], rawsock->txSeqValid[bufidx]))) {
		if (!CLOCK_GETTIME64(OPENAVB_CLOCK_WALLTIME, &readyWallNs)) {
			readyWallNs = 0;
		}
		if (!sendmmsgRawsockGetTaiTime(&readyTaiNs)) {
			readyTaiNs = 0;
		}
		rawsock->txReadyWallNs[bufidx] = readyWallNs;
		rawsock->txReadyTaiNs[bufidx] = readyTaiNs;
	}
#endif


	rawsock->buffersReady += 1;

	if (rawsock->buffersReady >= SENDMMSG_MAX_BURST) {
		if (sendmmsgRawsockSend(rawsock) < 0) {
			AVB_LOGF_WARNING("Auto-flush sendmmsg burst failed on %s (ready=%d max_burst=%d)",
				rawsock->base.ifInfo.name, rawsock->buffersReady, SENDMMSG_MAX_BURST);
		}
	}
	else if (rawsock->buffersReady >= rawsock->frameCount) {
		AVB_LOG_DEBUG("All TxFrame slots marked ready");
	}

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return TRUE;
}

// Send all packets that are ready (i.e. tell kernel to send them)
int sendmmsgRawsockSend(void *pvRawsock)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	int sz, bytes;
	int sendErrno = 0;
	U64 sendCallTaiNs = 0;
	U64 sendReturnTaiNs = 0;
#if USE_LAUNCHTIME
	bool hadLaunchTimeCmsg = false;
	bool haveCrfLeadMetrics = false;
#endif

	if (!VALID_TX_RAWSOCK(rawsock)) {
			AVB_LOG_ERROR("Marking TX frame ready; invalid argument");
			AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
			return -1;
	}

	if (rawsock->buffersOut != rawsock->buffersReady) {
		AVB_LOGF_ERROR("Tried to send with %d bufs out, %d bufs ready", rawsock->buffersOut, rawsock->buffersReady);
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return -1;
	}

	IF_LOG_INTERVAL(1000) AVB_LOGF_DEBUG("Send with %d of %d buffers ready", rawsock->buffersReady, rawsock->frameCount);
#if USE_LAUNCHTIME
	{
		int i;
		for (i = 0; i < rawsock->buffersReady; i++) {
			if (rawsock->mmsg[i].msg_hdr.msg_controllen > 0) {
				hadLaunchTimeCmsg = true;
				break;
			}
		}
		for (i = 0; i < rawsock->buffersReady; i++) {
			if (rawsock->txMetaValid[i] &&
					sendmmsgRawsockTrackCrf(rawsock->txSubtype[i], rawsock->txStreamUid[i], rawsock->txSeqValid[i])) {
				haveCrfLeadMetrics = true;
				break;
			}
		}
	}
#endif
	if (rawsock->diagTimingEnabled || haveCrfLeadMetrics) {
		(void)sendmmsgRawsockGetTaiTime(&sendCallTaiNs);
	}
	sz = sendmmsgRawsockSendBurst(rawsock, &sendErrno);
	if (rawsock->diagTimingEnabled || haveCrfLeadMetrics) {
		(void)sendmmsgRawsockGetTaiTime(&sendReturnTaiNs);
		if (sendReturnTaiNs < sendCallTaiNs) {
			sendReturnTaiNs = sendCallTaiNs;
		}
	}
	if (sz < 0) {
		int savedErrno = sendErrno;
#if USE_LAUNCHTIME
		sendmmsgRawsockLogBurstFailure(rawsock, savedErrno, hadLaunchTimeCmsg);
		if (rawsock->launchTimeEnabled && rawsock->launchTimeSockConfigured &&
				hadLaunchTimeCmsg && launchTimeErrnoIsUnsupported(savedErrno)) {
			int i;
			if (!rawsock->launchTimeFallbackLogged) {
				AVB_LOGF_WARNING("sendmmsg launch-time unsupported (errno=%d: %s); disabling launch-time on %s and retrying",
					savedErrno, strerror(savedErrno), rawsock->base.ifInfo.name);
				rawsock->launchTimeFallbackLogged = true;
			}
			rawsock->launchTimeEnabled = false;
			rawsock->launchTimeSockConfigured = false;
			for (i = 0; i < rawsock->buffersReady; i++) {
				fillmsghdr(&(rawsock->mmsg[i].msg_hdr), &(rawsock->miov[i]), false,
					rawsock->cmsgbuf[i], 0, rawsock->pktbuf[i], rawsock->miov[i].iov_len);
			}
			sz = sendmmsgRawsockSendBurst(rawsock, &savedErrno);
			if (sz < 0) {
				AVB_LOGF_ERROR("Call to sendmmsg failed after launch-time fallback! rc=%d errno=%d (%s)",
					sz, savedErrno, strerror(savedErrno));
				bytes = sz;
			}
			else {
				int i;
				for (i = 0, bytes = 0; i < sz; i++) {
					bytes += rawsock->mmsg[i].msg_len;
				}
				if (sz < rawsock->buffersReady) {
					AVB_LOGF_WARNING("Only sent %d of %d messages after fallback; dropping others", sz, rawsock->buffersReady);
				}
			}
		}
		else
#endif
				{
					AVB_LOGF_ERROR("Call to sendmmsg failed! rc=%d errno=%d (%s)", sz, savedErrno, strerror(savedErrno));
					bytes = sz;
				}
	} else {
		int i;
		for (i = 0, bytes = 0; i < sz; i++) {
			bytes += rawsock->mmsg[i].msg_len;
		}
		if (sz < rawsock->buffersReady) {
			AVB_LOGF_WARNING("Only sent %ld of %d messages; dropping others", sz, rawsock->buffersReady);
		}
	}

	if (sz > 0) {
		int i;
		for (i = 0; i < sz; i++) {
			if (!rawsock->txMetaValid[i] ||
					!sendmmsgRawsockTrackCrf(rawsock->txSubtype[i], rawsock->txStreamUid[i], rawsock->txSeqValid[i])) {
				continue;
			}

			if (rawsock->crfLastSentSeqValid) {
				U8 expectedSeq = (U8)(rawsock->crfLastSentSeq + 1U);
				if (rawsock->txSeq[i] != expectedSeq) {
					sendmmsgRawsockLogCrfSeqRow(
						rawsock,
						"send_flush",
						"seq_gap",
						&rawsock->crfSendGapLogCount,
						i,
						sz,
						rawsock->buffersReady,
						rawsock->txSeq[i],
						expectedSeq,
						rawsock->txRequestedWallLaunchNs[i],
						rawsock->txKernelLaunchNs[i]);
				}
			}
			rawsock->crfLastSentSeqValid = true;
			rawsock->crfLastSentSeq = rawsock->txSeq[i];
		}
	}

	if (sz >= 0 && sz < rawsock->buffersReady) {
		int i;
		for (i = sz; i < rawsock->buffersReady; i++) {
			if (!rawsock->txMetaValid[i] ||
					!sendmmsgRawsockTrackCrf(rawsock->txSubtype[i], rawsock->txStreamUid[i], rawsock->txSeqValid[i])) {
				continue;
			}

			sendmmsgRawsockLogCrfSeqRow(
				rawsock,
				"send_drop",
				"tail_drop",
				&rawsock->crfSendGapLogCount,
				i,
				sz,
				rawsock->buffersReady,
				rawsock->txSeq[i],
				rawsock->crfLastSentSeqValid ? (U8)(rawsock->crfLastSentSeq + 1U) : rawsock->txSeq[i],
				rawsock->txRequestedWallLaunchNs[i],
				rawsock->txKernelLaunchNs[i]);
		}
	}

#if USE_LAUNCHTIME
	if (rawsock->diagTimingEnabled || haveCrfLeadMetrics) {
		bool collectDiag = rawsock->diagTimingEnabled;
		U64 submitLatencyNs = (sendReturnTaiNs >= sendCallTaiNs) ? (sendReturnTaiNs - sendCallTaiNs) : 0ULL;
		int i;
		if (collectDiag) {
			sendmmsgRawsockDiagUpdateUnsigned(
				&rawsock->diagSubmitLatencyMinNs,
				&rawsock->diagSubmitLatencyMaxNs,
				&rawsock->diagSubmitLatencySumNs,
				rawsock->diagBurstCount,
				submitLatencyNs);
			rawsock->diagBurstCount++;
		}

		for (i = 0; i < rawsock->buffersReady; i++) {
			U64 readyTaiNs = rawsock->txReadyTaiNs[i];
			U64 readyWallNs = rawsock->txReadyWallNs[i];
			U64 requestedWallLaunchNs = rawsock->txRequestedWallLaunchNs[i];
			U64 kernelLaunchNs = rawsock->txKernelLaunchNs[i];
			U64 queueBeforeSendNs = (sendCallTaiNs >= readyTaiNs) ? (sendCallTaiNs - readyTaiNs) : 0ULL;
			bool isCrfPacket = rawsock->txMetaValid[i] &&
				sendmmsgRawsockTrackCrf(rawsock->txSubtype[i], rawsock->txStreamUid[i], rawsock->txSeqValid[i]);

			if (collectDiag) {
				sendmmsgRawsockDiagUpdateUnsigned(
					&rawsock->diagQueueBeforeSendMinNs,
					&rawsock->diagQueueBeforeSendMaxNs,
					&rawsock->diagQueueBeforeSendSumNs,
					rawsock->diagPacketCount,
					queueBeforeSendNs);
				rawsock->diagPacketCount++;
			}

			if (kernelLaunchNs == 0 || readyTaiNs == 0 || readyWallNs == 0) {
				if (isCrfPacket) {
					rawsock->crfDiagPacketCount++;
					if ((U32)rawsock->buffersReady > rawsock->crfDiagBurstReadyMax) {
						rawsock->crfDiagBurstReadyMax = (U32)rawsock->buffersReady;
					}
					rawsock->crfDiagNoLaunchCount++;
				}
				if (collectDiag) {
					rawsock->diagNoLaunchTimeCount++;
					rawsock->diagOutlierRowCount++;
					if (rawsock->diagOutlierRowCount <= 16ULL ||
						((rawsock->diagOutlierRowCount % 256ULL) == 0ULL)) {
						AVB_LOGF_WARNING(
							"TX OUTLIER ROW flags=..Z stage=send stream=all requested_launch=%" PRIu64 " kernel_launch=%" PRIu64 " ready_wall=%" PRIu64 " ready_tai=%" PRIu64 " send_call=%" PRIu64 " send_return=%" PRIu64 " lead_ready_wall=na lead_ready_kernel=na lead_send_call=na lead_send_return=na no_launch=1 rows=%" PRIu64,
							requestedWallLaunchNs,
							kernelLaunchNs,
							readyWallNs,
							readyTaiNs,
							sendCallTaiNs,
							sendReturnTaiNs,
							rawsock->diagOutlierRowCount);
					}
				}
				if (isCrfPacket) {
					sendmmsgRawsockLogCrfLeadRow(
						rawsock,
						"send_margin",
						"no_launch",
						&rawsock->crfNoLaunchLogCount,
						i,
						rawsock->txSeq[i],
						requestedWallLaunchNs,
						kernelLaunchNs,
						readyWallNs,
						readyTaiNs,
						sendCallTaiNs,
						sendReturnTaiNs,
						0,
						0,
						0,
						0,
						rawsock->crfMinLeadWarnNs);
					sendmmsgRawsockCrfDiagMaybeLog(rawsock);
				}
				continue;
			}

			{
				S64 readyWallLeadNs = (requestedWallLaunchNs >= readyWallNs)
					? (S64)(requestedWallLaunchNs - readyWallNs)
					: -((S64)(readyWallNs - requestedWallLaunchNs));
				S64 readyKernelLeadNs = (kernelLaunchNs >= readyTaiNs)
					? (S64)(kernelLaunchNs - readyTaiNs)
					: -((S64)(readyTaiNs - kernelLaunchNs));
				S64 sendCallLeadNs = (kernelLaunchNs >= sendCallTaiNs)
					? (S64)(kernelLaunchNs - sendCallTaiNs)
					: -((S64)(sendCallTaiNs - kernelLaunchNs));
				S64 sendReturnLeadNs = (kernelLaunchNs >= sendReturnTaiNs)
					? (S64)(kernelLaunchNs - sendReturnTaiNs)
					: -((S64)(sendReturnTaiNs - kernelLaunchNs));

				if (collectDiag) {
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->diagReadyWallLeadMinNs,
						&rawsock->diagReadyWallLeadMaxNs,
						&rawsock->diagReadyWallLeadSumNs,
						rawsock->diagLaunchMetricPacketCount,
						readyWallLeadNs);
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->diagReadyKernelLeadMinNs,
						&rawsock->diagReadyKernelLeadMaxNs,
						&rawsock->diagReadyKernelLeadSumNs,
						rawsock->diagLaunchMetricPacketCount,
						readyKernelLeadNs);
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->diagSendCallLeadMinNs,
						&rawsock->diagSendCallLeadMaxNs,
						&rawsock->diagSendCallLeadSumNs,
						rawsock->diagLaunchMetricPacketCount,
						sendCallLeadNs);
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->diagSendReturnLeadMinNs,
						&rawsock->diagSendReturnLeadMaxNs,
						&rawsock->diagSendReturnLeadSumNs,
						rawsock->diagLaunchMetricPacketCount,
						sendReturnLeadNs);
					rawsock->diagLaunchMetricPacketCount++;

					if (readyKernelLeadNs < 0) {
						rawsock->diagLateAtReadyCount++;
					}
					if (sendCallLeadNs < 0) {
						rawsock->diagLateAtSendCallCount++;
					}
					if (sendReturnLeadNs < 0) {
						rawsock->diagLateAtSendReturnCount++;
					}
				}

				if (isCrfPacket) {
					rawsock->crfDiagPacketCount++;
					if ((U32)rawsock->buffersReady > rawsock->crfDiagBurstReadyMax) {
						rawsock->crfDiagBurstReadyMax = (U32)rawsock->buffersReady;
					}
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->crfDiagReadyKernelLeadMinNs,
						&rawsock->crfDiagReadyKernelLeadMaxNs,
						&rawsock->crfDiagReadyKernelLeadSumNs,
						rawsock->crfDiagLeadMetricCount,
						readyKernelLeadNs);
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->crfDiagSendCallLeadMinNs,
						&rawsock->crfDiagSendCallLeadMaxNs,
						&rawsock->crfDiagSendCallLeadSumNs,
						rawsock->crfDiagLeadMetricCount,
						sendCallLeadNs);
					sendmmsgRawsockDiagUpdateSigned(
						&rawsock->crfDiagSendReturnLeadMinNs,
						&rawsock->crfDiagSendReturnLeadMaxNs,
						&rawsock->crfDiagSendReturnLeadSumNs,
						rawsock->crfDiagLeadMetricCount,
						sendReturnLeadNs);
					rawsock->crfDiagLeadMetricCount++;
				}

				if (isCrfPacket &&
						(rawsock->crfMinLeadWarnNs > 0) &&
						((readyKernelLeadNs >= 0 && (U64)readyKernelLeadNs < rawsock->crfMinLeadWarnNs) ||
						 (sendCallLeadNs >= 0 && (U64)sendCallLeadNs < rawsock->crfMinLeadWarnNs) ||
						 (sendReturnLeadNs >= 0 && (U64)sendReturnLeadNs < rawsock->crfMinLeadWarnNs))) {
					rawsock->crfDiagLowLeadCount++;
					sendmmsgRawsockLogCrfLeadRow(
						rawsock,
						"send_margin",
						"low_lead",
						&rawsock->crfLowLeadLogCount,
						i,
						rawsock->txSeq[i],
						requestedWallLaunchNs,
						kernelLaunchNs,
						readyWallNs,
						readyTaiNs,
						sendCallTaiNs,
						sendReturnTaiNs,
						readyWallLeadNs,
						readyKernelLeadNs,
						sendCallLeadNs,
						sendReturnLeadNs,
						rawsock->crfMinLeadWarnNs);
				}

				if (isCrfPacket &&
						(readyKernelLeadNs < 0 || sendCallLeadNs < 0 || sendReturnLeadNs < 0)) {
					rawsock->crfDiagLateCount++;
					sendmmsgRawsockLogCrfLeadRow(
						rawsock,
						"send",
						"late",
						&rawsock->crfLateLeadLogCount,
						i,
						rawsock->txSeq[i],
						requestedWallLaunchNs,
						kernelLaunchNs,
						readyWallNs,
						readyTaiNs,
						sendCallTaiNs,
						sendReturnTaiNs,
						readyWallLeadNs,
						readyKernelLeadNs,
						sendCallLeadNs,
						sendReturnLeadNs,
						rawsock->crfMinLeadWarnNs);
				}
				if (isCrfPacket) {
					sendmmsgRawsockCrfDiagMaybeLog(rawsock);
				}

				if (collectDiag && (readyKernelLeadNs < 0 || sendCallLeadNs < 0 || sendReturnLeadNs < 0)) {
					rawsock->diagOutlierRowCount++;
					if (rawsock->diagOutlierRowCount <= 16ULL ||
						((rawsock->diagOutlierRowCount % 256ULL) == 0ULL)) {
						AVB_LOGF_WARNING(
							"TX OUTLIER ROW flags=..Z stage=send stream=all requested_launch=%" PRIu64 " kernel_launch=%" PRIu64 " ready_wall=%" PRIu64 " ready_tai=%" PRIu64 " send_call=%" PRIu64 " send_return=%" PRIu64 " lead_ready_wall=%" PRId64 " lead_ready_kernel=%" PRId64 " lead_send_call=%" PRId64 " lead_send_return=%" PRId64 " no_launch=0 rows=%" PRIu64,
							requestedWallLaunchNs,
							kernelLaunchNs,
							readyWallNs,
							readyTaiNs,
							sendCallTaiNs,
							sendReturnTaiNs,
							readyWallLeadNs,
							readyKernelLeadNs,
							sendCallLeadNs,
							sendReturnLeadNs,
							rawsock->diagOutlierRowCount);
					}
				}
			}
		}

	if (collectDiag) {
			sendmmsgRawsockDiagMaybeLog(rawsock);
		}
	}
#endif

#if USE_LAUNCHTIME
	sendmmsgRawsockDrainErrqueue(rawsock);
#endif

#if USE_LAUNCHTIME
	{
		int i;
		for (i = 0; i < rawsock->buffersReady; i++) {
			sendmmsgRawsockClearTxMeta(rawsock, i);
			rawsock->txReadyWallNs[i] = 0;
			rawsock->txReadyTaiNs[i] = 0;
			rawsock->txRequestedWallLaunchNs[i] = 0;
			rawsock->txKernelLaunchNs[i] = 0;
		}
	}
#else
	{
		int i;
		for (i = 0; i < rawsock->buffersReady; i++) {
			sendmmsgRawsockClearTxMeta(rawsock, i);
		}
	}
#endif

	rawsock->buffersOut = rawsock->buffersReady = 0;

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return bytes;
}

int sendmmsgRawsockTxBufLevel(void *pvRawsock)
{
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	if (!rawsock) {
		return -1;
	}
	return rawsock->frameCount - rawsock->buffersOut;
}

// Get a RX frame
U8* sendmmsgRawsockGetRxFrame(void *pvRawsock, U32 timeout, unsigned int *offset, unsigned int *len)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	if (!VALID_RX_RAWSOCK(rawsock)) {
		AVB_LOG_ERROR("Getting RX frame; invalid arguments");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return NULL;
	}
// For switching to recvmmsg eventually
//	if (rawsock->rxbuffersOut >= rawsock->rxframeCount) {
//		AVB_LOG_ERROR("Too many RX buffers in use");
//		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
//		return NULL;
//	}

	int flags = 0;

	U8 *pBuffer = rawsock->rxBuffer;
	*offset = 0;
	*len = recv(rawsock->sock, pBuffer, rawsock->base.frameSize, flags);

	if (*len == (unsigned int)(-1)) {
		AVB_LOGF_ERROR("%s %s", __func__, strerror(errno));
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return NULL;
	}

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return pBuffer;
}

// Setup the rawsock to receive multicast packets
bool sendmmsgRawsockRxMulticast(void *pvRawsock, bool add_membership, const U8 addr[ETH_ALEN])
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK_DETAIL);

	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	if (!VALID_RX_RAWSOCK(rawsock)) {
		AVB_LOG_ERROR("Setting multicast; invalid arguments");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	struct ether_addr mcast_addr;
	memcpy(mcast_addr.ether_addr_octet, addr, ETH_ALEN);

	// Fill in the structure for the multicast ioctl
	struct packet_mreq mreq;
	memset(&mreq, 0, sizeof(struct packet_mreq));
	mreq.mr_ifindex = rawsock->base.ifInfo.index;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = ETH_ALEN;
	memcpy(&mreq.mr_address, &mcast_addr.ether_addr_octet, ETH_ALEN);

	// And call the ioctl to add/drop the multicast address
	int action = (add_membership ? PACKET_ADD_MEMBERSHIP : PACKET_DROP_MEMBERSHIP);
	if (setsockopt(rawsock->sock, SOL_PACKET, action,
					(void*)&mreq, sizeof(struct packet_mreq)) < 0) {
		AVB_LOGF_ERROR("Setting multicast; setsockopt(%s) failed: %s",
					   (add_membership ? "PACKET_ADD_MEMBERSHIP" : "PACKET_DROP_MEMBERSHIP"),
					   strerror(errno));

		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
		return FALSE;
	}

	// In addition to adding the multicast membership, we also want to
	//	add a packet filter to restrict the packets that we'll receive
	//	on this socket.  Multicast memeberships are global - not
	//	per-socket, so without the filter, this socket would receieve
	//	packets for all the multicast addresses added by all other
	//	sockets.
	//
	if (add_membership)
	{
		// Here's the template packet filter code.
		// It was produced by running:
		//   tcpdump -dd ether dest host 91:e0:01:02:03:04
		struct sock_filter bpfCode[] = {
			{ 0x20, 0, 0, 0x00000002 },
			{ 0x15, 0, 3, 0x01020304 },   // last 4 bytes of dest mac
			{ 0x28, 0, 0, 0x00000000 },
			{ 0x15, 0, 1, 0x000091e0 },   // first 2 bytes of dest mac
			{ 0x06, 0, 0, 0x0000ffff },
			{ 0x06, 0, 0, 0x00000000 }
		};

		// We need to graft our multicast dest address into bpfCode
		U32 tmp; U8 *buf = (U8*)&tmp;
		memcpy(buf, mcast_addr.ether_addr_octet + 2, 4);
		bpfCode[1].k = ntohl(tmp);
		memset(buf, 0, 4);
		memcpy(buf + 2, mcast_addr.ether_addr_octet, 2);
		bpfCode[3].k = ntohl(tmp);

		// Now wrap the filter code in the appropriate structure
		struct sock_fprog filter;
		memset(&filter, 0, sizeof(filter));
		filter.len = 6;
		filter.filter = bpfCode;

		// And attach it to our socket
		if (setsockopt(rawsock->sock, SOL_SOCKET, SO_ATTACH_FILTER,
						&filter, sizeof(filter)) < 0) {
			AVB_LOGF_ERROR("Setting multicast; setsockopt(SO_ATTACH_FILTER) failed: %s", strerror(errno));
		}
	}
	else {
		if (setsockopt(rawsock->sock, SOL_SOCKET, SO_DETACH_FILTER, NULL, 0) < 0) {
			AVB_LOGF_ERROR("Setting multicast; setsockopt(SO_DETACH_FILTER) failed: %s", strerror(errno));
		}
	}

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK_DETAIL);
	return TRUE;
}

unsigned long sendmmsgRawsockGetTXOutOfBuffers(void *pvRawsock)
{
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	if (!rawsock) {
		return 0;
	}
	return rawsock->txOutOfBuffers;
}

unsigned long sendmmsgRawsockGetTXOutOfBuffersCyclic(void *pvRawsock)
{
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	unsigned long ret;
	if (!rawsock) {
		return 0;
	}
	ret = rawsock->txOutOfBuffersCyclic;
	rawsock->txOutOfBuffersCyclic = 0;
	return ret;
}

// Get the socket used for this rawsock; can be used for poll/select
int  sendmmsgRawsockGetSocket(void *pvRawsock)
{
	AVB_TRACE_ENTRY(AVB_TRACE_RAWSOCK);
	sendmmsg_rawsock_t *rawsock = (sendmmsg_rawsock_t*)pvRawsock;
	if (!rawsock) {
		AVB_LOG_ERROR("Getting socket; invalid arguments");
		AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
		return -1;
	}

	AVB_TRACE_EXIT(AVB_TRACE_RAWSOCK);
	return rawsock->sock;
}
