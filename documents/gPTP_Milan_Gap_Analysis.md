# Milan gPTP Gap Analysis for `/root/src/gptp`

## Scope

This note assesses the local gPTP implementation in `/root/src/gptp` against the Milan 1.3 requirements that materially affect OpenAvnu integration:

- Milan 4.2.6 `gPTP`
- Milan 7.5 `gPTP as Media Clock Source`
- Milan 8.3.4 `gPTP` for redundancy

It also checks the current IPC contract between `/root/src/gptp` and `/root/src/OpenAvnu`, because Milan compliance for OpenAvnu depends on both sides being able to represent the required timing state.

## Executive Summary

The `/root/src/gptp` tree is a usable starting point for non-redundant Milan, but it is not Milan-ready as-is.

What is already useful:

- The non-automotive path has Milan-aligned default message intervals: Sync `125 ms`, Announce `1 s`, Pdelay `1 s` (`/root/src/gptp/common/ether_port.cpp:120-127`).
- The daemon default `priority1` is `248`, which matches Milan's grandmaster-capable default (`/root/src/gptp/linux/src/daemon_cl.cpp:145`, `/root/src/gptp/common/ieee1588clock.cpp:85-99`).
- `neighborPropDelayThresh` defaults to `800 ns` and is configurable (`/root/src/gptp/linux/src/daemon_cl.cpp:198-201`, `/root/src/gptp/gptp_cfg.ini:17-34`).
- gPTP frames are hard-wired to `transportSpecific = 1` on transmit and reject other values on receive (`/root/src/gptp/common/ptp_message.cpp:169-172`, `/root/src/gptp/common/ptp_message.cpp:608-619`).
- The implementation does handle the Milan multiple-Pdelay-response storm case by halting Pdelay for five minutes after three successive duplicates from different sources (`/root/src/gptp/common/ptp_message.cpp:1445-1464`).
- The IPC model exports most of the raw fields OpenAvnu needs for one AVB interface: current grandmaster ID/domain, interface clock identity, priority values, clock quality, intervals, port number, counters, `asCapable`, and port state (`/root/src/gptp/common/ipcdef.hpp:68-100`, `/root/src/gptp/linux/src/linux_hal_common.cpp:963-1055`).

Main blockers:

- `asCapable` handling does not satisfy Milan 4.2.6.2.4.
- `gptp_cfg.ini` contains Milan-relevant knobs that are parsed and logged but not actually applied.
- The AVnu Automotive Profile path is not a Milan path; it disables BMCA behavior and forces fixed master/slave roles.
- The IPC architecture is single-instance and hard-wired to one shared memory name, which blocks a clean Milan 8.3.4 redundant design.
- The OpenAvnu-side consumer struct for the gPTP shared memory is stale and no longer matches the producer.

Net: for non-redundant Milan, `/root/src/gptp` can probably be adapted with targeted fixes. For redundant Milan, the current IPC and process model need redesign.

## What Already Aligns Reasonably Well

### Non-AP defaults are close to Milan 4.2.6

The non-automotive path initializes:

- Sync interval to `-3` (`125 ms`)
- Pdelay interval to `0` (`1 s`)
- operational Sync interval to `0` only if explicitly driven into the AP signaling flow

See `/root/src/gptp/common/ether_port.cpp:116-127`.

This is much closer to Milan 4.2.6 than the AVnu Automotive Profile path. If Milan support is the target, the non-AP path should be treated as the baseline.

### Priority1 default is already Milan-friendly

The daemon default `priority1` is `248` (`/root/src/gptp/linux/src/daemon_cl.cpp:145`), and the clock object uses that value when constructed (`/root/src/gptp/linux/src/daemon_cl.cpp:410-412`, `/root/src/gptp/common/ieee1588clock.cpp:80-99`).

That matches Milan 4.2.6.2.1 for a grandmaster-capable PAAD.

### Multiple-Pdelay-response mitigation is present

Milan 4.2.6.2.5 requires Pdelay transmission to stop after three successive responses from multiple clock identities and resume after a triggering condition or five minutes. The implementation already has the five-minute backoff logic:

- duplicate detection and counter: `/root/src/gptp/common/ptp_message.cpp:1445-1464`
- five-minute restart timer: `/root/src/gptp/common/ptp_message.cpp:1460-1463`

### IPC exports the right kind of state for one interface

For a single-interface, non-redundant device, the producer side exports:

- grandmaster ID and domain
- local interface clock identity
- priority1, priority2, clock class, clock accuracy, offset scaled log variance
- current log Sync/Announce/Pdelay intervals
- port number
- sync/pdelay counters
- `asCapable`
- port state

See `/root/src/gptp/common/ipcdef.hpp:68-100` and `/root/src/gptp/linux/src/linux_hal_common.cpp:963-1055`.

That is sufficient raw data for OpenAvnu to populate Milan-oriented timing descriptors later, but only for one gPTP instance.

## Gaps and Risks

### 1. `asCapable` behavior is not Milan-compliant

Milan 4.2.6.2.4 requires `asCapable` to become `TRUE` only after no less than 2 and no more than 5 successful Pdelay response/follow-up pairs.

Current behavior is wrong in both paths:

- In Automotive Profile mode, the port is marked `asCapable` immediately in the constructor, before any Pdelay exchange (`/root/src/gptp/common/ether_port.cpp:104-116`).
- On Automotive Profile link-up, it is set `TRUE` again immediately (`/root/src/gptp/common/ether_port.cpp:427-429`).
- In the non-AP path, a single acceptable link delay causes `asCapable` to become `TRUE` immediately (`/root/src/gptp/common/ptp_message.cpp:1818-1830`).

This is a real Milan gap, not just a documentation issue.

### 2. `gptp_cfg.ini` has Milan-relevant fields that are parsed but ignored

The config file advertises:

- `priority1`
- `announceReceiptTimeout`
- `syncReceiptTimeout`

See `/root/src/gptp/gptp_cfg.ini:2-34`.

The daemon parses and logs those values (`/root/src/gptp/linux/src/daemon_cl.cpp:434-446`), but:

- the clock is constructed before the config file is parsed (`/root/src/gptp/linux/src/daemon_cl.cpp:410-412`, `/root/src/gptp/linux/src/daemon_cl.cpp:432-468`)
- `priority1` is never updated from the parsed config
- `announceReceiptTimeout` is never applied anywhere
- `syncReceiptTimeout` is never applied anywhere

The actual timeout paths are still hardcoded from interval-derived multipliers:

- timeout multipliers defined here: `/root/src/gptp/common/common_port.hpp:48-50`
- sync receipt timer restarts here: `/root/src/gptp/common/ptp_message.cpp:1157-1162`, `/root/src/gptp/common/ether_port.cpp:384-387`, `/root/src/gptp/common/ether_port.cpp:454-457`, `/root/src/gptp/common/ether_port.cpp:511-514`, `/root/src/gptp/common/ether_port.cpp:706-709`
- announce receipt timer paths here: `/root/src/gptp/common/common_port.cpp:573-574`, `/root/src/gptp/common/ptp_message.cpp:881-884`, `/root/src/gptp/common/ether_port.cpp:419-424`

So the current config surface is misleading. For Milan qualification, that is risky because operators can believe they are enforcing receipt-timeout behavior when they are not.

### 3. The AVnu Automotive Profile mode is not a Milan mode

The repo explicitly documents Automotive Profile behavior, including BMCA disablement, signaling-based interval changes, and fixed GM/slave operation (`/root/src/gptp/README_AVNU_AP.txt:5-31`, `/root/src/gptp/README_AVNU_AP.txt:43-50`).

In code, the Automotive Profile path forces the port state to `MASTER` or `SLAVE` when `-V` is used (`/root/src/gptp/linux/src/daemon_cl.cpp:489-500`).

It also sets AP-specific interval behavior:

- initial Sync interval `-5` (`31.25 ms`)
- operational Sync interval default `0` (`1 s`)

See `/root/src/gptp/common/ether_port.cpp:104-115`.

That is not the Milan 4.2.6 default timing model. Milan needs BTCA support; the AP path is a different profile and should not be treated as the Milan baseline.

### 4. Single-instance IPC blocks a clean Milan 8.3.4 redundancy implementation

Milan 8.3.4 requires two independent gPTP time-aware end stations on the primary and secondary AVB interfaces.

The current architecture is single-instance oriented:

- the daemon creates one `EtherPort` for one command-line network interface
- it sets a single port index (`portInit.index = 1`) (`/root/src/gptp/linux/src/daemon_cl.cpp:423-425`)
- the producer shared memory name is fixed to `/ptp` (`/root/src/gptp/linux/src/linux_ipc.hpp:39-40`)
- the "group" argument changes ownership, not the shared memory namespace (`/root/src/gptp/linux/src/linux_hal_common.cpp:896-925`)
- the producer always unlinks that single shared memory object on shutdown (`/root/src/gptp/linux/src/linux_hal_common.cpp:891-893`)

