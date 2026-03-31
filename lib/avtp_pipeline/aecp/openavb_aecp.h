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
 * MODULE : AVDECC Enumeration and control protocol (AECP)
 * MODULE SUMMARY : Interface for the AVDECC Enumeration and control protocol (AECP)
 ******************************************************************
 */

#ifndef OPENAVB_AECP_H
#define OPENAVB_AECP_H 1

#include "openavb_avdecc.h"
#include "openavb_aecp_pub.h"

#define OPENAVB_AECP_AVTP_SUBTYPE (0x7b)

// message_type field IEEE Std 1722.1-2013 clause 9.2.1.1.5
#define OPENAVB_AECP_MESSAGE_TYPE_AEM_COMMAND (0)
#define OPENAVB_AECP_MESSAGE_TYPE_AEM_RESPONSE (1)
#define OPENAVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND (2)
#define OPENAVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE (3)
#define OPENAVB_AECP_MESSAGE_TYPE_AVC_COMMAND (4)
#define OPENAVB_AECP_MESSAGE_TYPE_AVC_RESPONSE (5)
#define OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND (6)
#define OPENAVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE (7)
#define OPENAVB_AECP_MESSAGE_TYPE_HDCP_APM_COMMAND (8)
#define OPENAVB_AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE (9)
#define OPENAVB_AECP_MESSAGE_TYPE_EXTENDED_COMMAND (14)
#define OPENAVB_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE (15)

#define OPENAVB_AECP_AUDIO_MAP_MAX_MAPPINGS (63)
#define OPENAVB_AECP_DYNAMIC_INFO_DATA_LENGTH (1400)
#define OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH (6)

#define OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MILAN_INFO (0x0000)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_SET_SYSTEM_UNIQUE_ID (0x0001)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_GET_SYSTEM_UNIQUE_ID (0x0002)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_SET_MEDIA_CLOCK_REFERENCE_INFO (0x0003)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_GET_MEDIA_CLOCK_REFERENCE_INFO (0x0004)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_BIND_STREAM (0x0005)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_UNBIND_STREAM (0x0006)
#define OPENAVB_AECP_MVU_COMMAND_TYPE_GET_STREAM_INPUT_INFO_EX (0x0007)

#define OPENAVB_AECP_MILAN_INFO_FEATURE_FLAG_REDUNDANCY (1u << 0)
#define OPENAVB_AECP_MILAN_INFO_FEATURE_FLAG_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING (1u << 1)
#define OPENAVB_AECP_MILAN_INFO_FEATURE_FLAG_MVU_BINDING (1u << 2)
#define OPENAVB_AECP_MILAN_INFO_FEATURE_FLAG_TALKER_SIGNAL_PRESENCE (1u << 3)

// status field IEEE Std 1722.1-2013 clause 9.2.1.1.6
#define OPENAVB_AECP_STATUS_SUCCESS (0)
#define OPENAVB_AECP_STATUS_NOT_IMPLEMENTED (1)

#define OPENAVB_AECP_AA_STATUS_SUCCESS (0)
#define OPENAVB_AECP_AA_STATUS_NOT_IMPLEMENTED (1)
#define OPENAVB_AECP_AA_STATUS_ADDRESS_TOO_LOW (2)
#define OPENAVB_AECP_AA_STATUS_ADDRESS_TOO_HIGH (3)
#define OPENAVB_AECP_AA_STATUS_ADDRESS_INVALID (4)
#define OPENAVB_AECP_AA_STATUS_TLV_INVALID (5)
#define OPENAVB_AECP_AA_STATUS_DATA_INVALID (6)
#define OPENAVB_AECP_AA_STATUS_UNSUPPORTED (7)

