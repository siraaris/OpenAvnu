# Milan Roadmap / TODO

This note tracks the main Milan work that is still incomplete or only partially validated.

## Current State

- `split32` is the primary validated profile.
- The launcher now uses an `INFRA` / `STREAM` split:
  - `INFRA`: NIC tuning, `gPTP`, `phc2sys`, `MRPD`, `MAAP`, `shaper`, and seeded `tc`
  - `STREAM`: `openavb_host` and `openavb_avdecc`
- The stack now runs on the stock Intel `igb` driver for the `I210-T1`.
- `split32` AAF streams are `96 kHz`.
- `split32` CRF is currently `48 kHz`.

## Open Items

1. Validate and migrate the non-`split32` profiles.

The `tonegen`, `wav`, and `crf` profiles in [run_milan_stack_tmux.sh](../run_milan_stack_tmux.sh) still need a full pass under the current launcher model:

- confirm they work cleanly with the persistent `INFRA` / `STREAM` split
- confirm they are using the `sendmmsg:` host interface path consistently
- update and test them against the current intended rate plan

Current intended direction:

- audio talker streams at `96 kHz`
- CRF at `48 kHz`

2. Finish removing hard-coded `stream_addr` / `dest_addr` assumptions.

This is still not fully generalized across all Milan configs.

Current status:

- the `split32` AAF talkers and split32 CRF talker now use unresolved/commented address placeholders rather than fixed local values
- some listener and non-`split32` configs still carry fixed or semi-fixed assumptions

Known examples:

- [crf_listener_milan.ini](../test_configs/milan/crf_listener_milan.ini) still has an explicit `dest_addr`
- non-`split32` profiles still need end-to-end validation with unresolved `stream_addr` / `dest_addr`

Follow-up work:

- rely on MAAP for talker `dest_addr` where appropriate
- rely on controller/ACMP binding for listener `stream_addr` where appropriate
- keep repo-tracked Milan configs generic rather than tied to one host or one lab setup

3. Reduce remaining host-specific defaults.

The stack is still opinionated about the local environment in a few places, for example:

- `enp2s0`
- `sendmmsg:enp2s0`
- `/dev/ptp0`

Those defaults are workable, but we should continue moving toward cleaner override points and better documentation for alternate interface names.

4. Recheck secondary profile documentation.

The quick-start is now focused on `split32`. Once the other profiles are updated and validated, add short profile-specific notes for:

- `tonegen`
- `wav`
- `crf`
