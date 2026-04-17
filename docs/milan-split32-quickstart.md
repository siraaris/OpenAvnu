# Milan Split32 Quick Start

## Overview

This guide brings up the current OpenAvnu Milan stack in the `split32` profile:

- one 32-channel ALSA source
- four 8-channel AAF Milan talker streams
- one Milan CRF talker
- one local CRF listener for Milan compliance

The talker host in this setup uses an Intel `I210-T1` PCIe network interface for Milan traffic.

Typical topology:

- OpenAvnu Linux talker host with an Intel `I210-T1`
- one Milan listener device, for example a DAC
- one Hive controller instance

You can wire this in either of these ways:

- Recommended: connect the Linux host, the Milan listener, and the Hive controller to the same AVB/Milan-capable switch.
- Simple lab bring-up: connect the Linux host directly to the Milan listener. If you do this, the Hive controller must still be on the same Layer-2 segment. In practice that usually means running Hive on the talker host itself, or adding a small switch so the controller can see both devices.

It assumes:

- a Linux talker host with the OpenAvnu `milan` branch
- a sibling `gptp` checkout on the `milan` branch
- a Milan-capable listener device or DAC on the same network
- Hive or another Milan-capable controller on the same network

The current launcher is split into two layers:

- `INFRA`: NIC tuning, `gPTP`, `phc2sys`, `MRPD`, `MAAP`, `shaper`, and the seeded `tc`/queueing setup
- `STREAM`: `openavb_host` and `openavb_avdecc`

In normal use:

- `start` brings up `STREAM`, and seeds `INFRA` first if needed
- `stop` stops `STREAM` only and leaves `INFRA` running
- `infra-stop` tears down both `STREAM` and `INFRA`

For remaining gaps and follow-up work, see [milan-roadmap.md](./milan-roadmap.md).

## 1. Install host packages

Install the common build and runtime packages first:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git tmux alsa-utils \
  libpcap-dev libpci-dev libsndfile1-dev libjack-dev \
  libglib2.0-dev libasound2-dev linuxptp ethtool
```

## 2. Set up ALSA loopback and `/etc/asound.conf`

The `split32` profile expects a 32-channel ALSA capture device named `bus32_cap`. The simplest way to provide that is `snd-aloop`.

Load the loopback device:

```bash
sudo modprobe snd-aloop
aplay -l
arecord -l
```

You should see an ALSA card named `Loopback`.

Create `/etc/asound.conf` like this:

```conf
pcm.bus32_play_hw {
    type hw
    card Loopback
    device 0
    subdevice 0
}

pcm.bus32_cap_hw {
    type hw
    card Loopback
    device 1
    subdevice 0
}

pcm.bus32_play {
    type plug
    slave.pcm "bus32_play_hw"
    slave.rate 96000
    slave.channels 32
    slave.format S32_LE
}

pcm.bus32_cap {
    type plug
    slave.pcm "bus32_cap_hw"
    slave.rate 96000
    slave.channels 32
    slave.format S32_LE
}
```

With this setup:

- your source application writes 32-channel, 96 kHz, `S32_LE` audio to `bus32_play`
- OpenAvnu captures the same stream from `bus32_cap`

You can sanity-check it with any tool or DAW that can open a 32-channel ALSA PCM.

## 3. Use the stock Intel I210-T1 driver

This setup assumes the Milan-facing NIC is an Intel `I210-T1` PCIe adapter using the stock kernel `igb` driver. A custom DKMS driver is not required.

Verify the interface is bound to the stock driver:

```bash
ethtool -i enp2s0
```

You want to see:

```text
driver: igb
```

## 4. Prepare the network interface

Use the Intel I210 Milan interface you intend to stream on, for example `enp2s0`. The current Milan launcher defaults expect:

- daemon/control traffic on `enp2s0`
- talker traffic on `sendmmsg:enp2s0`
- PTP hardware clock at `/dev/ptp0`

At minimum, verify:

```bash
ip link show enp2s0
ethtool -T enp2s0
ls -l /dev/ptp0
```

The launcher will apply the runtime NIC tuning automatically at startup:

- disable EEE
- disable coalescing
- set ring size to 512/512
- disable TSO/GSO/GRO/LRO/VLAN offloads
- disable pause frames
- set combined queues to 4

## 5. Clone both repos as siblings

Clone the `milan` branches so the launcher finds `../gptp` automatically:

```bash
mkdir -p "$HOME/src"
cd "$HOME/src"
git clone -b milan git@github.com:siraaris/OpenAvnu.git
git clone -b milan git@github.com:siraaris/gptp.git
```

The resulting layout should look like:

```text
$HOME/src/
  OpenAvnu/
  gptp/