// Milan Vendor Unique AECP status field
#define OPENAVB_AECP_MVU_STATUS_SUCCESS (0)
#define OPENAVB_AECP_MVU_STATUS_NOT_IMPLEMENTED (1)
#define OPENAVB_AECP_MVU_STATUS_NO_SUCH_DESCRIPTOR (2)
#define OPENAVB_AECP_MVU_STATUS_ENTITY_LOCKED (3)
#define OPENAVB_AECP_MVU_STATUS_BAD_ARGUMENTS (7)
#define OPENAVB_AECP_MVU_STATUS_ENTITY_MISBEHAVING (10)
#define OPENAVB_AECP_MVU_STATUS_PAYLOAD_TOO_SHORT (13)

typedef struct {
	U8 cd;
	U8 subtype;
	U8 sv;
	U8 version;
	U8 message_type;			// Redefined from control data
	U8 status;
	U16 control_data_length;
	U8 target_entity_id[8];		// Redefined from stream_id
} openavb_aecp_control_header_t;

typedef struct {
	U8 controller_entity_id[8];
	U16 sequence_id;
} openavb_aecp_common_data_unit_t;

typedef struct {
	U32 flags;
	U8 owner_id[8];
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_acquire_entity_t;

typedef struct {
	U32 flags;
	U8 locked_id[8];
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_commandresponse_data_lock_entity_t;

typedef struct {
	// No command specific data
} openavb_aecp_commandresponse_data_entity_available_t;

typedef struct {
	// No command specific data
} openavb_aecp_commandresponse_data_controller_available_t;

typedef struct {
	U16 configuration_index;
	U16 reserved;
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_read_descriptor_t;

typedef struct {
	U16 configuration_index;
	U16 reserved;
	U16 descriptor_length;
	U8 descriptor_data[508];
} openavb_aecp_response_data_read_descriptor_t;

typedef struct {
	U16 reserved;
	U16 configuration_index;
} openavb_aecp_commandresponse_data_set_configuration_t;

typedef struct {
	// No command specific data
} openavb_aecp_command_data_get_configuration_t;

typedef struct {
	U16 reserved;
	U16 configuration_index;
} openavb_aecp_response_data_get_configuration_t;

// SET_STREAM_FORMAT command and response IEEE Std 1722.1-2013 clause 7.4.9
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U8 stream_format[8];		// Stream format sub details aren't needed for this command
} openavb_aecp_commandresponse_data_set_stream_format_t;

// GET_STREAM_FORMAT command IEEE Std 1722.1-2013 clause 7.4.10
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_stream_format_t;

// GET_STREAM_FORMAT response IEEE Std 1722.1-2013 clause 7.4.10
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U8 stream_format[8];		// Stream format sub details aren't needed for this command
} openavb_aecp_response_data_get_stream_format_t;

// SET_STREAM_INFO command and response IEEE Std 1722.1-2013 clause 7.4.15
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U32 flags;
	U8 stream_format[8];
	U8 stream_id[8];
	U32 msrp_accumulated_latency;
	U8 stream_dest_mac[6];
	U8 msrp_failure_code;
	U8 reserved_1;
	U8 msrp_failure_bridge_id[8];
	U16 stream_vlan_id;
	U16 reserved_2;
} openavb_aecp_commandresponse_data_set_stream_info_t;

// GET_STREAM_INFO command IEEE Std 1722.1-2013 clause 7.4.16
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_stream_info_t;

// GET_STREAM_INFO response IEEE Std 1722.1-2013 clause 7.4.16
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U32 flags;
	U8 stream_format[8];
	U8 stream_id[8];
	U32 msrp_accumulated_latency;
	U8 stream_dest_mac[6];
	U8 msrp_failure_code;
	U8 reserved_1;
	U8 msrp_failure_bridge_id[8];
	U16 stream_vlan_id;
	U16 reserved_2;
	U32 flags_ex;			// Extension flags (Milan / IEEE 1722.1-2021)
} openavb_aecp_response_data_get_stream_info_t;

typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 name_index;
	U16 configuration_index;
	U8 name[OPENAVB_AEM_STRLEN_MAX];
} openavb_aecp_commandresponse_data_set_name_t;

typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 name_index;
	U16 configuration_index;
} openavb_aecp_command_data_get_name_t;

typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 name_index;
	U16 configuration_index;
	U8 name[OPENAVB_AEM_STRLEN_MAX];
} openavb_aecp_response_data_get_name_t;

typedef struct {
	U8 association_id[8];
} openavb_aecp_commandresponse_data_set_association_id_t;

typedef struct {
	// No command specific data
} openavb_aecp_command_data_get_association_id_t;

typedef struct {
	U8 association_id[8];
} openavb_aecp_response_data_get_association_id_t;

typedef struct {
	U16 configuration_index;
	U16 memory_object_index;
} openavb_aecp_command_data_get_memory_object_length_t;

typedef struct {
	U16 configuration_index;
	U16 memory_object_index;
	U64 length;
} openavb_aecp_response_data_get_memory_object_length_t;

// SET_SAMPLING_RATE command and response IEEE Std 1722.1-2013 clause 7.4.21
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U8 sampling_rate[4];		// Sampling rate format sub details aren't needed for this command
} openavb_aecp_commandresponse_data_set_sampling_rate_t;

// GET_SAMPLING_RATE command IEEE Std 1722.1-2013 clause 7.4.22
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_sampling_rate_t;

// GET_SAMPLING_RATE response IEEE Std 1722.1-2013 clause 7.4.22
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U8 sampling_rate[4];		// Sampling rate format sub details aren't needed for this command
} openavb_aecp_response_data_get_sampling_rate_t;

// SET_CLOCK_SOURCE command and response IEEE Std 1722.1-2013 clause 7.4.23
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 clock_source_index;
	U16 reserved;
} openavb_aecp_commandresponse_data_set_clock_source_t;

// GET_CLOCK_SOURCE command IEEE Std 1722.1-2013 clause 7.4.24
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_clock_source_t;

// GET_CLOCK_SOURCE response IEEE Std 1722.1-2013 clause 7.4.24
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 clock_source_index;
	U16 reserved;
} openavb_aecp_response_data_get_clock_source_t;

// SET_CONTROL command and response IEEE Std 1722.1-2013 clause 7.4.25
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	union {
		S8 linear_int8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT8];
		U8 linear_uint8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT8];
		S16 linear_int16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT16];
		U16 linear_uint16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT16];
		S32 linear_int32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT32];
		U32 linear_uint32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT32];
		S64 linear_int64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT64];
		U64 linear_uint64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT64];
		float linear_float[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_FLOAT];
		double linear_double[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_DOUBLE];
		// AVDECC_TODO : These aren't implemented in the CONTROl descriptor yet.
		//selector_int8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT8];
		//selector_uint8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT8];
		//selector_int16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT16];
		//selector_uint16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT16];
		//selector_int32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT32];
		//selector_uint32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT32];
		//selector_int64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT64];
		//selector_uint64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT64];
		//selector_float[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_FLOAT];
		//selector_double[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_DOUBLE];
		//selector_string[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_STRING];
		//array_int8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT8];
		//array_uint8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT8];
		//array_int16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT16];
		//array_uint16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT16];
		//array_int32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT32];
		//array_uint32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT32];
		//array_int64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT64];
		//array_uint64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT64];
		//array_float[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_FLOAT];
		//array_double[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_DOUBLE];
		//utf8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_UTF8];
		//bode_plot[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_BODE_PLOT];
		//smpte_time[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SMPTE_TIME];
		//sample_rate[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SAMPLE_RATE];
		//gptp_time[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_GPTP_TIME];
		//vendor[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_VENDOR];
	} values;

	U16 valuesCount;		// Not part of spec. Used to track elements in arrays.
} openavb_aecp_commandresponse_data_set_control_t;

