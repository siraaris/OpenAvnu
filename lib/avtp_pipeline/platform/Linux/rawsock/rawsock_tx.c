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
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <glib.h>
#include <arpa/inet.h>
#include "./openavb_rawsock.h"
#include "openavb_log.h"

//Common usage: ./rawsock_tx -i eth0 -t 8944 -r 8000 -s 1 -c 1 -m 1 -l 100

#define MAX_NUM_FRAMES 100
#define NANOSECONDS_PER_SECOND		(1000000000ULL)
#define TIMESPEC_TO_NSEC(ts) (((uint64_t)ts.tv_sec * (uint64_t)NANOSECONDS_PER_SECOND) + (uint64_t)ts.tv_nsec)

#define RAWSOCK_TX_MODE_FILL (0)
#define RAWSOCK_TX_MODE_SEQ  (1)

static char* interface = NULL;
static int ethertype = -1;
static int txlen = 64;
static int txRate = 8000;
static int chunkSize = 1;
static int reportSec = 1;
static int mode = RAWSOCK_TX_MODE_FILL;
static char *dst = NULL;
static char *src = NULL;
static int vlan = -1;
static int pcp = 3;
static int mark = -1;

static U8 gDstMac[ETH_ALEN];
static U8 gSrcMac[ETH_ALEN];
static bool gHaveDst = FALSE;
static bool gHaveSrc = FALSE;

static GOptionEntry entries[] =
{
  { "interface", 'i', 0, G_OPTION_ARG_STRING, &interface, "network interface",                        "NAME" },
  { "ethertype", 't', 0, G_OPTION_ARG_INT,    &ethertype, "ethernet protocol",                        "NUM" },
  { "length",    'l', 0, G_OPTION_ARG_INT,    &txlen,     "frame length",                             "LEN" },
  { "rate",      'r', 0, G_OPTION_ARG_INT,    &txRate,    "tx rate",                                  "RATE" },
  { "chunk",     'c', 0, G_OPTION_ARG_INT,    &chunkSize, "Chunk size",                               "CHUNKSIZE" },
  { "rptsec",    's', 0, G_OPTION_ARG_INT,    &reportSec, "report interval in seconds",               "RPTSEC" },
  { "mode",      'm', 0, G_OPTION_ARG_INT,    &mode,      "mode: 0 = fill, 1 = sequence number",      "MODE" },
  { "dst",       'd', 0, G_OPTION_ARG_STRING, &dst,       "destination MAC (aa:bb:cc:dd:ee:ff)",      "MAC" },
  { "src",       'S', 0, G_OPTION_ARG_STRING, &src,       "source MAC (aa:bb:cc:dd:ee:ff)",           "MAC" },
  { "vlan",      'v', 0, G_OPTION_ARG_INT,    &vlan,      "VLAN ID (0-4094), -1 disables tagging",     "VID" },
  { "pcp",       'p', 0, G_OPTION_ARG_INT,    &pcp,       "VLAN PCP (0-7)",                            "PCP" },
  { "mark",      'k', 0, G_OPTION_ARG_INT,    &mark,      "fwmark value for queue selection",          "MARK" },
  { NULL }
};

