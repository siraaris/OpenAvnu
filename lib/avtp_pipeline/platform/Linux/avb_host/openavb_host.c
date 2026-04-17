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
* MODULE SUMMARY : Talker listener test host implementation.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "openavb_tl_pub.h"
#include "openavb_plugin.h"
#include "openavb_trace_pub.h"
#include "openavb_clock_source_runtime_pub.h"
#include "openavb_aem_types_pub.h"
#ifdef AVB_FEATURE_GSTREAMER
#include <gst/gst.h>
#endif

#define	AVB_LOG_COMPONENT	"TL Host"
#include "openavb_log_pub.h"

bool bRunning = TRUE;

typedef struct {
	bool pending;
	U32 selectionGeneration;
	U32 sourceGeneration;
	U64 stableSinceNs;
} deferred_start_state_t;

static bool deferredStartSelectedClockReady(U32 *pSelectionGeneration, U32 *pSourceGeneration)
{
	openavb_clock_source_runtime_t selection;
	U64 mediaClockNs = 0;
	bool uncertain = FALSE;
	U32 sourceGeneration = 0;

	if (!openavbClockSourceRuntimeGetSelection(&selection)) {
		return FALSE;
	}

	if (!(selection.clock_source_flags & OPENAVB_AEM_CLOCK_SOURCE_FLAG_STREAM_ID)) {
		return FALSE;
	}

	if (!openavbClockSourceRuntimeGetMediaClockForLocation(
			selection.clock_source_location_type,
			selection.clock_source_location_index,
			&mediaClockNs,
			&uncertain,
			&sourceGeneration)) {
		return FALSE;
	}

	if (uncertain) {
		return FALSE;
	}

	if (pSelectionGeneration) {
		*pSelectionGeneration = selection.generation;
	}
	if (pSourceGeneration) {
		*pSourceGeneration = sourceGeneration;
	}
	return TRUE;
}

// Platform independent mapping modules
extern bool openavbMapPipeInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapAVTPAudioInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapCtrlInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapCrfInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapH264Initialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapMjpegInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapMpeg2tsInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapNullInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);
extern bool openavbMapUncmpAudioInitialize(media_q_t *pMediaQ, openavb_map_cb_t *pMapCB, U32 inMaxTransitUsec);

// Platform independent interface modules
extern bool openavbIntfEchoInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfCtrlInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfLoggerInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfNullInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfToneGenInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfViewerInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);

// Linux interface modules
extern bool openavbIntfAlsaInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfAvb32DirectInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfBus32SplitInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfMpeg2tsFileInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfWavFileInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
#ifdef AVB_FEATURE_GSTREAMER
extern bool openavbIntfMjpegGstInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfMpeg2tsGstInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
extern bool openavbIntfH264RtpGstInitialize(media_q_t *pMediaQ, openavb_intf_cb_t *pIntfCB);
#endif

/***********************************************
 * Signal handler - used to respond to signals.
 * Allows graceful cleanup.
 */
static void openavbTLSigHandler(int signal)
{
	AVB_TRACE_ENTRY(AVB_TRACE_HOST);

	if (signal == SIGINT || signal == SIGTERM) {
		if (bRunning) {
			AVB_LOG_INFO("Host shutting down");
			bRunning = FALSE;
		}
		else {
			// Force shutdown
			exit(2);
		}
	}
	else if (signal == SIGUSR1) {
		AVB_LOG_DEBUG("Waking up streaming thread");
	}
	else {
		AVB_LOG_ERROR("Unexpected signal");
	}

	AVB_TRACE_EXIT(AVB_TRACE_HOST);
}

void openavbTlHostUsage(char *programName)
{
	printf(
		"\n"
		"Usage: %s [options] file...\n"
		"  -I val     Use given (val) interface globally, can be overriden by giving the ifname= option to the config line.\n"
		"  -l val     Filename of the log file to use.  If not specified, results will be logged to stderr.\n"
		"\n"
		"Examples:\n"
		"  %s talker.ini\n"
		"    Start 1 stream with data from the ini file.\n\n"
		"  %s talker1.ini talker2.ini\n"
		"    Start 2 streams with data from the ini files.\n\n"
		"  %s -I eth0 talker1.ini talker2.ini\n"
		"    Start 2 streams with data from the ini files, both talkers use eth0 interface.\n\n"
		"  %s -I eth0 talker1.ini talker2.ini listener1.ini,ifname=pcap:eth0\n"
		"    Start 3 streams with data from the ini files, talkers 1&2 use eth0 interface, listener1 use pcap:eth0.\n\n"
		,
		programName, programName, programName, programName, programName);
}

/**********************************************
 * main
 */