// GET_CONTROL command IEEE Std 1722.1-2013 clause 7.4.26
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_control_t;

// GET_CONTROL response IEEE Std 1722.1-2013 clause 7.4.26
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	union {
		S8 linear_int8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT8];
		U8 linear_uint8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT8];
		S16 linear_int16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT16];
		U16 linear_uint16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT16];
		S32 linear_int32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT32];
		U32 linear_uint32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT32];
		S64 linear_int64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_INT64];
		U64 linear_uint64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_UINT64];
		float linear_float[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_FLOAT];
		double linear_double[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_LINEAR_DOUBLE];
		// AVDECC_TODO : These aren't implemented in the CONTROl descriptor yet.
		//selector_int8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT8];
		//selector_uint8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT8];
		//selector_int16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT16];
		//selector_uint16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT16];
		//selector_int32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT32];
		//selector_uint32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT32];
		//selector_int64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_INT64];
		//selector_uint64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_UINT64];
		//selector_float[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_FLOAT];
		//selector_double[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_DOUBLE];
		//selector_string[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SELECTOR_STRING];
		//array_int8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT8];
		//array_uint8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT8];
		//array_int16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT16];
		//array_uint16[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT16];
		//array_int32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT32];
		//array_uint32[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT32];
		//array_int64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_INT64];
		//array_uint64[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_UINT64];
		//array_float[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_FLOAT];
		//array_double[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_ARRAY_DOUBLE];
		//utf8[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_UTF8];
		//bode_plot[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_BODE_PLOT];
		//smpte_time[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SMPTE_TIME];
		//sample_rate[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_SAMPLE_RATE];
		//gptp_time[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_GPTP_TIME];
		//vendor[OPENAVB_AEM_VALUE_MAX_COUNT_CONTROL_VENDOR];
	} values;

	U16 valuesCount;		// Not part of spec. Used to track elements in arrays.
} openavb_aecp_response_data_get_control_t;

// START_STREAMING command and response IEEE Std 1722.1-2013 clause 7.4.35
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_commandresponse_data_start_streaming_t;

// STOP_STREAMING command and response IEEE Std 1722.1-2013 clause 7.4.36
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_commandresponse_data_stop_streaming_t;

// GET_AVB_INFO command IEEE Std 1722.1-2013 clause 7.4.40
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_avb_info_t;

// GET_AVB_INFO response IEEE Std 1722.1-2013 clause 7.4.40
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U8 as_grandmaster_id[8];
	U32 propagation_delay;
	U8 as_domain_number;
	U8 flags;
	U16 msrp_mappings_count;
	U8 msrp_mappings[64];
	U16 msrpMappingsCount;		// Not part of spec. Used to track elements in arrays.
} openavb_aecp_response_data_get_avb_info_t;

// GET_AS_PATH command IEEE Std 1722.1-2013 clause 7.4.41
typedef struct {
	U16 descriptor_index;
	U16 reserved;
} openavb_aecp_command_data_get_as_path_t;

// GET_AS_PATH response IEEE Std 1722.1-2013 clause 7.4.41
typedef struct {
	U16 descriptor_index;
	U16 as_path_count;
	U32 path_latency;
	U8 as_path[64];
	U16 asPathCount;			// Not part of spec. Used to track elements in arrays.
} openavb_aecp_response_data_get_as_path_t;

// GET_MAX_TRANSIT_TIME command IEEE Std 1722.1-2013 clause 7.4.43
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_max_transit_time_t;

// GET_MAX_TRANSIT_TIME response IEEE Std 1722.1-2013 clause 7.4.43
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U64 max_transit_time;
} openavb_aecp_response_data_get_max_transit_time_t;

typedef struct {
	U16 mapping_stream_index;
	U16 mapping_stream_channel;
	U16 mapping_cluster_offset;
	U16 mapping_cluster_channel;
} openavb_aecp_audio_mapping_t;

