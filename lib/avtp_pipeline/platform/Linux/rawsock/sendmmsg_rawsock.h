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

#ifndef SENDMMSG_RAWSOCK_H
#define SENDMMSG_RAWSOCK_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/socket.h>

#include "rawsock_impl.h"

#define MSG_COUNT 8
#define SENDMMSG_MAX_BURST MSG_COUNT
#define MAX_FRAME_SIZE 1024
#define USE_LAUNCHTIME 1
#define SENDMMSG_ENOBUFS_RETRIES 8
#define SENDMMSG_ENOBUFS_SLEEP_USEC 250
#define SENDMMSG_TX_SNDBUF_BYTES (1 << 20)


// State information for raw socket
//
typedef struct {
	base_rawsock_t base;

	// the underlying socket
	int sock;

	// count of total buffers available for messages
	int frameCount;

	// count of buffers taken by senders
	int buffersOut;

	// count of buffers ready to send
	int buffersReady;

	// buffer for receiving frames
	U8 rxBuffer[1518];

	struct mmsghdr mmsg[MSG_COUNT];

	struct iovec miov[MSG_COUNT];

	unsigned char pktbuf[MSG_COUNT][MAX_FRAME_SIZE];
	bool txMetaValid[MSG_COUNT];
	U8 txSubtype[MSG_COUNT];
	U16 txStreamUid[MSG_COUNT];
	U8 txSeq[MSG_COUNT];
	bool txSeqValid[MSG_COUNT];
	unsigned long txOutOfBuffers;
	unsigned long txOutOfBuffersCyclic;
	U32 enobufsRetries;
	U32 enobufsSleepUsec;
	U32 txSndbufBytes;
	bool socketPriorityValid;
	U32 socketPriority;
	bool socketMarkValid;
	int socketMark;
#if USE_LAUNCHTIME
	unsigned char cmsgbuf[MSG_COUNT][CMSG_SPACE(sizeof(uint64_t))];
	bool launchTimeEnabled;
	bool launchTimeSockConfigured;
	bool launchTimeFallbackLogged;
	bool launchTimeClockOffsetValid;
	S64 launchTimeWallToTaiOffsetNs;
	U64 launchTimeOffsetLastUpdateMonoNs;
	U32 launchTimeOffsetLogCount;
	bool diagTimingEnabled;
	U32 diagTimingLogInterval;
	U64 txReadyWallNs[MSG_COUNT];
	U64 txReadyTaiNs[MSG_COUNT];
	U64 txRequestedWallLaunchNs[MSG_COUNT];
	U64 txKernelLaunchNs[MSG_COUNT];
	U64 diagBurstCount;
	U64 diagPacketCount;
	U64 diagLaunchMetricPacketCount;
	U64 diagMinLeadWarnNs;
	U64 crfMinLeadWarnNs;
	U64 diagNoLaunchTimeCount;
	U64 diagErrqueueEvents;
	U64 diagErrqueueMissed;
	U64 diagLateAtReadyCount;
	U64 diagLateAtSendCallCount;
	U64 diagLateAtSendReturnCount;
	U64 diagOutlierRowCount;
	U64 crfDiagPacketCount;
	U64 crfDiagLeadMetricCount;
	S64 crfDiagReadyKernelLeadMinNs;
	S64 crfDiagReadyKernelLeadMaxNs;
	S64 crfDiagReadyKernelLeadSumNs;
	S64 crfDiagSendCallLeadMinNs;
	S64 crfDiagSendCallLeadMaxNs;
	S64 crfDiagSendCallLeadSumNs;
	S64 crfDiagSendReturnLeadMinNs;
	S64 crfDiagSendReturnLeadMaxNs;
	S64 crfDiagSendReturnLeadSumNs;
	U64 crfDiagLowLeadCount;
	U64 crfDiagLateCount;
	U64 crfDiagNoLaunchCount;
	U32 crfDiagBurstReadyMax;
	U64 diagQueueBeforeSendMinNs;
	U64 diagQueueBeforeSendMaxNs;
	U64 diagQueueBeforeSendSumNs;
	U64 diagSubmitLatencyMinNs;
	U64 diagSubmitLatencyMaxNs;
	U64 diagSubmitLatencySumNs;
	S64 diagReadyWallLeadMinNs;
	S64 diagReadyWallLeadMaxNs;
	S64 diagReadyWallLeadSumNs;
	S64 diagReadyKernelLeadMinNs;
	S64 diagReadyKernelLeadMaxNs;
	S64 diagReadyKernelLeadSumNs;
	S64 diagSendCallLeadMinNs;
	S64 diagSendCallLeadMaxNs;
	S64 diagSendCallLeadSumNs;
	S64 diagSendReturnLeadMinNs;
	S64 diagSendReturnLeadMaxNs;
	S64 diagSendReturnLeadSumNs;
	bool crfLastQueuedSeqValid;
	U8 crfLastQueuedSeq;
	bool crfLastQueuedLaunchValid;
	U64 crfLastQueuedRequestedLaunchNs;
	U64 crfLastQueuedKernelLaunchNs;
	bool crfLastSentSeqValid;
	U8 crfLastSentSeq;
	U32 crfErrqueueLogCount;
	U32 crfLowLeadLogCount;
	U32 crfNoLaunchLogCount;
	U32 crfLateLeadLogCount;
	U32 crfQueueGapLogCount;
	U32 crfLaunchStepLogCount;
	U32 crfSendGapLogCount;
#endif
} sendmmsg_rawsock_t;

// Open a rawsock for TX or RX
void* sendmmsgRawsockOpen(sendmmsg_rawsock_t *rawsock, const char *ifname, bool rx_mode, bool tx_mode, U16 ethertype, U32 frame_size, U32 num_frames);

// Close the rawsock
void sendmmsgRawsockClose(void *pvRawsock);

// Get a buffer from the simple to use for TX
U8* sendmmsgRawsockGetTxFrame(void *pvRawsock, bool blocking, unsigned int *len);

// Set the Firewall MARK on the socket
// The mark is used by FQTSS to identify AVTP packets in kernel.
// FQTSS creates a mark that includes the AVB class and stream index.
bool sendmmsgRawsockTxSetMark(void *pvRawsock, int mark);

// Release a TX frame without sending it
bool sendmmsgRawsockRelTxFrame(void *pvRawsock, U8 *pBuffer);

// Pre-set the ethernet header information that will be used on TX frames
bool sendmmsgRawsockTxSetHdr(void *pvRawsock, hdr_info_t *pHdr);

// Release a TX frame, and mark it as ready to send
bool sendmmsgRawsockTxFrameReady(void *pvRawsock, U8 *pBuffer, unsigned int len, U64 timeNsec);

// Send all packets that are ready (i.e. tell kernel to send them)
int sendmmsgRawsockSend(void *pvRawsock);

// Check Tx buffer level in sockets
int sendmmsgRawsockTxBufLevel(void *pvRawsock);

// Get a RX frame
U8* sendmmsgRawsockGetRxFrame(void *pvRawsock, U32 timeout, unsigned int *offset, unsigned int *len);

// Setup the rawsock to receive multicast packets
bool sendmmsgRawsockRxMulticast(void *pvRawsock, bool add_membership, const U8 addr[ETH_ALEN]);

// Get TX out-of-buffer event counters
unsigned long sendmmsgRawsockGetTXOutOfBuffers(void *pvRawsock);
unsigned long sendmmsgRawsockGetTXOutOfBuffersCyclic(void *pvRawsock);

// Get the socket used for this rawsock; can be used for poll/select
int  sendmmsgRawsockGetSocket(void *pvRawsock);

#endif
