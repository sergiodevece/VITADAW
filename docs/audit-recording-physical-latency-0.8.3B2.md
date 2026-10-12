# VitaDAW 0.8.3-B2 — Physical Service-Latency Validation

0.8.3-B proved, deterministically, how much writer lateness the capture ring
absorbs: at 48 kHz with 512-frame callbacks the 2 s ring (96 000 frames)
accepts 95 744 frames, i.e. **1.9947 s without a drain** — about 59.8 nominal
30 Hz service periods — before it fails explicitly with `overflow`. It could
not say how late the writer service actually gets in a running VitaDAW.

0.8.3-B2 adds the instrumentation to measure that on real hardware, and the
procedure to do it. **It changes nothing in Recording**: writer, service
thread, ring, ring size, state machine, overflow policy and Record semantics
are exactly those of `cdd670f`.

**Result (§8.5–§8.8).** On a MacBook Air at 48 kHz / 512, every permitted
normal-load scenario is **VERDE**. The worst case is VitaDAW in the background
(P-2): a 643 ms service gap that still leaves **1.360 s** of ring headroom. The
development stalls behave exactly as predicted, an over-budget stall fails
safely, and VitaDAW records, publishes and plays a new take after repeated
overflows in the same process without a restart (P-8c, also covered by an
automated regression on the real adapter).

## 1. The real service path

Verified in the code before any change:

```text
JUCE message thread
  VitaDawJuceApplication::timerCallback            30 Hz (startTimerHz(30))
    JuceAudioDeviceAdapter::pollDeviceLifecycle
    DawApplication::synchroniseTransport
      DawApplication::synchroniseRecording         returns unless a take is busy
        IAudioEngineControl::serviceRecording  →  JuceAudioDeviceAdapter::serviceRecording
          RealtimeAudioEngine::drainRecording      8 192-frame chunks, until empty
            RealtimeCapture::drain                 sole SPSC consumer
          juce::AudioFormatWriter::writeFromAudioSampleBuffer
```

`serviceRecording()` has two further callers, both on the message thread and
both after the producer has already stopped: `finalizeRecording()` (final drain
before publication) and `shutdownRecording()` (final drain at application
quit). The audio callback never calls `serviceRecording()`; it only produces
into the ring.

The service is therefore exactly as late as the message thread is busy:
anything that occupies it — painting, a synchronous operation, a nested event
loop — delays the drain by that much.

## 2. What is measured, where, and with which clock

| Point | Code | What happens |
|---|---|---|
| Take start | `JuceAudioDeviceAdapter::tryRequestRecord` | `steady_clock::now()` is read **before** the Record request is published to the callback; the take is anchored there only if the request is accepted. The first measured gap therefore covers the whole time the ring is unattended once capture can start (over-estimating it by at most one callback). |
| Each service pass | `JuceAudioDeviceAdapter::serviceRecording` → `observeRecordingService` | After any development stall (§6) and **before** draining: `steady_clock::now()`; the gap since the previous pass updates the counters. The gap is drain-start to drain-start, i.e. how long the ring went without a new drain. |
| End of window | the first pass that observes `complete` or `failed` | That pass is still counted — its gap is how long the ring was unattended until the end was noticed, which matters most for an overflow. Every later pass (finalization drains) is ignored. |
| Take end | `confirmRecordingCommit` / `discardRecordingWithDiagnostics` | Immediately before `resetRecordingCapture()`, the 0.8.3-B ring telemetry is read once and the summary is produced. Every successful, failed, cancelled and shutdown take passes through exactly one of these. |

**Clock:** `std::chrono::steady_clock` — monotonic, unaffected by wall-clock
changes, nanosecond resolution on macOS. Every timing read happens on the
message thread; **none happens in the audio callback**.

**Cost while recording:** per 30 Hz service pass, one or two `steady_clock`
reads and a handful of integer comparisons in fixed-size state. No logging, no
allocation, no I/O and no string work happens until the take has ended; then
one summary is formatted, written to the log and appended to one file.

## 3. Summary fields

The summary is printed once per take. Gap counters are cumulative: a gap is
counted in every threshold it **strictly** exceeds.

