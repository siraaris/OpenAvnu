# Milan 1.3 Gap Analysis for OpenAvnu

## Scope

This assessment is based on:

- [Milan_Specification_Consolidated_v1.3.pdf]($HOME/docs/Milan_Specification_Consolidated_v1.3.pdf)
- Static review of the OpenAvnu tree, primarily `lib/avtp_pipeline/` and `avdecc-lib/`

The focus is Milan conformance for AVDECC/ATDECC end stations. It does not certify runtime timing behavior on hardware, and it does not audit external projects that OpenAvnu may depend on for gPTP.

## Overall Assessment

OpenAvnu has useful Milan building blocks:

- AAF transport code includes 32-bit integer audio support in [openavb_map_aaf_audio.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/map_aaf_audio/openavb_map_aaf_audio.c#L91)
- CRF transport exists in [openavb_map_crf.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/map_crf/openavb_map_crf.c#L57)
- There is partial 1722.1-2021/Milan-oriented work around `GET_STREAM_INFO` extension flags and `GET_MAX_TRANSIT_TIME_2021`
- The tree already contains Milan-oriented example INIs and launch scripts

However, OpenAvnu is not currently close to Milan 1.3 compliance. The main blockers are not in the packet mappers; they are in the AVDECC entity model, missing Milan MVU and binding flows, incomplete dynamic state/counters/notifications, and the absence of a Milan-compliant device model.

## Existing Foundations

These parts are usable as a base for a Milan update:

- AVTP AAF and CRF data paths exist.
- Clock domain and clock source descriptors already exist at 1722.1-2013 level.
- `GET_STREAM_INFO` exposes some Milan-style extension flags.
- `GET_MAX_TRANSIT_TIME_2021` readback exists for stream outputs.
- ACMP/AECP/ADP/MAAP scaffolding is already present.

These reduce implementation risk, but they do not solve the model/control-plane gaps below.

## Gaps

### 1. AEM model shape is not Milan-compliant

Relevant Milan clauses:

- 5.3.3.10 `IDENTIFY` CONTROL
- 5.3.3.12 `TIMING`
- 5.3.3.13 `PTP_INSTANCE`
- 5.3.3.14 `PTP_PORT`
- Annex A descriptor tree examples

Findings:

- The descriptor enum stops at `CONTROL_BLOCK`; there are no `TIMING`, `PTP_INSTANCE`, or `PTP_PORT` descriptor types in [openavb_aem_types_pub.h]($HOME/src/OpenAvnu/lib/avtp_pipeline/aem/openavb_aem_types_pub.h#L46).
- The entity builder creates one configuration per stream rather than one device-centric configuration with a complete descriptor tree in [openavb_avdecc.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/avdecc/openavb_avdecc.c#L506).
- The same builder adds only one non-top-level `AUDIO_CLUSTER` and at most one stream port per role in [openavb_avdecc.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/avdecc/openavb_avdecc.c#L526).
- The builder explicitly notes missing descriptor types such as `CONTROL` and `AUDIO_MAP` in [openavb_avdecc.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/avdecc/openavb_avdecc.c#L549).
- `IDENTIFY` control support exists as descriptor code, but it is not instantiated into the entity model.

Impact:

- Controllers cannot discover a Milan-style descriptor tree.
- gPTP-as-media-clock cannot be modeled correctly.
- Audio cluster/map topology cannot match Milan Annex A examples.
- The device model exposed over AVDECC reflects individual streams, not the actual end-station.

Priority: Critical

Recommended work:

- Redesign entity-model construction around device configurations, not one configuration per stream.
- Add `TIMING`, `PTP_INSTANCE`, and `PTP_PORT` descriptors and serialization.
- Instantiate `IDENTIFY` control, stream ports, audio clusters, and audio maps per Milan profile.
- Build at least one Milan reference descriptor tree from Annex A and make the code generate that shape.

### 2. Milan MVU control plane is absent

Relevant Milan clauses:

- 5.4.3 AECP Milan Vendor Unique format
- 5.4.4 `GET_MILAN_INFO`
- 5.4.4 `SET_SYSTEM_UNIQUE_ID`
- 5.4.4 `GET_SYSTEM_UNIQUE_ID`
- 5.4.4 `SET_MEDIA_CLOCK_REFERENCE_INFO`
- 5.4.4 `GET_MEDIA_CLOCK_REFERENCE_INFO`
- 5.4.4 `BIND_STREAM`
- 5.4.4 `UNBIND_STREAM`
- 5.4.4 `GET_STREAM_INPUT_INFO_EX`

Findings:

- No MVU command or descriptor handling is present in `lib/avtp_pipeline`.
- No Milan MVU symbols are present in `avdecc-lib`; a repo-wide search for `GET_MILAN_INFO`, `BIND_STREAM`, `UNBIND_STREAM`, `GET_STREAM_INPUT_INFO_EX`, `SET_SYSTEM_UNIQUE_ID`, and media-clock-reference MVU commands returned no matches.

Impact:

- OpenAvnu cannot expose Milan specification version, certification capabilities, redundancy bit, or system unique ID.
- Milan binding and media clock reference management cannot work.
- The bundled controller library cannot test or consume Milan MVU features after they are implemented in the entity.

Priority: Critical

Recommended work:

- Add MVU packet format support in AECP.
- Implement MVU commands on the entity side.
- Extend `avdecc-lib` to parse and issue MVU commands so the shipped controller tooling remains useful.

### 3. Milan connection management model is missing

Relevant Milan clauses:

- 5.5 Connection management
- 5.5.3 Listener behavior
- 5.5.4 Talker behavior
- 5.4.4.6 `BIND_STREAM`
- 5.4.4.7 `UNBIND_STREAM`
- 5.4.4.8 `GET_STREAM_INPUT_INFO_EX`

Findings:

- OpenAvnu implements classic ACMP state machines, but not Milan binding/settlement flows.
- There is no `BIND_STREAM`, `UNBIND_STREAM`, or `GET_STREAM_INPUT_INFO_EX` handling anywhere in the entity or controller library.
- The listener ACMP state machine still contains a TODO for `STREAMING_WAIT` fast-connect handling in [openavb_acmp_sm_listener.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/acmp/openavb_acmp_sm_listener.c#L507).

Impact:

- Milan controller workflows cannot be modeled correctly.
- Bound/stopped/started/settled listener behavior is incomplete.
- Fast-connect and auto-connect interoperability will remain inconsistent even if transport works.

Priority: Critical

Recommended work:

- Add Milan binding state to `STREAM_INPUT` dynamic state.
- Implement MVU bind/unbind/info-ex commands.
- Reconcile ACMP listener/talker behavior with Milan 5.5 state machines instead of only 1722.1-2013 connect/disconnect behavior.

### 4. Locking, registration, and notification state are incomplete

Relevant Milan clauses:

- 5.3.4.1 Locked state
- 5.3.4.2 List of registered controllers
- 5.4.2.21 `REGISTER_UNSOLICITED_NOTIFICATION`
- 5.4.2.22 `DEREGISTER_UNSOLICITED_NOTIFICATION`
- 5.4.5 Notifications
- 5.4.5.4 Detection of departing controllers

Findings:

- The entity model only stores one acquired controller and one locked controller in [openavb_aem.h]($HOME/src/OpenAvnu/lib/avtp_pipeline/aem/openavb_aem.h#L73); there is no registered-controller list.
- `LOCK_ENTITY` is explicitly unimplemented in [openavb_aecp_sm_entity_model_entity.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aecp/openavb_aecp_sm_entity_model_entity.c#L184).
- `REGISTER_UNSOLICITED_NOTIFICATION` and `DEREGISTER_UNSOLICITED_NOTIFICATION` are compatibility no-ops that always return success in [openavb_aecp_sm_entity_model_entity.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aecp/openavb_aecp_sm_entity_model_entity.c#L1176).
- The unsolicited-response state exists, but it does not manage a registered-controller database or per-controller sequence numbers.
- `ACQUIRE_ENTITY` also contains an explicit TODO for `CONTROLLER_AVAILABLE` handling in [openavb_aecp_sm_entity_model_entity.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aecp/openavb_aecp_sm_entity_model_entity.c#L161).

Impact:

- Milan-required controller registration, sequence tracking, and notification delivery are absent.
- Multiple controllers cannot be supported correctly.
- Automatic cleanup of departed controllers cannot be implemented on the current state model.

Priority: Critical

Recommended work:

- Add a real registered-controller table keyed by `(entity_id, MAC, port)`.
- Track next AEM and MVU unsolicited sequence IDs per registered controller.
- Implement `LOCK_ENTITY`, controller liveness checks, and true register/deregister semantics.
- Emit unsolicited notifications from actual state changes instead of returning no-op success.

### 5. Required AEM command coverage is incomplete

Relevant Milan clauses:

- 5.4.2 AEM commands and responses
- 5.4.2.25 `GET_COUNTERS`
- 5.4.2.26 `GET_AUDIO_MAP`
- 5.4.2.27 `ADD_AUDIO_MAPPINGS`
- 5.4.2.28 `REMOVE_AUDIO_MAPPINGS`
- 5.4.2.29 `GET_DYNAMIC_INFO`
- 5.4.2.30 `SET_MAX_TRANSIT_TIME`

Findings:

- The command-code table does not define `GET_DYNAMIC_INFO` or `SET_MAX_TRANSIT_TIME`; it only has a custom alias for `GET_MAX_TRANSIT_TIME_2021` in [openavb_aem_types_pub.h]($HOME/src/OpenAvnu/lib/avtp_pipeline/aem/openavb_aem_types_pub.h#L1156).
- `ADD_AUDIO_MAPPINGS` and `REMOVE_AUDIO_MAPPINGS` are empty stubs in [openavb_aecp_sm_entity_model_entity.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aecp/openavb_aecp_sm_entity_model_entity.c#L1292).
- `GET_AUDIO_MAP` only returns counts; it does not enumerate dynamic mappings in [openavb_aecp_sm_entity_model_entity.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aecp/openavb_aecp_sm_entity_model_entity.c#L1265).
- `SET_CONFIGURATION`, `GET_CONFIGURATION`, `SET_NAME`, and `GET_NAME` are not implemented in the entity command switch.
- `GET_AS_PATH` returns a zeroed placeholder response.

Impact:

- Milan-required dynamic control and introspection commands are missing or incomplete.
- Controllers cannot configure mappings or query dynamic descriptor state in a compliant way.

Priority: High

Recommended work:

- Add missing command codes and wire them through AECP encode/decode.
- Implement `GET_DYNAMIC_INFO` and `SET_MAX_TRANSIT_TIME`.
- Implement real `GET_AUDIO_MAP` / `ADD_AUDIO_MAPPINGS` / `REMOVE_AUDIO_MAPPINGS` behavior.
- Finish `SET_NAME`, `GET_NAME`, and configuration commands where Milan requires them.

### 6. Counter reporting is far below Milan requirements

Relevant Milan clauses:

- 5.3.6.3 AVB interface diagnostic counters
- 5.3.7.7 Stream output diagnostic counters
- 5.3.8.10 Stream input diagnostic counters
- 5.3.11.2 Clock domain diagnostic counters
- 5.4.2.25 `GET_COUNTERS`

Findings:

- `GET_COUNTERS` is only meaningfully implemented for `CLOCK_DOMAIN`.
- AVB interface counters are still a TODO in [openavb_avdecc_pipeline_interaction.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/avdecc/openavb_avdecc_pipeline_interaction.c#L722).
- Stream input counters are also still a TODO in [openavb_avdecc_pipeline_interaction.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/avdecc/openavb_avdecc_pipeline_interaction.c#L749).
- There is no stream-output mandatory counter implementation for `STREAM_START`, `STREAM_STOP`, `MEDIA_RESET`, `TIMESTAMP_UNCERTAIN`, and `FRAMES_TX`.

Impact:

- Milan diagnostic state is unavailable to controllers.
- Controllers cannot reliably distinguish clocking, SRP, or stream-health failures.

Priority: High

Recommended work:

- Define persistent counter storage per AVB interface, stream input, stream output, and clock domain.
- Feed counters from SRP, AVTP RX/TX, gPTP, and endpoint state transitions.
- Expose mandatory Milan counters first; optional counters can follow.

### 7. Stream format advertisement is not Milan-complete

Relevant Milan clauses:

- 6.2 Base Format Type
- 6.3 Extended Format Types
- 6.4 Talker requirements
- 6.5 Listener requirements
- 6.6 Summary of Milan Audio Stream Formats

Findings:

- The AAF mapper supports 32-bit integer AAF in [openavb_map_aaf_audio.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/map_aaf_audio/openavb_map_aaf_audio.c#L91), which is good.
- CRF transport also exists in [openavb_map_crf.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/map_crf/openavb_map_crf.c#L57), which is also good.
- But each stream descriptor advertises exactly one format: `number_of_formats = 1` in [openavb_descriptor_stream_io.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aem/openavb_descriptor_stream_io.c#L529).
- Milan listeners that advertise a Base sampling rate must advertise all Base channel-count variants for that rate. The current model cannot do that without generating extra descriptors/configurations that do not reflect the actual device.

Impact:

- Controllers cannot infer Milan Base/XF16/XF24 support correctly.
- Listener interoperability suffers because Milan format-set semantics are stricter than “current format only”.

Priority: High

Recommended work:

- Separate “current format” from “supported Milan format set”.
- Allow stream descriptors to advertise multiple Base and Extended format variants.
- Make listener descriptors advertise the full required channel-count family for any supported Milan rate.

### 8. Media clock modeling is incomplete

Relevant Milan clauses:

- 7.2.2 Support for media clock inputs
- 7.2.3 Support for media clock outputs
- 7.3 Milan Clock Reference Format
- 7.5 gPTP as Media Clock Source
- 7.6 Media Clock Management
- 5.3.5.1 Sampling rate
- 5.3.11.1 Clock source

Findings:

- Milan requires `TIMING -> PTP_INSTANCE -> PTP_PORT` when gPTP is used as a media clock source; those descriptors are missing.
- OpenAvnu exposes `GPTP_SUPPORTED` capability but does not populate `identify_control_index` or `interface_index` in ADP in [openavb_adp.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/adp/openavb_adp.c#L139), and it has no gPTP media-clock descriptor chain.
- Milan requires current sampling rate and current clock source to be saved in non-volatile memory and restored after power cycle. The only persistence I found is listener fast-connect state in [openavb_avdecc_read_ini.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/platform/Linux/avdecc/openavb_avdecc_read_ini.c#L534).
- The current model adds clock sources opportunistically per stream, but it does not enforce Milan’s required CRF-input/output topology per supported media clock domain.

Impact:

- Media clock capabilities cannot be described correctly to controllers.
- Clock-source selection and sampling rate behavior will diverge from Milan across power cycles.

Priority: High

Recommended work:

- Implement Milan media-clock descriptors and runtime state.
- Persist sampling rate and clock-source selection independently of fast-connect state.
- Model CRF inputs/outputs as first-class media-clock resources per clock domain.

### 9. START/STOP streaming behavior conflicts with Milan

Relevant Milan clauses:

- 5.4.2.19 `START_STREAMING`
- 5.4.2.20 `STOP_STREAMING`

Findings:

- Milan says `START_STREAMING` and `STOP_STREAMING` shall be supported for `STREAM_INPUT` only, and `STREAM_OUTPUT` shall return `NOT_SUPPORTED`.
- OpenAvnu currently returns success for both inputs and outputs in [openavb_aecp_sm_entity_model_entity.c]($HOME/src/OpenAvnu/lib/avtp_pipeline/aecp/openavb_aecp_sm_entity_model_entity.c#L1106).

Impact:

- Controllers will observe non-Milan state semantics even before MVU/binding work starts.

Priority: High

Recommended work:

- Restrict these commands to bound listener stream inputs.
- Return `NOT_SUPPORTED` for stream outputs.
- Tie started/stopped state to Milan binding state, not only pause/resume of stream clients.

### 10. Redundancy support is not present as a Milan feature set

Relevant Milan clauses:

- 4.2.5 Redundancy
- Section 8 Seamless Network Redundancy
- Annex C stream descriptor extension

Findings:

- Redundancy is optional for Milan, but the current implementation is not an `R-PAAD`.
- The ADP capability set in [openavb_adp_pub.h]($HOME/src/OpenAvnu/lib/avtp_pipeline/adp/openavb_adp_pub.h#L47) has no Milan redundancy feature plumbing.
- The descriptor builder creates one AVB interface, not the required primary/secondary pair for redundancy.
- Descriptor structs still carry legacy backup-stream fields, but there is no actual redundant-pair modeling or per-interface behavior.
- There is no evidence of per-interface controller registration, per-interface unsolicited notifications, dual gPTP end stations, or independent primary/secondary MAAP state machines as required by Milan Section 8.

Impact:

- OpenAvnu should currently be treated as non-redundant Milan target only.
- Full Milan redundancy requires a separate architecture track, not a small patch set.

Priority: Optional overall, but Critical if the product goal is an `R-PAAD`

Recommended work:

- First decide whether Milan redundancy is in scope.
- If yes, add a dual-interface device model first, then add redundant stream-pair semantics, per-interface protocol state, and Section 8 control behavior.

### 11. `avdecc-lib` also needs a Milan update

Relevant Milan clauses:

- All MVU clauses
- New descriptor types and dynamic-info flows

Findings:

- The bundled controller library has no `TIMING`, `PTP_INSTANCE`, `PTP_PORT`, `GET_DYNAMIC_INFO`, `SET_MAX_TRANSIT_TIME`, or MVU support.
- Even if the entity side is updated, the repository’s own controller tooling will not be able to exercise the new Milan features until `avdecc-lib` is updated too.

Impact:

- Development and interoperability testing will be slower and less reliable.

Priority: High

Recommended work:

- Treat `avdecc-lib` as part of the Milan upgrade plan, not as a separate follow-up.

## Recommended Implementation Order

### Phase 1: Define the target Milan profile

Pick one explicit target first:

- Non-redundant Milan talker/listener end station
- Redundant Milan `R-PAAD`

Do not begin with redundancy unless there is a concrete product need; it multiplies the AVDECC and state-machine work.

### Phase 2: Rebuild the AEM model

- Add missing descriptor types and serializers.
- Move from “one configuration per stream” to “one configuration per device mode”.
- Instantiate `IDENTIFY`, audio clusters, stream ports, maps, timing, ptp instance, and ptp port descriptors.
- Validate against one Annex A example before generalizing.

### Phase 3: Finish base AEM dynamic behavior

- Implement `LOCK_ENTITY`.
- Implement registered-controller tracking and unsolicited notifications.
- Finish `SET_NAME`, `GET_NAME`, configuration commands, `GET_DYNAMIC_INFO`, `SET_MAX_TRANSIT_TIME`, and audio mapping commands.
- Correct `START_STREAMING` / `STOP_STREAMING` semantics.

### Phase 4: Add Milan MVU and binding

- Add MVU transport and commands.
- Implement media clock reference management.
- Implement `BIND_STREAM`, `UNBIND_STREAM`, and `GET_STREAM_INPUT_INFO_EX`.
- Align ACMP and listener state with Milan 5.5.

### Phase 5: Complete diagnostics and persistence

- Implement mandatory counters.
- Persist sampling rate and clock source.
- Ensure `GET_AVB_INFO`, `GET_AS_PATH`, and dynamic-info responses are accurate.

### Phase 6: Finish format advertisement and media-clock topology

- Advertise Milan Base/XF16/XF24 format families correctly.
- Ensure CRF inputs/outputs are exposed as required per supported clock domain.
- Ensure gPTP media-clock descriptor chain is correct.

### Phase 7: Add redundancy only if required

- Add dual AVB interfaces, independent protocol state per interface, redundant stream-pair modeling, and Section 8 semantics.

## Practical Conclusion

The shortest credible path to Milan is:

1. Target non-redundant Milan first.
2. Replace the current stream-centric AEM model with a device-centric Milan descriptor tree.
3. Implement controller registration/locking/notifications and missing AEM commands.
4. Add MVU and binding.
5. Update `avdecc-lib` in parallel.

Trying to “patch in” Milan on top of the current entity model without restructuring `openavb_avdecc.c`, AEM descriptor coverage, and AECP state handling will create a partial implementation that still fails controller interoperability.

## Notes

- This repository is currently mid-change in several AVDECC files. This report intentionally avoids rewriting those files and instead identifies the structural work needed.
- Static analysis cannot confirm whether the external gPTP daemon, hardware timestamping, or driver behavior already satisfy all Milan timing tolerances; those items need runtime validation after the control/model work is in place.
