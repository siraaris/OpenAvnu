/*************************************************************************************************************
Copyright (c) 2026
All rights reserved.
*************************************************************************************************************/

/*
 * HEADER SUMMARY : Runtime clock-source selection and CRF reference cache.
 */

#ifndef OPENAVB_CLOCK_SOURCE_RUNTIME_PUB_H
#define OPENAVB_CLOCK_SOURCE_RUNTIME_PUB_H 1

#include "openavb_types_pub.h"

typedef struct {
	bool valid;
	U16 clock_domain_index;
	U16 clock_source_index;
	U16 clock_source_flags;
	U16 clock_source_type;
	U16 clock_source_location_type;
	U16 clock_source_location_index;
	U32 generation;
} openavb_clock_source_runtime_t;

void openavbClockSourceRuntimeSetSelection(
	U16 clock_domain_index,
	U16 clock_source_index,
	U16 clock_source_flags,
	U16 clock_source_type,
	U16 clock_source_location_type,
	U16 clock_source_location_index);

bool openavbClockSourceRuntimeGetSelection(openavb_clock_source_runtime_t *pSelection);

void openavbClockSourceRuntimeSetCrfTimeForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 crfTimeNs,
	bool uncertain);

bool openavbClockSourceRuntimeGetCrfTimeForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 *pCrfTimeNs,
	bool *pUncertain,
	U32 *pGeneration);

void openavbClockSourceRuntimeClearCrfTimeForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index);

void openavbClockSourceRuntimeSetCrfTime(U64 crfTimeNs, bool uncertain);

bool openavbClockSourceRuntimeGetCrfTime(U64 *pCrfTimeNs, bool *pUncertain, U32 *pGeneration);

void openavbClockSourceRuntimeClearCrfTime(void);

void openavbClockSourceRuntimeSetMediaClockForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 mediaClockNs,
	bool uncertain);

bool openavbClockSourceRuntimeGetMediaClockForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U64 *pMediaClockNs,
	bool *pUncertain,
	U32 *pGeneration);

void openavbClockSourceRuntimeClearMediaClockForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index);

bool openavbClockSourceRuntimeAcquireWarmupAnchorForLocation(
	U16 clock_source_location_type,
	U16 clock_source_location_index,
	U32 selectionGeneration,
	U32 recoveryGeneration,
	U32 sourceGeneration,
	U64 proposedAnchorNs,
	U64 *pAnchorNs,
	bool *pWasCreated);

void openavbClockSourceRuntimeReset(void);

#endif // OPENAVB_CLOCK_SOURCE_RUNTIME_PUB_H