| Field | Source | Meaning |
|---|---|---|
| `Terminal state` | first pass observing the end, else cleanup snapshot | `complete / none`, `failed / overflow`, `failed / cancelled`, … |
| `Device` | Record request | device sample rate and buffer size of the take |
| `Duration` | platform | Record request → last counted service pass |
| `Service calls` | platform | counted service passes |
| `Nominal period` | platform | 1 / 30 Hz = 33.3 ms |
| `Maximum service gap` | platform | largest drain-to-drain interval of the take |
| `Gaps > nominal` | platform | gaps above 33.3 ms. Expect a large share in normal use: an OS timer jitters around its period. It is context, not an alarm. |
| `Gaps > 50 … 2000 ms` | platform | gaps above 50, 100, 250, 500, 1000, 1500, 1900 and 2000 ms |
| `Ring capacity` | Core telemetry | ring frames for this take |
| `Peak ring occupancy` | Core telemetry | high-water mark written by the producer — **the authoritative risk metric** |
| `Peak service backlog` | Core telemetry | largest backlog any drain found on entry |
| `Minimum remaining headroom` | derived | `capacity − peakOccupancy`, in frames and in seconds at the device rate |
| `Late service events` | Core telemetry | drain calls whose backlog exceeded one service period plus one device buffer; one very late pass can count several (one per 8 192-frame chunk) |
| `Dropped frames` / `Overflow events` | Core telemetry | non-zero only if the ring overflowed; the take then fails explicitly and is never committed |
| `Injected development stalls` | platform | stalls executed from the development plan (§6) |
| `Headroom class` | derived | VERDE / AMARILLO / NARANJA / ROJO, §7 |

**Remaining headroom** answers the 0.8.3-B2 question directly: *at the worst
moment of this take, how much longer could the service have stalled before
audio was lost?* It is an upper bound by at most one device buffer, because
overflow rejects a whole callback that no longer fits (0.8.3-B, LR-4: 256
frames ≈ 5.3 ms at 48 kHz / 512).

**Gap vs. occupancy.** A gap is measured between pass *starts*, so a slow WAV
write inside pass *k* lengthens gap *k → k+1* even though the ring was being
drained during that write. Peak occupancy is what the producer actually saw.
Where the two disagree, occupancy decides the risk and the gap explains it.

## 4. Isolation from production

| Guard | Effect |
|---|---|
| CMake option `VITADAW_RECORDING_SERVICE_DIAGNOSTICS`, default **OFF** | only when ON is `VITADAW_RECORDING_SERVICE_DIAGNOSTICS=1` defined, and only for the `vitadaw_app` target |
| `#if` around every member, helper and hook in the adapter | a normal build contains **no** diagnostic state, code or strings. Verified on the binaries: the normal app has zero occurrences of `VITADAW_DIAG_SERVICE_STALLS_MS`, `VITADAW_DIAG_SUMMARY_FILE`, the log prefix and the summary labels; the diagnostics app has them |
| Configure-time `CMake Warning` | the option cannot be enabled silently |
| Startup banner `DEVELOPMENT BUILD - service-latency diagnostics enabled` | a diagnostics build cannot be mistaken for a normal one at run time |
| Platform layer only | the accumulator (`platform/juce/RecordingServiceDiagnostics.h`) and the hooks live in the JUCE adapter. No change to `IAudioEngineControl`, `DawApplication`, `RecordingTypes.h` or any file under `src/vitadaw/audio/`; Core does not know the timer rate |

The audio callback (`audioDeviceIOCallbackWithContext`) is byte-identical to
`cdd670f`, and `RealtimeCapture` / `RealtimeAudioEngine` are untouched.

## 5. Automated tests

`tests/RecordingServiceDiagnosticsTests.cpp` (`vitadaw_recording_service_diagnostics_tests`,
injected time, no sleeps):

- every bucket edge, just below, on and just above each threshold (strictly
  greater than), the maximum gap and the duration;
- the first gap measured from the Record request, not hidden before the first
  pass;
- remaining headroom in frames and seconds, saturation at a full ring, and
  every classification boundary;
- reset between takes: session, rate, buffer, counters, buckets, stalls and
  terminal state all start from zero;
- the pass revealing an overflow is kept, later finalization drains are
  ignored, an idle or finished take emits nothing, and an unobserved end takes
  its reason from the cleanup snapshot;
- stall-plan parsing (malformed, out-of-range, excess and timing errors reject
  the whole plan) and stall scheduling (offset, spacing, one stall per pass,
  re-armed per take);
- the formatted summary carries every mandatory magnitude.

The diagnostic hooks in the adapter are compiled only in a diagnostics build
and have no automated test of their own; the diagnostics build is compiled as
part of this phase to prove it builds, and the physical smoke exercises them.

