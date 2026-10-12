# VitaDAW 0.8.3-A — Recording Reliability Audit & Test Matrix

## Scope and audit baseline

This document audits the recording path at `60917c2` (`VitaDAW 0.8.2`), without
changing production code. The checked test build was `build`; the targeted
recording/media/lifecycle suite passed on 2026-10-12:

```
vitadaw_project_media_integration_tests
vitadaw_input_monitoring_device_lifecycle_tests
vitadaw_recording_capture_tests
vitadaw_recording_application_tests
vitadaw_recording_recovery_tests
vitadaw_recording_media_io_tests
```

This is evidence for the existing automated contract, not evidence of a long
physical recording or a hardware-disconnect smoke test.

## Current recording foundation

### Data path

1. `DawApplication` accepts Record only from `Stopped`, with Loop disabled and
   one valid armed audio track. It performs recording preflight before exposing
   the RT begin command.
2. `JuceAudioDeviceAdapter::prepareRecording` opens requested inputs, prepares
   a fixed SPSC capture ring, creates an exclusive hidden WAV temporary, and
   persists recovery metadata when possible. The ring is currently two seconds
   at the device rate.
3. The audio callback is the only ring producer. `RealtimeCapture` performs no
   allocation and explicitly terminalizes missing input, ring overflow,
   device loss, or sample-rate change.
4. The application timer (30 Hz) invokes `serviceRecording`; its non-RT writer
   drains the ring into the temporary WAV. A write failure terminalizes the
   matching session directly.
5. Stop terminalizes capture and transport in the same RT command. The control
   side drains, rewrites/syncs/closes the WAV header, publishes through a
   no-replace hard link, syncs the containing directory, verifies identity and
   decodes before atomically committing source and clip to the project.
6. A failed/cancelled/shutdown take is never committed. Artifacts are retained
   conservatively and described through recovery markers; 0.8.2's inventory is
   discovery-only and never auto-repairs or deletes them.

### Existing state authorities

| Layer | States / authority | Notes |
|---|---|---|
| Transport | `Stopped`, `Playing`, `Paused` | Record projects `Playing` internally while capture is active; Stop is the terminal command. |
| RT capture | `idle`, `prepared`, `capturing`, `complete`, `failed` | SPSC status/failure publication; `finalizing` is deliberately not RT-owned. |
| Application | capture states plus `finalizing` | Only this layer may finalise and commit a take. |
| Device | operational / initialising / stopped / error | Device loss terminalizes active capture as `deviceLost`; UI-thread polling consumes lifecycle events. |

The requested user-facing `Armed` state is represented today by
`ProjectSession::armedTrack`, not by `RecordingPhase`. It should remain
orthogonal to capture state: a failed take must not silently disarm the track.

## What is already protected

| Area | Evidence in current code/tests |
|---|---|
| No callback allocation and bounded capture memory | SPSC ring allocated before Record; capture allocation test; two fixed sample vectors. |
| Capture integrity | Wrap, partial callbacks, stereo separation, exact accepted frame count and callback partition invariance are tested. |
| Input/device loss at Core level | `missingInput`, `deviceLost`, `sampleRateChanged` and overflow terminalize explicitly; capture tests cover the core transitions. |
| Writer/media failures | Fault injection covers write, WAV-header, file-sync, close, publication and directory-sync failures. |
| Project atomicity | Failed decode/waveform/plan/placement does not add a source, clip or history mutation. |
| Stop semantics | Application tests explicitly assert Record-toggle and explicit Stop leave transport stopped, including with monitoring enabled. |
| Recovery retention | Temporary/final markers, closed-uncommitted takes and torn-marker scans are covered; artifacts are retained rather than deleted by a pathname race. |

## Gaps and risks to close in 0.8.3

| Priority | Gap | Why it matters | Intended closure |
|---|---|---|---|
| P0 | No endurance test of the real producer/drain/writer path | Passing unit tests do not show stable file growth, bounded memory, or lack of callback starvation over time. | Deterministic endurance harness plus physical 30/60/120-minute smoke tiers; record telemetry. |
| P0 | Writer service runs on the UI timer at 30 Hz; ring holds about two seconds | A blocked UI or slow storage can fill the ring. Current safe behavior is an `overflow` failure, but this has no measured headroom or user-visible root cause. | Measure drain lag/high-water mark and write latency; test timer stalls and slow/short writes; define the accepted abort/recovery UX. |
| P0 | Device/configuration change during capture has no explicit recording policy test | `pollDeviceLifecycle` may reprepare an observed configuration change. A rate change is eventually failed in RT; same-rate device/buffer changes have no take-specific contract proving no silent discontinuity. | Decide policy: abort any active take before reconfiguration (recommended for 0.8.3), retain/recover it, stop transport, and prove it. |
| P0 | Input route disappearance is not separately tested while recording | Monitoring route loss is handled, but recording must independently fail safely if active inputs vanish/change. | Add adapter-level fault seams and tests for zero/changed inputs during Record, with no project commit. |
| P1 | The app-level phase can briefly project `capturing` before the callback consumes `beginRecord` | This is intentional asynchronous command behavior, but needs transition-table coverage for Stop/Cancel/device loss before first callback. | Exhaustive transition tests for prepared/begin-pending states and stale commands. |
| P1 | Disk-space/media errors are injected, not observed under a real unavailable volume | Injection validates control flow but not platform behavior and diagnostics. | Physical removable-volume/unmount test where safe, or a documented OS fault harness; never fill a user disk. |
| P1 | Recovery tests use structural fixtures, not actual interrupted writer output | A valid marker and a synthetically truncated file are useful but do not prove post-failure WAV readability across real write/finalize points. | Generate actual recordings, interrupt at defined lifecycle points, then inventory/reopen and assert classification/readability. |
| P2 | No recording reliability telemetry | Investigation of glitches needs context beyond an end-state failure. | Expose session id, device/rate/buffer, ring capacity/high-water, drained/written frames, service lag, terminal failure and retained paths in a bounded diagnostic record. |