int main(int argc, char *argv[])
{
	AVB_TRACE_ENTRY(AVB_TRACE_HOST);

	int iniIdx = 0;
	char *programName;
	char *optIfnameGlobal = NULL;
	char *optLogFileName = NULL;

	programName = strrchr(argv[0], '/');
	programName = programName ? programName + 1 : argv[0];

	if (argc < 2) {
		openavbTlHostUsage(programName);
		exit(-1);
	}

	tl_handle_t *tlHandleList = NULL;
	int i1;

	// Process command line
	bool optDone = FALSE;
	while (!optDone) {
		int opt = getopt(argc, argv, "hI:l:");
		if (opt != EOF) {
			switch (opt) {
				case 'I':
					optIfnameGlobal = strdup(optarg);
					break;
				case 'l':
					optLogFileName = strdup(optarg);
					break;
				case 'h':
				default:
					openavbTlHostUsage(programName);
					exit(-1);
			}
		}
		else {
			optDone = TRUE;
		}
	}

	osalAVBInitialize(optLogFileName, optIfnameGlobal);

	iniIdx = optind;
	U32 tlCount = argc - iniIdx;

	if (!openavbTLInitialize(tlCount)) {
		AVB_LOG_ERROR("Unable to initialize talker listener library");
		osalAVBFinalize();
		exit(-1);
	}

	// Setup signal handler
	// We catch SIGINT and shutdown cleanly
	bool err;
	struct sigaction sa;
	sa.sa_handler = openavbTLSigHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	err = sigaction(SIGINT, &sa, NULL);
	if (err)
	{
		AVB_LOG_ERROR("Failed to setup SIGINT handler");
		osalAVBFinalize();
		exit(-1);
	}
	err = sigaction(SIGTERM, &sa, NULL);
	if (err)
	{
		AVB_LOG_ERROR("Failed to setup SIGTERM handler");
		osalAVBFinalize();
		exit(-1);
	}
	err = sigaction(SIGUSR1, &sa, NULL);
	if (err)
	{
		AVB_LOG_ERROR("Failed to setup SIGUSR1 handler");
		osalAVBFinalize();
		exit(-1);
	}

	// Ignore SIGPIPE signals.
	signal(SIGPIPE, SIG_IGN);

	registerStaticMapModule(openavbMapPipeInitialize);
	registerStaticMapModule(openavbMapAVTPAudioInitialize);
	registerStaticMapModule(openavbMapCtrlInitialize);
	registerStaticMapModule(openavbMapCrfInitialize);
	registerStaticMapModule(openavbMapH264Initialize);
	registerStaticMapModule(openavbMapMjpegInitialize);
	registerStaticMapModule(openavbMapMpeg2tsInitialize);
	registerStaticMapModule(openavbMapNullInitialize);
	registerStaticMapModule(openavbMapUncmpAudioInitialize);

	registerStaticIntfModule(openavbIntfEchoInitialize);
	registerStaticIntfModule(openavbIntfCtrlInitialize);
	registerStaticIntfModule(openavbIntfLoggerInitialize);
	registerStaticIntfModule(openavbIntfNullInitialize);
	registerStaticIntfModule(openavbIntfToneGenInitialize);
	registerStaticIntfModule(openavbIntfViewerInitialize);
	registerStaticIntfModule(openavbIntfAlsaInitialize);
	registerStaticIntfModule(openavbIntfAvb32DirectInitialize);
	registerStaticIntfModule(openavbIntfBus32SplitInitialize);
	registerStaticIntfModule(openavbIntfMpeg2tsFileInitialize);
	registerStaticIntfModule(openavbIntfWavFileInitialize);
#ifdef AVB_FEATURE_GSTREAMER
	registerStaticIntfModule(openavbIntfMjpegGstInitialize);
	registerStaticIntfModule(openavbIntfMpeg2tsGstInitialize);
	registerStaticIntfModule(openavbIntfH264RtpGstInitialize);
#endif
	tlHandleList = calloc(1, sizeof(tl_handle_t) * tlCount);
	openavb_tl_cfg_t *tlCfgList = calloc(1, sizeof(openavb_tl_cfg_t) * tlCount);
	deferred_start_state_t *deferredStartList = calloc(1, sizeof(deferred_start_state_t) * tlCount);
	if (!tlHandleList || !tlCfgList || !deferredStartList) {
		AVB_LOG_ERROR("Unable to allocate talker/listener startup state.");
		osalAVBFinalize();
		exit(-1);
	}

	// Open all streams
	for (i1 = 0; i1 < tlCount; i1++) {
		tlHandleList[i1] = openavbTLOpen();
	}

	// Parse ini and configure all streams
	for (i1 = 0; i1 < tlCount; i1++) {
		openavb_tl_cfg_t cfg;
		openavb_tl_cfg_name_value_t NVCfg;
		char iniFile[1024];

		snprintf(iniFile, sizeof(iniFile), "%s", argv[i1 + iniIdx]);

		if (optIfnameGlobal && !strcasestr(iniFile, ",ifname=")) {
			snprintf(iniFile + strlen(iniFile), sizeof(iniFile), ",ifname=%s", optIfnameGlobal);
		}

		openavbTLInitCfg(&cfg);
		memset(&NVCfg, 0, sizeof(NVCfg));

			if (!openavbTLReadIniFileOsal(tlHandleList[i1], iniFile, &cfg, &NVCfg)) {
				AVB_LOGF_ERROR("Error reading ini file: %s\n", argv[i1 + iniIdx]);
				osalAVBFinalize();
				exit(-1);
			}
			if (!openavbTLConfigure(tlHandleList[i1], &cfg, &NVCfg)) {
				AVB_LOGF_ERROR("Error configuring: %s\n", argv[i1 + iniIdx]);
				osalAVBFinalize();
				exit(-1);
			}
			tlCfgList[i1] = cfg;
			deferredStartList[i1].pending =
				(cfg.role == AVB_ROLE_TALKER && cfg.defer_start_until_selected_clock);

		int i2;
		for (i2 = 0; i2 < NVCfg.nLibCfgItems; i2++) {
			free(NVCfg.libCfgNames[i2]);
			free(NVCfg.libCfgValues[i2]);
		}
	}

#ifdef AVB_FEATURE_GSTREAMER
	// If we're supporting the interface modules which use GStreamer,
	// initialize GStreamer here to avoid errors.
	gst_init(0, NULL);
#endif

	// Run any streams where the stop initial state was not requested.
	for (i1 = 0; i1 < tlCount; i1++) {
		if (deferredStartList[i1].pending) {
			AVB_LOGF_INFO("Deferring stream start until selected clock is stable: %s uid=%u wait=%uus",
				tlCfgList[i1].friendly_name,
				tlCfgList[i1].stream_uid,
				tlCfgList[i1].defer_start_stable_usec);
			continue;
		}
		if (openavbTLGetInitialState(tlHandleList[i1]) != TL_INIT_STATE_STOPPED) {
			openavbTLRun(tlHandleList[i1]);
		}
	}

	while (bRunning) {
		for (i1 = 0; i1 < tlCount; i1++) {
			if (!deferredStartList[i1].pending || openavbTLIsRunning(tlHandleList[i1])) {
				continue;
			}

			U32 selectionGeneration = 0;
			U32 sourceGeneration = 0;
			U64 nowNs = 0;
			bool haveNow = CLOCK_GETTIME64(OPENAVB_TIMER_CLOCK, &nowNs);

			if (!haveNow ||
					!deferredStartSelectedClockReady(&selectionGeneration, &sourceGeneration)) {
				deferredStartList[i1].stableSinceNs = 0;
				deferredStartList[i1].selectionGeneration = 0;
				deferredStartList[i1].sourceGeneration = 0;
				continue;
			}

			// The selected media clock sample generation advances continuously while the
			// source is healthy. Treat selection changes as instability, but do not keep
			// restarting the timer just because fresh clock samples are arriving.
			if (deferredStartList[i1].stableSinceNs == 0 ||
					deferredStartList[i1].selectionGeneration != selectionGeneration) {
				deferredStartList[i1].stableSinceNs = nowNs;
				deferredStartList[i1].selectionGeneration = selectionGeneration;
				deferredStartList[i1].sourceGeneration = sourceGeneration;
				continue;
			}

			deferredStartList[i1].sourceGeneration = sourceGeneration;

			if ((nowNs - deferredStartList[i1].stableSinceNs) <
					((U64)tlCfgList[i1].defer_start_stable_usec * 1000ULL)) {
				continue;
			}

			AVB_LOGF_WARNING("Deferred stream start released: %s uid=%u stable_for=%lluus selection_gen=%u source_gen=%u",
				tlCfgList[i1].friendly_name,
				tlCfgList[i1].stream_uid,
				(unsigned long long)((nowNs - deferredStartList[i1].stableSinceNs) / 1000ULL),
				selectionGeneration,
				sourceGeneration);
			openavbTLRun(tlHandleList[i1]);
			deferredStartList[i1].pending = FALSE;
		}
		SLEEP_MSEC(1);
	}

	for (i1 = 0; i1 < tlCount; i1++) {
		openavbTLStop(tlHandleList[i1]);
	}

	for (i1 = 0; i1 < tlCount; i1++) {
		openavbTLClose(tlHandleList[i1]);
	}

	openavbTLCleanup();
	free(deferredStartList);
	free(tlCfgList);
	free(tlHandleList);

	if (optLogFileName) {
		free(optLogFileName);
		optLogFileName = NULL;
	}

#ifdef AVB_FEATURE_GSTREAMER
	// If we're supporting the interface modules which use GStreamer,
	// De-initialize GStreamer to clean up resources.
	gst_deinit();
#endif

	osalAVBFinalize();

	AVB_TRACE_EXIT(AVB_TRACE_HOST);
	exit(0);
}