**Recovery after overflow — production path.**
`tests/ProjectMediaIntegrationTests.cpp` → `recordingRecoversAfterOverflow()`
drives the **real** `JuceAudioDeviceAdapter` and `DawApplication` through the
existing hardware-free virtual device. That device has no thread, so every
callback is the production `audioDeviceIOCallbackWithContext`, invoked
deterministically by the test. The test was added after the forensic audit in
§8.6 found the path uncovered. It asserts:

1. *Take A*: Record accepted; with no service the real ring fails as
   `failed / overflow`, exactly one 256-frame callback dropped, and the accepted
   frames equal the ring capacity.
2. The application timer path (`synchroniseTransport`) drains, cleans up and
   stops the transport; the error reports the overflow and the retained media.
   Nothing is committed: no source, no clip, session not dirty, undo cursor and
   state token unchanged.
3. The retained temporary `.Recording 000001.1.part.wav` is a readable WAV that
   holds **exactly** the accepted prefix, sample for sample.
4. *Take B*, same process and project: a new Record is accepted (session 2);
   real PCM is captured; the telemetry starts from zero (no inherited overflow
   or dropped frames).
5. Stop completes; finalization publishes `Recording 000002.wav`; exactly one
   source and one clip are committed with the exact frame count; the published
   clip holds take B's PCM sample for sample; `Recording 000001.wav` is never
   published; the failed take's retained media is untouched.

This is the automated counterpart of the P-8c smoke. It does not involve the
diagnostics build: it covers production behaviour.

## 6. Development stall (P-8)

Purpose: prove that the measurement responds, that the ring grows and
recovers as predicted, and that an over-budget stall fails safely. Available
**only** in a diagnostics build, controlled by environment variables read once
at startup:

| Variable | Meaning | Default |
|---|---|---|
| `VITADAW_DIAG_SERVICE_STALLS_MS` | comma-separated stalls, each 1–10 000 ms, at most 16 | none |
| `VITADAW_DIAG_SERVICE_STALL_FIRST_MS` | delay from the Record request to the first stall | 5000 |
| `VITADAW_DIAG_SERVICE_STALL_SPACING_MS` | delay between successive stalls | 5000 |
| `VITADAW_DIAG_SUMMARY_FILE` | summary file override (absolute or relative to the working directory) | `~/Library/Logs/VitaDAW/recording-service-diagnostics.log` |

A malformed plan is rejected whole (the banner reports `INVALID`) and nothing
is injected. Each stall is a `sleep_for` **inside a service pass on the
message thread**, taken only while the capture is `capturing`, at most one per
pass — exactly what a slow main-thread operation does to the writer.

**The plan is re-armed on every new take; it is not consumed once per
process.** The environment is read once at startup, but every accepted Record
restarts the schedule, so every take of the same launch receives the same
stalls at the same offsets. This is deliberate: it makes a fault reproducible
take after take. The consequence must be kept in mind when testing recovery.
With `VITADAW_DIAG_SERVICE_STALLS_MS=2100`, **every** take that is still
capturing when the first stall is due (`FIRST_MS`, default 5 s) overflows
again in exactly the same way. A take stopped before that point is never
stalled — that is how P-8c verifies recovery within the same launch.

Expected results at 48 kHz / 512, where the budget is 1.9947 s. A stall *S*
starts inside a pass that is itself about one period after the previous one,
so its gap is ≈ *S* + 33 ms:

| Stall | Expected gap | Expected bucket | Expected outcome |
|---|---|---|---|
| 50 ms | ≈ 83 ms | > 50 | recovers |
| 100 ms | ≈ 133 ms | > 100 | recovers |
| 250 ms | ≈ 283 ms | > 250 | recovers |
| 500 ms | ≈ 533 ms | > 500 | recovers |
| 1000 ms | ≈ 1033 ms | > 1000 | recovers |
| 1500 ms | ≈ 1533 ms | > 1500 | recovers |
| 1900 ms | ≈ 1933 ms | > 1900 | recovers with only ≈ 60 ms (≈ 3 000 frames) of headroom → class ROJO, as designed |
| 2100 ms | ≈ 2133 ms | > 2000 | **overflow**: `failed / overflow`, `Overflow events: 1`, `Dropped frames` = one callback, take not committed |

A stall is a fault injection: ROJO or overflow on P-8 is the expected,
correct result, not an architectural failure. With larger device buffers the
usable ring is smaller (95 232 frames at 1 024) — run P-8 at 48 kHz / 512.

## 7. Classification

Applied to the **worst normal-load take** (P-1 … P-7):

