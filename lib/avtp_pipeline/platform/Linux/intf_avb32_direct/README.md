# AVB32 Direct Interface

This interface is the consumer side of a new direct 32-channel playback path.

Instead of:

- capturing from ALSA Loopback
- splitting capture into 4 talker rings
- reconstructing capture timestamps afterward

it expects a writer to publish one shared `32ch / 96k / S32LE` timeline into shared memory.

Each talker instance:

- selects one `8ch` slice via `intf_nv_stream_index`
- timestamps items from the shared frame timeline
- pushes directly into `media_q`

Important:

- this does not yet provide the writer/playback-device side
- current live profiles do not use this path yet
- the shared ABI is sourced from [thirdparty/3SBAVBABI/include/openavb_avb32_direct_abi.h](/home/aris/src/3SBController/3rdparty/OpenAvnuMilan/thirdparty/3SBAVBABI/include/openavb_avb32_direct_abi.h)