```

## 6. Build `gptp` and OpenAvnu

Build `gptp`:

```bash
cd gptp
make -C linux/build -j"$(nproc)"
```

Build the OpenAvnu pieces used by the Milan launcher:

```bash
cd ../OpenAvnu
make daemons_all mrp_client avtp_pipeline avtp_avdecc -j"$(nproc)"
```

That should produce the binaries used by `run_milan_stack_tmux.sh`:

- `gptp/linux/build/obj/daemon_cl`
- `daemons/mrpd/mrpd`
- `daemons/maap/linux/build/maap_daemon`
- `daemons/shaper/shaper_daemon`
- `lib/avtp_pipeline/build/platform/Linux/avb_host/openavb_host`
- `lib/avtp_pipeline/build_avdecc/platform/Linux/avb_avdecc/openavb_avdecc`

## 7. Start the split32 Milan stack

From the OpenAvnu repo:

```bash
cd ~/path/to/OpenAvnu
sudo ./run_milan_stack_tmux.sh infra-stop
sudo STACK_PROFILE=split32 ./run_milan_stack_tmux.sh start
```

Using `infra-stop` first is recommended for:

- the first bring-up after boot
- switching NIC/clock/shaper settings
- changing the active Milan profile
- recovering from a bad or partial previous run

Useful follow-up commands:

```bash
sudo ./run_milan_stack_tmux.sh status
sudo ./run_milan_stack_tmux.sh logs
sudo ./run_milan_stack_tmux.sh stop
sudo ./run_milan_stack_tmux.sh infra-stop
```

On a clean bring-up, the launcher seeds `INFRA` and then starts `STREAM`.

`INFRA` contains:

- gPTP
- phc2sys
- MRPD
- MAAP
- shaper

`STREAM` contains:

- `openavb_host`
- `openavb_avdecc`

For `split32`, it also starts:

- four 8-channel AAF Milan talkers
- one CRF talker
- one CRF listener

After that initial bring-up:

- `sudo ./run_milan_stack_tmux.sh stop` stops only `openavb_host` and `openavb_avdecc`
- `sudo ./run_milan_stack_tmux.sh start` starts them again on top of the existing infrastructure
- `sudo ./run_milan_stack_tmux.sh infra-stop` is the full teardown

## 8. Feed 32-channel audio into the stack

Start your 32-channel source and point it at `bus32_play`. The source must match the Milan profile:

- 32 channels
- 96 kHz
- 32-bit little-endian samples

Examples:

- a DAW or audio engine writing to the ALSA device `bus32_play`
- a custom test app writing to `bus32_play`
- any ALSA-capable generator/player that can output 32 channels at `96k/S32_LE`

## 9. Connect streams in Hive to the Milan listener

Put the talker host, the Milan DAC/listener, and the Hive controller on the same Milan network.

In Hive:

1. Wait for the OpenAvnu talker entity to appear. The current default entity name is `3SB Audio Processor`.
2. Confirm the listener/DAC is visible and locked to the network clock.
3. Locate the four talker stream outputs from the OpenAvnu device.
4. Connect those four 8-channel AAF streams to the appropriate 8-channel listener inputs on the DAC.
5. Apply/start streaming from the controller.

Notes:

- The OpenAvnu `split32` profile is a 4 x 8-channel, 96 kHz AAF talker. The listener device must support that stream format and channel count.
- The CRF streams are part of the Milan timing model; the launcher starts the needed CRF endpoints automatically.

## 10. Quick sanity checks

If the stack starts but you do not get audio:

- confirm `sudo ./run_milan_stack_tmux.sh status` shows all core components `proc=up`
- check `/var/log/3sb/openavnu/daemon_cl.log`
- check `/var/log/3sb/openavnu/phc2sys.log`
- check `/var/log/3sb/openavnu/openavb_host.log`
- confirm the source is really writing 32-channel `96k/S32_LE` audio into `bus32_play`
- confirm Hive shows the talker streams and the listener connections as active

This quick-start stays at the working-profile level. If you need to change interface names, PTP device, or INI paths, start with `run_milan_stack_tmux.sh` and the Milan INIs under `test_configs/milan/` and `lib/avtp_pipeline/platform/Linux/intf_bus32split/`.
