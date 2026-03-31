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

#include <inttypes.h>
#include <stdlib.h>
#include <linux/ptp_clock.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "avb_gptp.h"

#include "openavb_platform.h"
#include "openavb_time_osal.h"
#include "openavb_trace.h"

#define	AVB_LOG_COMPONENT	"osalTime"
#include "openavb_pub.h"
#include "openavb_log.h"

static pthread_mutex_t gOSALTimeInitMutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()  	pthread_mutex_lock(&gOSALTimeInitMutex)
#define UNLOCK()	pthread_mutex_unlock(&gOSALTimeInitMutex)

static bool bInitialized = FALSE;
static int gPtpShmFd = -1;
static char *gPtpMmap = NULL;
gPtpTimeData gPtpTD;

#define GPTP_WALLTIME_HOLDOVER_ENTER_NS (5000000ULL)
#define GPTP_WALLTIME_HOLDOVER_EXIT_NS  (1000000ULL)
#define GPTP_WALLTIME_RECOVERY_DEBOUNCE_NS (500000000ULL)

typedef struct {
	bool valid;
	bool holdoverActive;
	U64 lastReturnedNs;
	U64 lastMonotonicNs;
	U64 holdoverCount;
	U64 holdoverStartMonoNs;
} openavb_walltime_holdover_t;

static __thread openavb_walltime_holdover_t gWalltimeHoldover = {0};
static U32 gWalltimeRecoveryGeneration = 0;
static U64 gWalltimeRecoveryLastMonoNs = 0;

// Brief BMCA / GM disturbances should ride through holdover without forcing a
// media-clock recovery epoch reset. Only longer outages escalate to a
// generation bump that downstream CRF/AAF logic treats as a hard recovery.
#define GPTP_WALLTIME_RECOVERY_RESET_THRESHOLD_NS (2000000000ULL)

static U32 x_noteWalltimeRecoveryEvent(U64 monoNow)
{
	U32 generation;

	LOCK();
	if (gWalltimeRecoveryLastMonoNs == 0 ||
			monoNow < gWalltimeRecoveryLastMonoNs ||
			(monoNow - gWalltimeRecoveryLastMonoNs) > GPTP_WALLTIME_RECOVERY_DEBOUNCE_NS) {
		gWalltimeRecoveryGeneration++;
		gWalltimeRecoveryLastMonoNs = monoNow;
	}
	generation = gWalltimeRecoveryGeneration;
	UNLOCK();

	return generation;
}

static bool x_getMonotonicTime(U64 *timeNsec)
{
	struct timespec ts;
	if (!timeNsec) {
		return FALSE;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return FALSE;
	}
	*timeNsec = ((U64)ts.tv_sec * NANOSECONDS_PER_SECOND) + (U64)ts.tv_nsec;
	return TRUE;
}

static bool x_timeInit(void) {
	AVB_TRACE_ENTRY(AVB_TRACE_TIME);

	if (gptpinit(&gPtpShmFd, &gPtpMmap) < 0) {
		AVB_LOG_ERROR("GPTP init failed");
		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return FALSE;
	}

	if (gptpgetdata(gPtpMmap, &gPtpTD) < 0) {
		AVB_LOG_ERROR("GPTP data fetch failed");
		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return FALSE;
	}

	AVB_LOGF_INFO("local_time = %" PRIu64, gPtpTD.local_time);
	AVB_LOGF_INFO("ml_phoffset = %" PRId64 ", ls_phoffset = %" PRId64, gPtpTD.ml_phoffset, gPtpTD.ls_phoffset);
	AVB_LOGF_INFO("ml_freqffset = %Lf, ls_freqoffset = %Lf", gPtpTD.ml_freqoffset, gPtpTD.ls_freqoffset);

	AVB_TRACE_EXIT(AVB_TRACE_TIME);
	return TRUE;
}

