#ifndef OPENAVB_AVB32_DIRECT_ABI_H
#define OPENAVB_AVB32_DIRECT_ABI_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
typedef __u8 openavb_avb32_direct_u8_t;
typedef __u32 openavb_avb32_direct_u32_t;
typedef __u64 openavb_avb32_direct_u64_t;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t openavb_avb32_direct_u8_t;
typedef uint32_t openavb_avb32_direct_u32_t;
typedef uint64_t openavb_avb32_direct_u64_t;
#endif

#define OPENAVB_AVB32_DIRECT_ABI_MAGIC   0x41564233u
#define OPENAVB_AVB32_DIRECT_ABI_VERSION 3u

#define OPENAVB_AVB32_DIRECT_CHANNELS          32u
#define OPENAVB_AVB32_DIRECT_BYTES_PER_SAMPLE   4u
#define OPENAVB_AVB32_DIRECT_DEFAULT_RATE   96000u
#define OPENAVB_AVB32_DIRECT_DEFAULT_RING_FRAMES 4096u
#define OPENAVB_AVB32_DIRECT_KERNEL_RING_FRAMES 32768u

#define OPENAVB_AVB32_DIRECT_MEDIA_TIME_VALID   0x00000001u
#define OPENAVB_AVB32_DIRECT_MEDIA_TIME_TAI     0x00000002u
#define OPENAVB_AVB32_DIRECT_MEDIA_TIME_RUNNING 0x00000004u

typedef struct openavb_avb32_direct_shm {
	openavb_avb32_direct_u32_t magic;
	openavb_avb32_direct_u32_t version;
	openavb_avb32_direct_u32_t header_bytes;
	openavb_avb32_direct_u32_t channel_count;
	openavb_avb32_direct_u32_t bytes_per_sample;
	openavb_avb32_direct_u32_t sample_rate;
	openavb_avb32_direct_u32_t ring_frames;
	openavb_avb32_direct_u32_t writer_period_frames;
	openavb_avb32_direct_u32_t flags;
	openavb_avb32_direct_u32_t reserved0;
	openavb_avb32_direct_u64_t frame0_walltime_ns;
	volatile openavb_avb32_direct_u64_t committed_frames;
	volatile openavb_avb32_direct_u64_t presented_frames;
	volatile openavb_avb32_direct_u64_t overrun_count;
	volatile openavb_avb32_direct_u64_t underrun_count;
	volatile openavb_avb32_direct_u64_t writer_generation;
	volatile openavb_avb32_direct_u64_t writer_walltime_ns;
	volatile openavb_avb32_direct_u64_t writer_monotonic_ns;
	openavb_avb32_direct_u64_t reserved1;
	openavb_avb32_direct_u64_t media_time_flags;
	openavb_avb32_direct_u64_t media_time_epoch;
	openavb_avb32_direct_u64_t media_time_anchor_frames;
	openavb_avb32_direct_u64_t media_time_anchor_tai_ns;
	openavb_avb32_direct_u64_t media_time_presentation_offset_ns;
	openavb_avb32_direct_u8_t audio_data[];
} openavb_avb32_direct_shm_t;

static inline size_t openavbAvb32DirectFrameBytes(void)
{
	return (size_t)OPENAVB_AVB32_DIRECT_CHANNELS * (size_t)OPENAVB_AVB32_DIRECT_BYTES_PER_SAMPLE;
}

static inline size_t openavbAvb32DirectShmBytes(uint32_t ringFrames)
{
	return sizeof(openavb_avb32_direct_shm_t)
		+ ((size_t)ringFrames * openavbAvb32DirectFrameBytes());
}

#endif