| Class | Worst remaining headroom |
|---|---|
| **VERDE** | > 1.0 s |
| **AMARILLO** | 0.5 – 1.0 s |
| **NARANJA** | 0.2 – 0.5 s |
| **ROJO** | < 0.2 s, or any dropped frame / overflow |

Boundaries as implemented: 1.0 s is AMARILLO, 0.5 s is AMARILLO, 0.2 s is
NARANJA. The summary prints the class of each take; the verdict of the phase
is the class of the worst P-1 … P-7 take.

## 8. Physical smoke procedure

### 8.1 Build

```sh
cmake -S . -B build-diag-083b2 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DVITADAW_RECORDING_SERVICE_DIAGNOSTICS=ON
cmake --build build-diag-083b2 --target vitadaw_app -j8
```

RelWithDebInfo is deliberate: main-thread costs in a Debug build (unoptimised
painting) would make the result pessimistic. Expect the configure warning
from §4. Use this build **only** for the smoke; never distribute it. (Already
built at `build-diag-083b2/vitadaw_app_artefacts/RelWithDebInfo/VitaDAW.app`
during preparation.)

### 8.2 Launch

Launch the executable from Terminal so the log is visible:

```sh
build-diag-083b2/vitadaw_app_artefacts/RelWithDebInfo/VitaDAW.app/Contents/MacOS/VitaDAW
```

The first lines must include:

```text
[VitaDAW recording diagnostics] DEVELOPMENT BUILD - service-latency diagnostics enabled; stall plan: none; summaries appended to …/Library/Logs/VitaDAW/recording-service-diagnostics.log
```

No banner means this is not a diagnostics build — stop. Launched this way,
macOS attributes microphone access to Terminal; if the input meter stays
silent, allow Terminal under *System Settings › Privacy & Security ›
Microphone*. Every summary is also appended to the log file, which is the
authoritative record (`tail -f ~/Library/Logs/VitaDAW/recording-service-diagnostics.log`).

### 8.3 Common setup

1. Note interface, macOS version, machine, power source.
2. Set the device to **48 kHz, 512 frames** (primary configuration). Optionally
   repeat the worst scenario at the smallest and largest buffer you use.
3. Save a project, create and arm one mono track. For P-3/P-5 the project
   should already contain several long clips (several minutes each) so the
   arrange view has real waveforms to paint — import them **before** recording.
4. Do **not** change the audio device, sample rate or buffer size during a
   take: that is device reconfiguration under capture (DF-01…DF-04), a later
   0.8.3 phase, not service latency.
5. One take per scenario: press Record, perform the action continuously for the
   stated time, press Stop, and copy the summary into §8.5.

### 8.4 Scenarios

| ID | Scenario | Duration | What to do during the take | Applicability |
|---|---|---|---|---|
| P-1 | Idle | 5 min | Nothing. Leave VitaDAW frontmost. | Applies |
| P-2 | Window manipulation | 2 min | Drag the window continuously, resize repeatedly, enter/leave full screen, Mission Control / switch Spaces. **Also** minimise VitaDAW or bring another app to the front for ≥ 60 s: a hidden app may have its timers coalesced by macOS (App Nap), the most plausible source of a long real gap. Record which sub-case produced the worst gap. | Applies |
| P-3 | Arrange navigation | 2 min | Zoom in/out repeatedly from closest to furthest; drag both scrollbars quickly end to end; select tracks. | Applies — zoom and scroll are UI state, not commands |
| P-4 | Save during Record | — | Press *Save*: VitaDAW rejects it immediately ("Recording is capturing or finalizing") and writes nothing. | **NO APLICA / operación no permitida durante Record** |
| P-5 | Waveform painting load | 2 min | With the long clips from §8.3, zoom between extremes continuously so every repaint redraws waveforms at a new scale. Waveform *rebuild* is not reachable while recording, because media cannot change during a take; repainting is. Executed as **P-5b**, a multitrack arrange variant. | Applies (painting only) |
| P-6 | Import / decode during Record | — | *Import WAV* is disabled while recording. | **NO APLICA / operación no permitida durante Record** |
| P-7 | Native dialogs | 2 min | *Save As…* and *Load Project…* stay enabled and open the native macOS panel during a take. Open one, browse folders for 20–30 s, cancel; repeat with the other. Choosing a file is harmless: the command is rejected afterwards. | Applies |
| P-8a | Stall ladder | ≥ 45 s | Relaunch with `VITADAW_DIAG_SERVICE_STALLS_MS=50,100,250,500,1000,1500,1900` and record one take of at least 45 s (last stall at 35 s). Expected: buckets `>50…>1900` = 7, 6, 5, 4, 3, 2, 1; `>2000` = 0; maximum gap ≈ 1.93 s; no overflow; class ROJO from the 1 900 ms stall. | Development build only |
| P-8b | Over-budget stall | ≥ 10 s | Relaunch with `VITADAW_DIAG_SERVICE_STALLS_MS=2100`. Expected: overflow at the stall; the take fails with "Recording stopped because the capture buffer overflowed"; summary `failed / overflow`, `Overflow events: 1`, `Dropped frames: 512`; no clip or source added. **Every further take of the same launch that runs past 5 s overflows again in the same way (§6) — that is the stall repeating, not a blocked engine.** | Development build only |
| P-8c | Recovery after overflow | < 5 s | In the **same** launch, after P-8b: press Record and press Stop **before** the first stall is due (5 s). Expected: `complete / none`, `Injected development stalls: 0`, no overflow; the clip is published and appears in the arrange; it plays back correctly; the failed takes' retained media is untouched. | Development build only |

