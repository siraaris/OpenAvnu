# bus32split interface

`openavbIntfBus32SplitInitialize` is a talker-side interface that captures one
interleaved 32-channel ALSA stream and fans it out into four interleaved
8-channel AVB streams.

## Design

- One shared ALSA capture handle (`snd_pcm_t`) for all four streams.
- One shared capture thread.
- Four per-stream rings (stream indexes `0..3`).
- Each stream pulls only its 8-channel lane from the shared capture source.

## Required INI keys per stream

- `intf_fn = openavbIntfBus32SplitInitialize`
- `intf_nv_device_name = <ALSA capture device>` (for example `bus32_cap_hw`)
- `intf_nv_stream_index = 0|1|2|3`
- `intf_nv_audio_rate = 48000`
- `intf_nv_audio_type = int|uint|float` (or numeric enum)
- `intf_nv_audio_bit_depth = 32`
- `intf_nv_audio_channels = 8` (must be 8)
- `intf_nv_audio_endian = little|big` (or numeric enum)

Optional:

- `intf_nv_allow_resampling = 0|1`
- `intf_nv_period_time = <microseconds>`
- `intf_nv_clock_skew_ppb = <signed integer>`

## Example stream set

Example Milan AAF profiles are provided in this folder:

- `bus32split_milan_0.ini`
- `bus32split_milan_1.ini`
- `bus32split_milan_2.ini`
- `bus32split_milan_3.ini`
- `bus32split_milan_crf.ini`

The four AAF talkers plus CRF are intended to be started together in one
`openavb_host` process.

Example with `run_avb_stack_tmux.sh`:

```bash
INI_FILES="\
/root/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_0.ini \
/root/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_1.ini \
/root/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_2.ini \
/root/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_3.ini \
/root/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/intf_bus32split/bus32split_milan_crf.ini" \
./run_avb_stack_tmux.sh restart
```