// GET_AUDIO_MAP command IEEE Std 1722.1-2013 clause 7.4.46
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 map_index;
	U16 reserved;
} openavb_aecp_command_data_get_audio_map_t;

// GET_AUDIO_MAP response IEEE Std 1722.1-2013 clause 7.4.46
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 map_index;
	U16 number_of_maps;
	U16 number_of_mappings;
	U16 reserved;
	openavb_aecp_audio_mapping_t mappings[OPENAVB_AECP_AUDIO_MAP_MAX_MAPPINGS];
	U16 mappingsCount;		// Not part of spec. Used to track elements in arrays.
} openavb_aecp_response_data_get_audio_map_t;

typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U16 number_of_mappings;
	U16 reserved;
	openavb_aecp_audio_mapping_t mappings[OPENAVB_AECP_AUDIO_MAP_MAX_MAPPINGS];
	U16 mappingsCount;		// Not part of spec. Used to track elements in arrays.
} openavb_aecp_commandresponse_data_audio_mappings_t;

// GET_COUNTERS command IEEE Std 1722.1-2013 clause 7.4.42
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_command_data_get_counters_t;

// GET_COUNTERS response IEEE Std 1722.1-2013 clause 7.4.42
typedef struct {
	U16 descriptor_type;
	U16 descriptor_index;
	U32 counters_valid;
	U16 counters_block_length;
	U8 counters_block[128];
} openavb_aecp_response_data_get_counters_t;

typedef struct {
	U16 payload_length;
	U8 payload[OPENAVB_AECP_DYNAMIC_INFO_DATA_LENGTH];
} openavb_aecp_commandresponse_data_get_dynamic_info_t;

typedef struct {
	U16 flags;
	U16 descriptor_type;
	U16 descriptor_index;
	U8 talker_entity_id[8];
	U16 talker_stream_index;
	U16 reserved;
} openavb_aecp_mvu_commandresponse_data_bind_stream_t;

typedef struct {
	U16 reserved_0;
	U16 descriptor_type;
	U16 descriptor_index;
} openavb_aecp_mvu_commandresponse_data_unbind_stream_t;

typedef struct {
	U16 reserved_0;
} openavb_aecp_mvu_commandresponse_data_get_milan_info_t;

typedef struct {
	U16 reserved_0;
	U32 protocol_version;
	U32 features_flags;
	U32 certification_version;
	U32 specification_version;
} openavb_aecp_mvu_response_data_get_milan_info_t;

typedef struct {
	U16 reserved_0;
	U64 system_unique_id;
	U8 system_name[OPENAVB_AEM_STRLEN_MAX];
} openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t;

typedef struct {
	U16 reserved_0;
} openavb_aecp_mvu_commandresponse_data_get_system_unique_id_t;

typedef openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t openavb_aecp_mvu_response_data_get_system_unique_id_t;

typedef struct {
	U16 clock_domain_index;
} openavb_aecp_mvu_commandresponse_data_get_media_clock_reference_info_t;

typedef struct {
	U16 clock_domain_index;
	U8 flags;
	U8 reserved_0;
	U8 default_media_clock_priority;
	U8 user_media_clock_priority;
	U32 reserved_1;
	U8 media_clock_domain_name[OPENAVB_AEM_STRLEN_MAX];
} openavb_aecp_mvu_response_data_get_media_clock_reference_info_t;

typedef struct {
	U16 reserved_0;
	U16 descriptor_type;
	U16 descriptor_index;
	U8 talker_entity_id[8];
	U16 talker_unique_id;
	U8 probing_acmp_status;
	U8 reserved_1;
} openavb_aecp_mvu_response_data_get_stream_input_info_ex_t;