Natural UI gaps add to the P-8a buckets; a count above the prediction is
explained by the `Maximum service gap` and the scenario, not a defect.

### 8.5 Results

Environment: MacBook Air (`Mac17,4`) · macOS 26.5 (25F71) · built-in device
reported as *Altavoces del MacBook Air*, 1 input / 2 outputs · 48 000 Hz ·
512-frame buffer · `RelWithDebInfo` diagnostics build · power source not
recorded. Every figure below is copied from
`~/Library/Logs/VitaDAW/recording-service-diagnostics.log` (log time in
brackets); the scenario of each take was assigned by the operator.

| ID | Duration (s) | Service calls | Max gap (ms) | >50 | >100 | >250 | >500 | >1000 | >1500 | >1900 | >2000 | Peak occupancy (frames / ms) | Min headroom (frames / s) | Late events | Dropped | Overflow | Terminal | Class | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P-1 | 307.204 | 8716 | 93.5 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4096 / 85.3 | 91 904 / 1.915 | 2 | 0 | 0 | complete / none | **VERDE** | [17:25:23] baseline, no interaction |
| P-2 | 147.643 | 4164 | 643.0 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 0 | 30 720 / 640.0 | 65 280 / 1.360 | 9 | 0 | 0 | complete / none | **VERDE** | [17:48:57] VitaDAW in the background — worst normal load |
| P-3 | 113.823 | 3196 | 44.5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2048 / 42.7 | 93 952 / 1.957 | 0 | 0 | 0 | complete / none | **VERDE** | [17:33:55] zoom / scroll |
| P-4 | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | NO APLICA / operación no permitida durante Record |
| P-5b | 120.802 | 3375 | 48.1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2560 / 53.3 | 93 440 / 1.947 | 3 | 0 | 0 | complete / none | **VERDE** | [17:41:50] multitrack waveform painting |
| P-6 | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | NO APLICA / operación no permitida durante Record |
| P-7 | 53.004 | 1489 | 161.2 | 13 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 7680 / 160.0 | 88 320 / 1.840 | 14 | 0 | 0 | complete / none | **VERDE** | [17:52:01] native Save As / Load panels |
| P-8a | 44.684 | 1103 | 1934.5 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 | 92 672 / 1930.7 | 3328 / 0.069 | 34 | 0 | 0 | complete / none | ROJO (expected) | [17:57:04] stall ladder; 7 stalls injected |
| P-8b | 7.102 | 141 | 2136.9 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 95 744 / 1994.7 | 256 / 0.005 | 12 | 512 | 1 | failed / overflow | ROJO (expected) | [17:59:30] 2100 ms stall; no clip published |
| P-8c | 3.292 | 93 | 38.8 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2048 / 42.7 | 93 952 / 1.957 | 0 | 0 | 0 | complete / none | VERDE | [18:44:30] same launch as P-8b, after three overflows; clip published and played back |

Worst normal-load take (P-1 … P-7): **P-2** · 643.0 ms maximum gap ·
remaining headroom **1.360 s** · **class VERDE**.

P-8a matched its prediction in every bucket (7, 6, 5, 4, 3, 2, 1, 0) and in
size (maximum gap 1934.5 ms predicted ≈ 1933 ms; 69 ms of headroom left,
predicted ≈ 60 ms). P-8b overflowed at exactly the 0.8.3-B limit: peak
occupancy 95 744 frames, one 512-frame callback rejected.