static bool x_getPTPTime(U64 *timeNsec) {
	AVB_TRACE_ENTRY(AVB_TRACE_TIME);

	U64 monoNow = 0;
	bool haveMonoNow = x_getMonotonicTime(&monoNow);

	if (gptpgetdata(gPtpMmap, &gPtpTD) < 0) {
		if (gWalltimeHoldover.valid && haveMonoNow) {
			U64 projectedNs = gWalltimeHoldover.lastReturnedNs;
			if (monoNow >= gWalltimeHoldover.lastMonotonicNs) {
				projectedNs += (monoNow - gWalltimeHoldover.lastMonotonicNs);
			}
			gWalltimeHoldover.lastReturnedNs = projectedNs;
			gWalltimeHoldover.lastMonotonicNs = monoNow;
			*timeNsec = projectedNs;
			AVB_TRACE_EXIT(AVB_TRACE_TIME);
			return TRUE;
		}
		AVB_LOG_ERROR("GPTP data fetch failed");
		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return FALSE;
	}

	uint64_t now_local;
	uint64_t update_8021as;
	int64_t delta_8021as;
	int64_t delta_local;

	if (gptplocaltime(&gPtpTD, &now_local)) {
		update_8021as = gPtpTD.local_time - gPtpTD.ml_phoffset;
		delta_local = now_local - gPtpTD.local_time;
		delta_8021as = gPtpTD.ml_freqoffset * delta_local;
		U64 rawTimeNs = update_8021as + delta_8021as;
		U64 filteredTimeNs = rawTimeNs;

		if (haveMonoNow) {
			if (!gWalltimeHoldover.valid) {
				gWalltimeHoldover.valid = TRUE;
			}
			else {
				U64 projectedNs = gWalltimeHoldover.lastReturnedNs;
				if (monoNow >= gWalltimeHoldover.lastMonotonicNs) {
					projectedNs += (monoNow - gWalltimeHoldover.lastMonotonicNs);
				}

				S64 wallErrNs = (rawTimeNs >= projectedNs)
					? (S64)(rawTimeNs - projectedNs)
					: -((S64)(projectedNs - rawTimeNs));
				U64 absWallErrNs = (U64)llabs(wallErrNs);

				if (gWalltimeHoldover.holdoverActive) {
					if (absWallErrNs <= GPTP_WALLTIME_HOLDOVER_EXIT_NS) {
						U64 holdoverDurationNs = 0;
						U32 recoveryGeneration = gWalltimeRecoveryGeneration;
						if (gWalltimeHoldover.holdoverStartMonoNs != 0 &&
								monoNow >= gWalltimeHoldover.holdoverStartMonoNs) {
							holdoverDurationNs = monoNow - gWalltimeHoldover.holdoverStartMonoNs;
						}
						if (holdoverDurationNs >= GPTP_WALLTIME_RECOVERY_RESET_THRESHOLD_NS) {
							recoveryGeneration = x_noteWalltimeRecoveryEvent(monoNow);
						}
						gWalltimeHoldover.holdoverActive = FALSE;
						AVB_LOGF_WARNING(
							"GPTP walltime holdover recovered: raw=%" PRIu64 " projected=%" PRIu64 " err=%" PRId64 " holds=%" PRIu64 " duration=%" PRIu64 "ns generation=%u",
							rawTimeNs,
							projectedNs,
							wallErrNs,
							gWalltimeHoldover.holdoverCount,
							holdoverDurationNs,
							recoveryGeneration);
						filteredTimeNs = rawTimeNs;
						gWalltimeHoldover.holdoverStartMonoNs = 0;
					}
					else {
						filteredTimeNs = projectedNs;
					}
				}
				else if (absWallErrNs > GPTP_WALLTIME_HOLDOVER_ENTER_NS) {
					gWalltimeHoldover.holdoverActive = TRUE;
					gWalltimeHoldover.holdoverCount++;
					gWalltimeHoldover.holdoverStartMonoNs = monoNow;
					filteredTimeNs = projectedNs;
					AVB_LOGF_WARNING(
						"GPTP walltime discontinuity detected: raw=%" PRIu64 " projected=%" PRIu64 " err=%" PRId64 " hold=%" PRIu64 " reset_threshold=%" PRIu64 "ns",
						rawTimeNs,
						projectedNs,
						wallErrNs,
						gWalltimeHoldover.holdoverCount,
						(U64)GPTP_WALLTIME_RECOVERY_RESET_THRESHOLD_NS);
				}
			}

			gWalltimeHoldover.lastReturnedNs = filteredTimeNs;
			gWalltimeHoldover.lastMonotonicNs = monoNow;
		}

		*timeNsec = filteredTimeNs;

		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return TRUE;
	}

	if (gWalltimeHoldover.valid && haveMonoNow) {
		U64 projectedNs = gWalltimeHoldover.lastReturnedNs;
		if (monoNow >= gWalltimeHoldover.lastMonotonicNs) {
			projectedNs += (monoNow - gWalltimeHoldover.lastMonotonicNs);
		}
		gWalltimeHoldover.lastReturnedNs = projectedNs;
		gWalltimeHoldover.lastMonotonicNs = monoNow;
		*timeNsec = projectedNs;
		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return TRUE;
	}

	AVB_TRACE_EXIT(AVB_TRACE_TIME);
	return FALSE;
}