typedef struct {
	U8 u;
	U16 command_type;
	union {
		openavb_aecp_command_data_acquire_entity_t acquireEntityCmd;
		openavb_aecp_command_data_acquire_entity_t acquireEntityRsp;
		openavb_aecp_commandresponse_data_lock_entity_t lockEntityCmd;
		openavb_aecp_commandresponse_data_lock_entity_t lockEntityRsp;
		openavb_aecp_commandresponse_data_entity_available_t entityAvailableCmd;
		openavb_aecp_commandresponse_data_entity_available_t entityAvailableRsp;
		openavb_aecp_commandresponse_data_controller_available_t controllerAvailableCmd;
		openavb_aecp_commandresponse_data_controller_available_t controllerAvailableRsp;
		openavb_aecp_command_data_read_descriptor_t readDescriptorCmd;
		openavb_aecp_response_data_read_descriptor_t readDescriptorRsp;
		openavb_aecp_commandresponse_data_set_configuration_t setConfigurationCmd;
		openavb_aecp_commandresponse_data_set_configuration_t setConfigurationRsp;
		openavb_aecp_command_data_get_configuration_t getConfigurationCmd;
		openavb_aecp_response_data_get_configuration_t getConfigurationRsp;
		openavb_aecp_commandresponse_data_set_stream_format_t setStreamFormatCmd;
		openavb_aecp_commandresponse_data_set_stream_format_t setStreamFormatRsp;
		openavb_aecp_command_data_get_stream_format_t getStreamFormatCmd;
		openavb_aecp_response_data_get_stream_format_t getStreamFormatRsp;
		openavb_aecp_commandresponse_data_set_stream_info_t setStreamInfoCmd;
		openavb_aecp_commandresponse_data_set_stream_info_t setStreamInfoRsp;
		openavb_aecp_command_data_get_stream_info_t getStreamInfoCmd;
		openavb_aecp_response_data_get_stream_info_t getStreamInfoRsp;
		openavb_aecp_commandresponse_data_set_name_t setNameCmd;
		openavb_aecp_commandresponse_data_set_name_t setNameRsp;
		openavb_aecp_command_data_get_name_t getNameCmd;
		openavb_aecp_response_data_get_name_t getNameRsp;
		openavb_aecp_commandresponse_data_set_association_id_t setAssociationIDCmd;
		openavb_aecp_commandresponse_data_set_association_id_t setAssociationIDRsp;
		openavb_aecp_command_data_get_association_id_t getAssociationIDCmd;
		openavb_aecp_response_data_get_association_id_t getAssociationIDRsp;
		openavb_aecp_commandresponse_data_set_sampling_rate_t setSamplingRateCmd;
		openavb_aecp_commandresponse_data_set_sampling_rate_t setSamplingRateRsp;
		openavb_aecp_command_data_get_sampling_rate_t getSamplingRateCmd;
		openavb_aecp_response_data_get_sampling_rate_t getSamplingRateRsp;
		openavb_aecp_commandresponse_data_set_clock_source_t setClockSourceCmd;
		openavb_aecp_commandresponse_data_set_clock_source_t setClockSourceRsp;
		openavb_aecp_command_data_get_clock_source_t getClockSourceCmd;
		openavb_aecp_response_data_get_clock_source_t getClockSourceRsp;
		openavb_aecp_commandresponse_data_set_control_t setControlCmd;
		openavb_aecp_commandresponse_data_set_control_t setControlRsp;
		openavb_aecp_command_data_get_control_t getControlCmd;
		openavb_aecp_response_data_get_control_t getControlRsp;
		openavb_aecp_commandresponse_data_start_streaming_t startStreamingCmd;
		openavb_aecp_commandresponse_data_start_streaming_t startStreamingRsp;
		openavb_aecp_commandresponse_data_stop_streaming_t stopStreamingCmd;
		openavb_aecp_commandresponse_data_stop_streaming_t stopStreamingRsp;
		openavb_aecp_command_data_get_avb_info_t getAvbInfoCmd;
		openavb_aecp_response_data_get_avb_info_t getAvbInfoRsp;
		openavb_aecp_command_data_get_as_path_t getAsPathCmd;
		openavb_aecp_response_data_get_as_path_t getAsPathRsp;
		openavb_aecp_command_data_get_max_transit_time_t getMaxTransitTimeCmd;
		openavb_aecp_response_data_get_max_transit_time_t getMaxTransitTimeRsp;
		openavb_aecp_command_data_get_audio_map_t getAudioMapCmd;
		openavb_aecp_response_data_get_audio_map_t getAudioMapRsp;
		openavb_aecp_commandresponse_data_audio_mappings_t addAudioMappingsCmd;
		openavb_aecp_commandresponse_data_audio_mappings_t addAudioMappingsRsp;
		openavb_aecp_commandresponse_data_audio_mappings_t removeAudioMappingsCmd;
		openavb_aecp_commandresponse_data_audio_mappings_t removeAudioMappingsRsp;
		openavb_aecp_command_data_get_counters_t getCountersCmd;
		openavb_aecp_response_data_get_counters_t getCountersRsp;
		openavb_aecp_command_data_get_memory_object_length_t getMemoryObjectLengthCmd;
		openavb_aecp_response_data_get_memory_object_length_t getMemoryObjectLengthRsp;
		openavb_aecp_commandresponse_data_get_dynamic_info_t getDynamicInfoCmd;
		openavb_aecp_commandresponse_data_get_dynamic_info_t getDynamicInfoRsp;
		openavb_aecp_mvu_commandresponse_data_get_milan_info_t getMilanInfoCmd;
		openavb_aecp_mvu_response_data_get_milan_info_t getMilanInfoRsp;
		openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t setSystemUniqueIdCmd;
		openavb_aecp_mvu_commandresponse_data_set_system_unique_id_t setSystemUniqueIdRsp;
		openavb_aecp_mvu_commandresponse_data_get_system_unique_id_t getSystemUniqueIdCmd;
		openavb_aecp_mvu_response_data_get_system_unique_id_t getSystemUniqueIdRsp;
		openavb_aecp_mvu_commandresponse_data_get_media_clock_reference_info_t getMediaClockReferenceInfoCmd;
		openavb_aecp_mvu_response_data_get_media_clock_reference_info_t getMediaClockReferenceInfoRsp;
		openavb_aecp_mvu_commandresponse_data_bind_stream_t bindStreamCmd;
		openavb_aecp_mvu_commandresponse_data_bind_stream_t bindStreamRsp;
		openavb_aecp_mvu_commandresponse_data_unbind_stream_t unbindStreamCmd;
		openavb_aecp_mvu_commandresponse_data_unbind_stream_t unbindStreamRsp;
		openavb_aecp_mvu_commandresponse_data_unbind_stream_t getStreamInputInfoExCmd;
		openavb_aecp_mvu_response_data_get_stream_input_info_ex_t getStreamInputInfoExRsp;
	} command_data;
} openavb_aecp_entity_model_data_unit_t;

typedef struct {
	openavb_aecp_common_data_unit_t commonPdu;
	openavb_aecp_entity_model_data_unit_t entityModelPdu;
} openavb_aecp_data_unit_t;

// IEEE Std 1722.1-2013 clause 9.2.2.3.1.1.1
typedef struct {
	U8 host[ETH_ALEN];
	openavb_aecp_control_header_t headers;
	openavb_aecp_common_data_unit_t commonPdu;
	U8 protocol_id[OPENAVB_AECP_MVU_PROTOCOL_ID_LENGTH];
	openavb_aecp_entity_model_data_unit_t entityModelPdu;
} openavb_aecp_AEMCommandResponse_t;

// IEEE Std 1722.1-2013 clause 9.2.2.3.1.2.3
typedef struct {
	U8 myEntityID[8];
} openavb_aecp_sm_global_vars_t;

openavbRC openavbAecpStart(void);
void openavbAecpStop(void);

#endif // OPENAVB_AECP_H