Traceability: the log holds 13 takes. Besides those above, takes [17:30:36],
[17:31:25] and [17:31:53] were not assigned to a scenario by the operator (all
`complete / none`, VERDE, worst gap 50.3 ms), and takes [18:06:57] and
[18:08:45] are explained in §8.6.

### 8.6 Forensic note: the "blocked" Record after P-8b

After P-8b, two further attempts to record in the same launch appeared to
fail: no clip, and the same overflow message. A read-only forensic audit
established what happened:

- the log holds two further takes, sessions 2 and 3 ([18:06:57], [18:08:45]),
  each `failed / overflow` with `Injected development stalls: 1` and figures
  identical to P-8b;
- their retained temporaries sit in the same project folder with
  per-process nonces 1, 2, 3 — the same process, no restart;
- each holds exactly 334 336 frames (653 whole callbacks) of real, non-zero
  microphone audio in a valid WAV.

The engine had **not** blocked. Each new Record was accepted, captured real
audio, and was then overflowed by the same 2100 ms development stall, re-armed
for every take (§6). The previous version of this procedure wrongly promised
that "a new Record then works" after P-8b in the same launch; §6 and §8.4 now
state the actual semantics, and P-8c verifies recovery without being caught by
the stall. Play also appeared not to work: an empty project has no prepared
audio, and `tryRequestPlay()` rejects Play then by design (no loop, no
metronome). Neither observation was a recovery defect.

### 8.7 Recovery after overflow

| Aspect | Status | Evidence |
|---|---|---|
| **Operational recovery** — a new take can be recorded, stopped, published and played after an overflow, without restarting | **Demonstrated** | Physical P-8c (after three overflows in one process) and the automated regression in §5 on the real adapter |
| Retained media of a failed take is a valid, complete record of the accepted audio | Demonstrated for the files involved | Forensic read of the three retained temporaries (valid WAV, exact whole-callback frame counts); the automated regression checks the retained temporary sample for sample |
| **Content recovery** — bringing a retained partial take back into a project through the 0.8.2 recovery flow | **Not validated** | Not exercised by this smoke; remains open for a later 0.8.3 phase |

### 8.8 Conclusion

On this machine and configuration, the 30 Hz UI-serviced writer has ample
margin for everything VitaDAW permits during Record: the worst normal load
leaves 1.360 s of the 1.9947 s budget. Background operation (P-2) is the
dominant real-world source of service delay, at 643 ms about a third of the
budget. It is the scenario to watch on other machines and for longer
background periods; asking macOS not to throttle VitaDAW while a take is
active is a candidate mitigation for a later phase, not decided here. The
overflow path fails safely, and VitaDAW recovers from it without a restart.

## 9. What this does not demonstrate

- **Audio-callback health.** Only the service side is measured. A driver-level
  input dropout or an xrun inside the device is invisible here: the ring only
  sees what the callback delivered.
- **Other machines or configurations.** Results hold for the machine,
  interface, buffer size, OS version and system load of the run — here a single
  MacBook Air with its built-in device at 48 kHz / 512. External interfaces,
  other buffer sizes and longer background periods were not measured.
- **Content recovery of retained takes** (§8.7): the retained media was
  verified as files, not recovered into a project.
- **Exhaustive coverage of main-thread work.** The scenarios cover the
  operations VitaDAW permits during Record today. Operations it rejects (save,
  import, editing) cannot delay the service now, but would have to be measured
  if a future version allowed them during a take.
- **The exact time to overflow.** Remaining headroom is optimistic by up to one
  device buffer (§3).
- **The message-thread assertion in RelWithDebInfo.** The hooks carry
  `JUCE_ASSERT_MESSAGE_THREAD`, but assertions are compiled out with `NDEBUG`.
  The single-thread claim rests on the code path in §1; a Debug diagnostics
  build also checks it at run time.
- **A fix.** 0.8.3-B2 measures. On this configuration the result does not call
  for a larger ring or a dedicated service thread (0.8.3-B §9); that decision
  stays open for configurations not measured here.

## 10. Pending visual issues (not fixed in this phase)

The repository has no separate backlog file, so they are recorded here. Both
were observed during the smoke and are unrelated to recording reliability.

| ID | Issue |
|---|---|
| **UI-1** | Clips visually overlap neighbouring tracks while the timeline is scrolled. |
| **UI-2** | A double Stop returns the playhead to zero but does not scroll the arrange viewport back to zero. |