static bool parseMac(const char *txt, U8 out[ETH_ALEN])
{
	unsigned int v[ETH_ALEN];
	if (!txt) {
		return FALSE;
	}
	if (sscanf(txt, "%02x:%02x:%02x:%02x:%02x:%02x",
			&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != ETH_ALEN) {
		return FALSE;
	}
	for (int i = 0; i < ETH_ALEN; i++) {
		out[i] = (U8)v[i];
	}
	return TRUE;
}

void dumpAscii(U8 *pFrame, int i, int *j)
{
	char c;

	printf("  ");
	
	while (*j <= i) {
		c = pFrame[*j];
		*j += 1;
		if (!isprint(c) || isspace(c))
			c = '.';
		printf("%c", c);
	}
}

void dumpFrameContent(U8 *pFrame, U32 len)
{
	int i = 0, j = 0;
	while (TRUE) {
		if (i % 16 == 0) {
			if (i != 0 ) {
				// end of line stuff
				dumpAscii(pFrame, (i < len ? i : len), &j);
				printf("\n");

				if (i >= len)
					break;
			}
			if (i+1 < len) {
				// start of line stuff
				printf("0x%4.4d:  ", i);
			}
		}
		else if (i % 2 == 0) {
			printf("  ");
		}

		if (i >= len)
			printf("  ");
		else
			printf("%2.2x", pFrame[i]);

		i += 1;
	}
}

void dumpFrame(U8 *pFrame, U32 len, hdr_info_t *hdr)
{
	printf("Frame received, ethertype=0x%x len=%u\n", hdr->ethertype, len);
	printf("src: %s\n", ether_ntoa((const struct ether_addr*)hdr->shost));
	printf("dst: %s\n", ether_ntoa((const struct ether_addr*)hdr->dhost));
	if (hdr->vlan) {
		printf("VLAN pcp=%u, vid=%u\n", (unsigned)hdr->vlan_pcp, hdr->vlan_vid); 
	}
	dumpFrameContent(pFrame, len);
	printf("\n");
}

int main(int argc, char* argv[])
{
	GError *error = NULL;
	GOptionContext *context;

	context = g_option_context_new("- rawsock listenr");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		printf("error: %s\n", error->message);
		exit(1);
	}

	if (interface == NULL || ethertype < 0) {
		printf("error: must specify network interface and ethertype\n");
		exit(2);
	}
	if (vlan > 4094 || vlan < -1) {
		printf("error: invalid VLAN ID %d\n", vlan);
		exit(2);
	}
	if (pcp < 0 || pcp > 7) {
		printf("error: invalid PCP %d\n", pcp);
		exit(2);
	}
	if (dst && !parseMac(dst, gDstMac)) {
		printf("error: invalid destination MAC '%s'\n", dst);
		exit(2);
	}
	if (src && !parseMac(src, gSrcMac)) {
		printf("error: invalid source MAC '%s'\n", src);
		exit(2);
	}
	gHaveDst = (dst != NULL);
	gHaveSrc = (src != NULL);

	void* rs = openavbRawsockOpen(interface, FALSE, TRUE, ethertype, 0, MAX_NUM_FRAMES);
	if (!rs) {
		printf("error: failed to open raw socket (are you root?)\n");
		exit(3);
	}

	U8 *pBuf, *pData;
	U32 buflen, hdrlen, datalen;
	hdr_info_t hdr;

	avbLogInit();

	memset(&hdr, 0, sizeof(hdr_info_t));
	hdr.ethertype = ethertype;
	if (gHaveDst) {
		hdr.dhost = gDstMac;
	}
	if (gHaveSrc) {
		hdr.shost = gSrcMac;
	}
	if (vlan >= 0) {
		hdr.vlan = TRUE;
		hdr.vlan_vid = vlan;
		hdr.vlan_pcp = (U8)pcp;
	}
	openavbRawsockTxSetHdr(rs, &hdr);
	if (mark >= 0) {
		if (!openavbRawsockTxSetMark(rs, mark)) {
			printf("warning: openavbRawsockTxSetMark(%d) failed\n", mark);
		}
	}
	printf("TX config: if=%s ethertype=0x%x vlan=%d pcp=%d mark=%d rate=%d len=%d\n",
		interface, ethertype, vlan, pcp, mark, txRate, txlen);

	struct timespec now;
	static U64 packetIntervalNSec = 0;
	static U64 nextCycleNSec = 0;
	static U32 packetCnt = 0;
	static U64 nextReportInterval = 0;

	packetIntervalNSec = NANOSECONDS_PER_SECOND / txRate;
	clock_gettime(CLOCK_MONOTONIC, &now);
	nextCycleNSec = TIMESPEC_TO_NSEC(now);
	nextReportInterval = TIMESPEC_TO_NSEC(now) + (NANOSECONDS_PER_SECOND * reportSec);

	while (1) {
		int remainder = 0;
		pBuf = (U8*)openavbRawsockGetTxFrame(rs, TRUE, &buflen);
		if (!pBuf) {
			printf("failed to get TX frame buffer\n");
			exit(4);
		}

		if (buflen < txlen || txlen == -1)
			txlen = buflen;

		openavbRawsockTxFillHdr(rs, pBuf, &hdrlen);
		pData = pBuf + hdrlen;
		datalen = txlen - hdrlen;

		if (mode == RAWSOCK_TX_MODE_FILL) {
			int i;
			for (i=0; i<datalen; i++)
				pData[i] = (i & 0xff);
		}
		else {	// RAWSOCK_TX_MODE_sEQ
			static unsigned char seq = 0;
			pData[0] = 0x7F;		// Experimental subtype
			pData[1] = 0x00;
			pData[2] = seq++;
			txlen = hdrlen + 3;
		}

		openavbRawsockTxFrameReady(rs, pBuf, txlen, 0);

		packetCnt++;

		if ((packetCnt % chunkSize) == 0) {
			openavbRawsockSend(rs);
		}

		nextCycleNSec += packetIntervalNSec;
		clock_gettime(CLOCK_MONOTONIC, &now);
		U64 nowNSec = TIMESPEC_TO_NSEC(now);;

		if (nowNSec > nextReportInterval) {
			printf("TX Packets: %d\n", packetCnt-remainder);
			packetCnt %= chunkSize;
			remainder = packetCnt;
			nextReportInterval = nowNSec + (NANOSECONDS_PER_SECOND * reportSec);
		}

		if (nowNSec < nextCycleNSec) {
			usleep((nextCycleNSec - nowNSec) / 1000);
		}
	}

	openavbRawsockClose(rs);
	avbLogExit();
	return 0;
}