bool osalAVBTimeInit(void) {
	AVB_TRACE_ENTRY(AVB_TRACE_TIME);

	LOCK();
	if (!bInitialized) {
		if (x_timeInit())
			bInitialized = TRUE;
	}
	UNLOCK();

	AVB_TRACE_EXIT(AVB_TRACE_TIME);
	return bInitialized;
}

bool osalAVBTimeClose(void) {
	AVB_TRACE_ENTRY(AVB_TRACE_TIME);

	gptpdeinit(&gPtpShmFd, &gPtpMmap);

	AVB_TRACE_EXIT(AVB_TRACE_TIME);
	return TRUE;
}

bool osalClockGettime(openavb_clockId_t openavbClockId, struct timespec *getTime) {
	AVB_TRACE_ENTRY(AVB_TRACE_TIME);

	if (openavbClockId < OPENAVB_CLOCK_WALLTIME)
	{
		clockid_t clockId = CLOCK_MONOTONIC;
		switch (openavbClockId) {
		case OPENAVB_CLOCK_REALTIME:
			clockId = CLOCK_REALTIME;
			break;
		case OPENAVB_CLOCK_MONOTONIC:
			clockId = CLOCK_MONOTONIC;
			break;
		case OPENAVB_TIMER_CLOCK:
			clockId = CLOCK_MONOTONIC;
			break;
		case OPENAVB_CLOCK_WALLTIME:
			break;
		}
		if (!clock_gettime(clockId, getTime)) return TRUE;
	}
	else if (openavbClockId == OPENAVB_CLOCK_WALLTIME) {
		U64 timeNsec;
		if (!x_getPTPTime(&timeNsec)) {
			AVB_TRACE_EXIT(AVB_TRACE_TIME);
			return FALSE;
		}
		getTime->tv_sec = timeNsec / NANOSECONDS_PER_SECOND;
		getTime->tv_nsec = timeNsec % NANOSECONDS_PER_SECOND;
		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return TRUE;
	}
	AVB_TRACE_EXIT(AVB_TRACE_TIME);
	return FALSE;
}

bool osalClockGettime64(openavb_clockId_t openavbClockId, U64 *timeNsec) {
	if (openavbClockId < OPENAVB_CLOCK_WALLTIME)
	{
		clockid_t clockId = CLOCK_MONOTONIC;
		switch (openavbClockId) {
		case OPENAVB_CLOCK_REALTIME:
			clockId = CLOCK_REALTIME;
			break;
		case OPENAVB_CLOCK_MONOTONIC:
			clockId = CLOCK_MONOTONIC;
			break;
		case OPENAVB_TIMER_CLOCK:
			clockId = CLOCK_MONOTONIC;
			break;
		case OPENAVB_CLOCK_WALLTIME:
			break;
		}
		struct timespec getTime;
		if (!clock_gettime(clockId, &getTime)) {
			*timeNsec = ((U64)getTime.tv_sec * (U64)NANOSECONDS_PER_SECOND) + (U64)getTime.tv_nsec;
			AVB_TRACE_EXIT(AVB_TRACE_TIME);
			return TRUE;
		}
	}
	else if (openavbClockId == OPENAVB_CLOCK_WALLTIME) {
		AVB_TRACE_EXIT(AVB_TRACE_TIME);
		return x_getPTPTime(timeNsec);
	}
	AVB_TRACE_EXIT(AVB_TRACE_TIME);
	return FALSE;
}

U32 osalClockGetWalltimeRecoveryGeneration(void)
{
	U32 generation;

	LOCK();
	generation = gWalltimeRecoveryGeneration;
	UNLOCK();

	return generation;
}
