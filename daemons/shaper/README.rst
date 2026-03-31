OpenAvnu Traffic Shaping Daemon
===============================

.. contents::
..
   1  Introduction
   2  Support
   3  Future Updates

Introduction
------------

The shaper daemon is an interface to use the tc (Traffic Control) command to
configure kernel traffic shaping with IEEE 802.1Qav Credit Based Shaping (CBS)
for SR Class A/B traffic. While tc could be called directly, using the daemon
allows for a simpler interface and keeps track of the current traffic shaping
configurations in use.

Support
-------

To enable the Shaper daemon support for AVTP Pipeline, edit the endpoint.ini
file and uncomment the port=15365 line of the shaper section.

The Shaper daemon does not work (i.e. no shaping occurs) if the Intel IGB
support is loaded for the adapter being used.  You may also need to compile
the AVTP Pipeline with PLATFORM_TOOLCHAIN=generic to not include the IGB
features.

TSN Notes
---------

CBS shaping requires a link speed to compute the shaper parameters. Provide
`-l <Mbps>` on the first reserve command or set `SHAPER_LINK_SPEED_MBPS` in the
environment.

This daemon now programs CBS shapers per class and does not install per-stream
MAC address filters. Stream destination addresses are used for accounting
(reserve/unreserve) only. Classification into Class A/B queues should be done
by socket priority / VLAN PCP mapping in the host configuration.

If your system already configures a TSN root qdisc (for example TAPRIO),
set `SHAPER_SKIP_ROOT_QDISC=1` so the daemon does not install its own `mqprio`
root qdisc. In that case, you are responsible for mapping priorities to the
traffic classes that correspond to Class A/B queues.

Alternatively, you can have the daemon install a TAPRIO root qdisc by setting
`SHAPER_TAPRIO_CMD` to the arguments that follow `tc qdisc add dev <if> root`.
Example:

`SHAPER_TAPRIO_CMD="handle 100: taprio num_tc 4 map 3 3 1 0 2 2 2 2 2 2 2 2 2 2 2 2 queues 1@0 1@1 1@2 1@3 base-time 0 sched-entry S 0x8 1000000 clockid CLOCK_TAI"`

When integrating with a non-default root qdisc, you can override the CBS
attach points and handles via:

- `SHAPER_CLASSA_PARENT` (default `1:5`)
- `SHAPER_CLASSB_PARENT` (default `1:6`)
- `SHAPER_CLASSA_HANDLE` (default `2`)
- `SHAPER_CLASSB_HANDLE` (default `3`)

Future Updates
--------------

- Have the daemon verify that tc is installed
- Have the daemon verify that the kernel is configured to support Hierarchy
  Token Bucket traffic shaping
- Add a method to interlace frames from multiple streams of the same class
  (perhaps using multiple layers of queues)
- Add updates to support IEEE 802.1Qcc configurable classes
