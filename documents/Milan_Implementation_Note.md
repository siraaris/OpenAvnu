# OpenAvnu Milan 1.3 Implementation Note

## Scope

This branch brings OpenAvnu substantially closer to non-redundant Milan 1.3 interoperability.

Implemented areas:

- gPTP timing model exposure in the AEM tree
- Milan MVU support for non-redundant listener binding
- `LOCK_ENTITY`, controller registration, unsolicited notifications
- Milan `IDENTIFY` control with configurable timeout
- `GET_DYNAMIC_INFO`, `GET_STREAM_INPUT_INFO_EX`, `GET_MILAN_INFO`
- stream input/output counters needed by Milan controllers
- dynamic audio mapping for non-redundant listener inputs
- CRF integration for shared clock-domain modeling
- external-controller interoperability with Hive and Milan endpoints

Out of scope:

- Milan redundancy
- Automotive profile behavior
- full persistence/recovery semantics beyond the current save-state path

## Main OpenAvnu Changes

### Entity Model / AEM

- Added `TIMING`, `PTP_INSTANCE`, and `PTP_PORT` descriptors.
- Reworked descriptor construction so non-CRF audio streams, CRF streams, and shared clock domains present a Milan-usable device model.
- Added a valid primary `IDENTIFY` control and corrected descriptor ordering / descriptor validity issues flagged by Hive.

### AECP / MVU / Dynamic State

- Added Milan MVU command handling for:
  - `GET_MILAN_INFO`
  - `GET_SYSTEM_UNIQUE_ID`
  - `GET_MEDIA_CLOCK_REFERENCE_INFO`
  - `BIND_STREAM`
  - `UNBIND_STREAM`
  - `GET_STREAM_INPUT_INFO_EX`
- Added `GET_DYNAMIC_INFO` handling used by Milan controllers.
- Added queued unsolicited notifications for AEM and MVU updates.
- Tightened stale controller acquire/lock cleanup.

### Stream Control / Runtime

- Listener MVU bind now drives normal ACMP listener connect/disconnect behavior.
- Listener-side runtime state is propagated back through `avdecc_msg` so Milan readback reflects actual stream state.
- Audio map add/remove/get flows are implemented for non-redundant listener inputs.
- Stream input/output counters and mandatory AVB interface counters are surfaced to controllers.

### Test Profiles

- `STACK_PROFILE=wav`
  - 4x tonegen Milan talkers
  - CRF talker + listener
  - 8-channel AAF WAV listener sink
- `STACK_PROFILE=custom`
  - supports null talker/listener smoke testing via explicit `INI_FILES`

## Validation Performed

Validated during this implementation:

- Hive compatibility faults were iteratively cleared until the entity enumerated without Milan compatibility errors.
- Hive showed Milan listener `Binding State` instead of legacy `Connection State`.
- External Milan bind/unbind was validated against an RME device for:
  - AAF media stream
  - CRF media clock stream
- Listener media receipt was confirmed from OpenAvnu host logs and WAV capture output.
- Hive matrix state was corrected to show the Milan media-locked dot for successful external binds.

## Known Remaining Limitations

- Redundancy is not implemented.
- The current work is listener-focused for Milan MVU binding; broader talker-side Milan feature coverage is still limited.
- Runtime behavior has been exercised primarily through Hive and real-device interop, not a formal automated Milan conformance suite.
- Some broader OpenAvnu tree changes in this branch are unrelated to Milan and should be reviewed separately before upstreaming.