## 0.8.3 state/transition contract

The following is the release contract to encode in tests before hardware work:

| From | Event | To | Mandatory effects |
|---|---|---|---|
| Stopped + Armed | Record accepted | Prepared / begin pending | Allocate/preflight only; no project mutation; no implicit Play command. |
| Prepared | first RT begin | Recording | Capture starts at one exact frame; writer is ready first. |
| Recording | Stop or Record-toggle | Complete → Finalizing → Stopped | Stop capture before the next captured callback; drain/finalize/commit once; never enter Play. |
| Recording | Cancel | Failed(cancelled) → Stopped | No project mutation; close/retain safely. |
| Recording | missing input, device loss, rate/configuration change, writer failure, overflow | Failed(reason) → Stopped/Error | Stop transport; drain already-published frames if safe; never commit; retain recoverable media and diagnostic. |
| Finalizing | success | Complete + Stopped | Commit exactly one source and clip; arm is preserved. |
| Finalizing | any failure | Failed + Stopped/Error | No partial project/history mutation; published/temporary evidence remains discoverable. |
| Error | explicit successful device recovery | Stopped + Armed (arm preserved if track survives) | New Record requires full preflight; no automatic resume. |

`Playing` is never a legal destination of recording finalization or failure.
`Armed` stays orthogonal to transport and capture phases.

## Test matrix

| ID | Layer | Scenario | Assertions |
|---|---|---|---|
| LR-01 | Core | Simulated 2+ hour equivalent, fixed callback cadence and regular drain | Exact frame count/order; no allocation; ring high-water stays below capacity; memory remains constant. |
| LR-02 | Adapter | Long writer with bounded short writes and periodic service jitter | WAV size grows monotonically; no lost/reordered frames; explicit overflow only after documented headroom is exceeded. |
| LR-03 | Physical | 30/60/120-minute mono and stereo takes on each real interface/aggregate | Audible continuity; expected duration/frame count; file opens after restart; no device/UI degradation. |
| DF-01 | Adapter | `audioDeviceStopped` while recording | `deviceLost`; transport stopped; no project commit; closed temporary is discoverable. |
| DF-02 | Adapter | asynchronous `audioDeviceError` while recording | Same as DF-01; cleanup occurs only on the control side. |
| DF-03 | Adapter | active input route disappears/changes | Deterministic failure and retained material; monitoring behavior remains independent. |
| DF-04 | Adapter | rate, buffer, or device identity changes while recording | Chosen policy is enforced before reconfiguration; no silent continuation across a discontinuity. |
| IO-01..06 | Adapter | write/header/sync/close/publication/directory-sync faults | No commit; accurate terminal reason; retained candidate and marker classification match the failure point. |
| RI-01 | Integration | Failure before first PCM, during PCM, after close, after publication, after directory sync | On relaunch/inventory, candidate is safely classified; only structurally valid files are offered. |
| ST-01 | Application | Stop, Record-toggle, Cancel, device loss and writer error from every pending state | Transport remains Stopped; exactly-once cleanup; no implicit Play; a subsequent take can start. |
| RG-01 | Full | Record, monitoring, undo/redo, multi-track, import/media, recovery and transport smoke | Existing workflows still pass without a new state leak or history mutation. |

## Recommended implementation order

1. Add observability and deterministic non-physical tests for LR-01/LR-02 and
   the complete transition table. Do not alter the audio-thread allocation
   contract.
2. Make device/input/configuration-loss policy explicit and testable at the
   adapter boundary; recommended policy is fail-and-retain the active take
   before any hardware reconfiguration.
3. Expand fault injection from raw file helpers to recording integration tests,
   including actual marker/inventory outcomes.
4. Execute physical smoke tiers and record results per interface, aggregate,
   sample rate, buffer and storage target.

## First implementation prompt (GPT-5 mini / local Copilot)

> Work on VitaDAW 0.8.3-B foundation only. First read
> `docs/audit-recording-reliability-0.8.3.md`, `RealtimeCapture`,
> `RealtimeAudioEngine`, `JuceAudioDeviceAdapter`, and the existing recording
> tests. Do not change UI behavior or start hardware tests. Add a deterministic,
> non-RT test seam that can exercise prolonged recording with controlled writer
> service cadence/jitter and bounded writes. Add a small bounded recording
> diagnostics snapshot containing at least session id, ring capacity/high-water,
> accepted/drained frames, and terminal failure. Preserve the callback contract:
> no allocation, locks, filesystem I/O, or new device calls in RT. Add tests for
> exact PCM/frame integrity over a long simulated run, constant capture
> allocation, and a delayed service period that deterministically yields the
> existing overflow failure without corrupting earlier drained PCM. Keep all
> existing tests green. Report the files changed, tests run, and any design
> decision that would affect the subsequent device-failure phase.