This means the current daemon/OpenAvnu contract does not naturally represent two simultaneous gPTP instances with separate timing state. That is a hard architectural blocker for redundant Milan.

### 5. OpenAvnu's gPTP shared-memory consumer is stale

This is technically an OpenAvnu-side issue, but it materially affects whether `/root/src/gptp` can be consumed correctly.

Producer side:

- shared memory size is `sizeof(gPtpTimeData) + sizeof(pthread_mutex_t)` (`/root/src/gptp/linux/src/linux_ipc.hpp:39`)
- producer struct includes counters, `asCapable`, port state, and process ID (`/root/src/gptp/common/ipcdef.hpp:94-100`)

OpenAvnu consumer side:

- still maps `SHM_NAME "/ptp"` (`/root/src/OpenAvnu/lib/common/avb_gptp.h:6-7`)
- uses an obsolete `SHM_SIZE` macro (`/root/src/OpenAvnu/lib/common/avb_gptp.h:6`)
- consumer struct stops at `port_number` and does not include the Linux-specific tail (`/root/src/OpenAvnu/lib/common/avb_gptp.h:11-36`)
- `gptpinit()` maps only that smaller size (`/root/src/OpenAvnu/lib/common/avb_gptp.c:24-37`)
- `gptpgetdata()` copies `sizeof(*td)` from shared memory (`/root/src/OpenAvnu/lib/common/avb_gptp.c:79-86`)

At minimum this means OpenAvnu cannot consume the producer's extra per-port state. Depending on ABI/layout, it also creates a real shared-memory size mismatch risk.

### 6. Some Milan timing requirements are only partially covered

Two specific areas still look incomplete:

- I did not find any explicit `followUpReceiptTimeout` timer path; Sync receipt timeout is implemented, but not a dedicated Milan-style follow-up receipt watchdog.
- Pdelay turnaround time is computed but not checked against a Milan limit or used to implement late-response handling (`/root/src/gptp/common/ptp_message.cpp:1327-1345`).

This needs a deeper protocol-level review before claiming Milan 4.2.6.2.3 or 4.2.6.2.6 behavior.

## Milan 7.5 Assessment

Milan 7.5 is mostly an OpenAvnu entity-model problem, not a gPTP-daemon problem.

From the gPTP side, the important point is that the daemon already exports enough single-interface state to back:

- `PTP_INSTANCE.clock_identity`
- `PTP_PORT.avb_interface_index`
- AVB interface dynamic timing fields

The missing part is that OpenAvnu does not yet model `TIMING`, `PTP_INSTANCE`, and `PTP_PORT` descriptors correctly, and the current IPC model only represents one interface cleanly.

So for Milan 7.5:

- `/root/src/gptp` is not the main blocker for non-redundant devices
- `/root/src/gptp` is a blocker for redundant devices because it is not exposed as two independent timing endpoints

## Recommended Work

### Priority 1

- Do not build Milan on the Automotive Profile path. Treat non-AP gPTP as the baseline.
- Fix `asCapable` so it transitions to `TRUE` only after 2 to 5 successful Pdelay response/follow-up pairs.
- Make `priority1`, `announceReceiptTimeout`, and `syncReceiptTimeout` either actually apply or remove them from `gptp_cfg.ini`.

### Priority 2

- Redesign the IPC namespace so multiple gPTP instances can coexist without sharing the same `/ptp` object.
- Update the OpenAvnu consumer-side shared-memory struct and size definitions to match the producer exactly.
- Expose per-interface gPTP state in a way that OpenAvnu can bind to AVB interface index 0 and 1 separately.

### Priority 3

- Audit follow-up receipt timeout handling against Milan 4.2.6.2.3.
- Audit Pdelay turnaround-time behavior against Milan 4.2.6.2.6.
- Once the IPC model is fixed, wire the gPTP data into OpenAvnu's future `TIMING`, `PTP_INSTANCE`, and `PTP_PORT` descriptors.

## Bottom Line

For non-redundant Milan, `/root/src/gptp` is salvageable and already has several Milan-friendly defaults in the non-AP path.

For redundant Milan, the current design is not sufficient. The fixed single shared-memory endpoint and single-interface consumption model mean OpenAvnu cannot cleanly represent the two independent gPTP time-aware end stations required by Milan 8.3.4 without redesign.
