# VitaDAW 0.8.3-B — Recording Endurance

Phase 0.8.3-B closes the measurement half of the P0 risk raised by
[`audit-recording-reliability-0.8.3.md`](audit-recording-reliability-0.8.3.md):

> The writer is serviced from the UI timer at ~30 Hz against a ring of ~2
> seconds. The design is bounded and currently safe, but its resistance to
> jitter, a slow UI or a late writer drain was never demonstrated and the real
> available margin was unknown.

It deliberately **does not change the writer model**. The ring, the SPSC
exchange, the UI-timer service and the failure policy are the 0.8.2 ones; what
0.8.3-B adds is bounded RT-safe instrumentation and a deterministic endurance
harness that measures the margin. Baseline: `60917c2` (VitaDAW 0.8.2), test
build `build` (Debug, AppleClang, macOS 25.5).

## 1. Architecture actually under test

Reconstructed from the code before any change was made:

| Stage | Owner | Facts |
|---|---|---|
| Ring allocation | `JuceAudioDeviceAdapter::prepareRecording` (non-RT) | `ceil(deviceRate * 2.0)` frames, two `std::vector<float>` channels, allocated before Record is exposed to RT. 96 000 frames at 48 kHz. |
| Producer | `RealtimeAudioEngine::processBlock` → `RealtimeCapture::capture` | Sole producer. Rejects a whole callback and terminalizes `overflow` when it does not fit; never overwrites unread frames. |
| Consumer | `DawApplication::synchroniseRecording` → `JuceAudioDeviceAdapter::serviceRecording` | Sole consumer. Drains in 8 192-frame chunks **in a loop until the ring is empty**, so one service pass can empty the whole ring; a write failure terminalizes `writerFailed`. |
| Service cadence | `VitaDawJuceApplication::timerCallback` | The JUCE UI timer, 30 Hz → one nominal service period is 1 600 frames / 33.33 ms at 48 kHz. |
| Finalization | control thread only | Stop → drain → header/sync/close → no-replace hard-link publication → identity verification → atomic project commit. A failed take is never committed. |

Two facts from the reconstruction shape every scenario below: the drain loop is
**not** rate-limited per tick (so recovery after a stall is immediate and bounded
only by write throughput), and overflow rejects a **whole callback** (so the
accepted frames are always a contiguous prefix of the input stream).

The ring length was a magic literal in the adapter. It is now the named
constant `audio::recordingCaptureRingSeconds` (`RecordingTypes.h`), with the
same value: it is a recording-domain decision and belongs to Core. The service
cadence is deliberately *not* in Core. `JuceApplication` owns its 30 Hz timer
as a platform/UI decision, and `JuceAudioDeviceAdapter` keeps a file-local
`expectedRecordingServiceRateHz` mirroring it for one purpose only — labelling
late service in telemetry. Nothing in Core describes or governs the timer, and
the two concepts ("how often the UI repaints" and "how often the writer is
expected to be serviced") stay separate even though one timer serves both.

## 2. Instrumentation

`RecordingCaptureTelemetry` (`RecordingTypes.h`), published by
`RealtimeCapture::telemetry()` and reachable through exactly one further
accessor, `RealtimeAudioEngine::recordingTelemetry()`. That is the whole
surface: the counters are deliberately *not* exposed on
`IAudioEngineControl`, the adapter or `DawApplication`, because nothing in
production consumes them yet. When the diagnostic record of the 0.8.3-A P2 gap
acquires a real consumer, that is the moment to add the layer it needs — not
before.

| Field | Written by | Meaning |
|---|---|---|
| `ringCapacityFrames` | non-RT | total ring capacity |
| `occupancyFrames` | derived | `producedFrames - consumedFrames` |
| `peakOccupancyFrames` | producer | occupancy high-water mark |
| `minimumHeadroomFrames` | derived | `capacity - peakOccupancy`: the smallest margin the take ever had |
| `peakServiceBacklogFrames` | consumer | worst backlog observed at drain entry |
| `serviceBudgetFrames` | non-RT | nominal backlog: one service period + one device buffer |
| `producedFrames` / `consumedFrames` | producer / consumer | accepted and drained frames |
| `droppedFrames` / `overflowEvents` | producer | frames in rejected callbacks, and rejections |
| `captureCalls` | producer | well-formed capturing callbacks |
| `drainCalls` / `emptyDrainCalls` | consumer | drain invocations, and those that found the ring empty |
| `lateServiceEvents` | consumer | drain calls whose backlog exceeded the budget |

Ownership is strictly single-writer per counter: the audio callback writes only
producer counters, the recording service only consumer counters, and both are
zeroed together with the ring in `prepare`/`reset`/`begin`, i.e. at points where
no concurrent writer of those fields exists.

Recording has no underrun concept: an empty ring is the normal steady state of
a writer that keeps up, so `emptyDrainCalls` is reported rather than an
"underrun" count. `drainCalls` counts drain invocations, not service passes — a
healthy pass contributes one draining call plus the empty call that ends it,
which is why a nominal 30 s take reports 1 800 drain calls for 900 passes.

## 3. Methodology

`tests/RecordingEnduranceTests.cpp` (target
`vitadaw_recording_endurance_tests`, core-only, no JUCE and no hardware).

* **Deterministic simulated clock, measured in device frames.** No sleeps, no
  wall-clock dependency, no threads. The producer emits one callback per step;
  the writer runs when the simulated frame counter crosses its next deadline.
  Jitter, stalls and recovery are injected by moving that deadline.
* **Production RT path.** Blocks go through `RealtimeAudioEngine::processBlock`,
  not directly into the ring, so command consumption, the capture gate and the
  transport clock are all in the measured path.
* **Per-sample integrity, on top of frame counts.** Every frame carries its own
  stream index encoded as a dyadic rational, so the values stay exact in
  binary32 however long the run is; the right channel carries the negated
  pattern. The pattern repeats every 8192 frames, so the value comparison
  detects any loss, duplication, reordering or channel swap that is **not** an
  exact multiple of 8192 frames; a loss that *is* an exact multiple would slip
  past the values and is caught instead by the exact `produced == consumed`
  frame-count identity that every scenario asserts. The two checks together,
  not the value check alone, are what establish integrity.
* **Real write path.** The simulated writer mirrors `serviceRecording` (drain in
  8 192-frame chunks until empty, stop at the first write failure) and, in the
  shorter scenarios, writes the drained PCM through the production
  `platform::files::RecordingFileHandle` with `maximumWriteChunk` set, so the
  bounded short-write loop and the bytes that reach a real descriptor are
  verified too. The file is re-read and compared against the pattern afterwards.
* **Allocation accounting.** The test replaces the global
  `operator new(size_t)` and `operator new[](size_t)` and counts every call;
  a thread-local realtime-region flag separates calls made inside a capture
  callback from the rest. That yields two assertions: no intercepted
  `operator new`/`new[]` call inside any capture callback, and none at all
  during steady-state endurance. It is not a universal allocation proof —
  `malloc`/`calloc`/`realloc`, `posix_memalign`, over-aligned `operator new`
  (`align_val_t`), the `nothrow` forms and allocator-internal paths are not
  intercepted. The RT path uses no over-aligned types and no direct
  `malloc`, so the interception covers it in practice; the claim it supports
  is nonetheless the narrower one.
* **Tiers.** `VITADAW_ENDURANCE_SECONDS` scales LR-6. The default is 1 800 s of
  simulated audio (~16 s wall clock in Debug); 7 200 s and 14 400 s were run for
  this report.

## 4. Scenarios and results

All at 48 kHz, 512-frame callbacks, 2 s ring (96 000 frames), 30 Hz service
(1 600 frames = 33.33 ms per period), unless stated.

| ID | Scenario | Result |
|---|---|---|
| LR-1 | nominal service, 30 s mono, file-backed writer with 4 096-byte short writes | **PASS** — zero drops, zero overflows, stable occupancy, byte-exact file |
| LR-2 | service period jittered 1×–4× nominal, 30 s stereo, 7 000-byte short writes | **PASS** — ring absorbs the jitter, zero loss, channels independent, 323 late-service events recorded |
| LR-3 | 1.5 s writer pause inside the ring, then recovery | **PASS** — occupancy grows to 1.557 s, no overflow, drains back to zero, stream complete |
| LR-4 | capacity limit probed per callback size | **PASS** — maximum absorbable burst measured exactly; no corruption at the limit |
| LR-5 | 3 s writer outage against a 2 s ring (ring level **and** application level) | **PASS** — explicit `overflow` failure, exactly one callback rejected, pre-overflow PCM intact, take never committed |
| LR-6 | long endurance, jitter plus a 1 s pause every 300 s | **PASS** at 1 800 s / 7 200 s / 14 400 s simulated — zero loss, zero accumulation, no intercepted allocation |

### Measured metrics (default run)

| Scenario | sim s | produced | consumed | peak occ. | peak occ. | min headroom | ring used | drains | late | drops |
|---|---|---|---|---|---|---|---|---|---|---|
| LR-1 nominal | 30.0 | 1 439 744 | 1 439 744 | 2 048 | 42.7 ms | 93 952 (1.957 s) | 2.1 % | 1 800 | 0 | 0 |
| LR-2 jitter | 30.0 | 1 439 744 | 1 439 744 | 6 656 | 138.7 ms | 89 344 (1.861 s) | 6.9 % | 738 | 323 | 0 |
| LR-3 pause | 5.5 | 263 168 | 263 168 | 74 752 | 1 557.3 ms | 21 248 (0.443 s) | 77.9 % | 247 | 9 | 0 |
| LR-5 overflow | 3.0 | 142 336 | 142 336 | 95 744 | 1 994.7 ms | 256 (5.3 ms) | 99.7 % | 71 | 12 | 512 |
| LR-6 endurance | 1 800.0 | 86 399 488 | 86 399 488 | 51 712 | 1 077.3 ms | 44 288 (0.923 s) | 53.9 % | 71 864 | 20 143 | 0 |

In every lossless row `produced == consumed` and final occupancy is zero.

### LR-4 — real capacity of the ring

Largest burst the ring absorbs with the writer never running at all:

| callback | capacity | absorbed frames | absorbed seconds | residual headroom | nominal service periods |
|---|---|---|---|---|---|
| 64 | 96 000 | 96 000 | 2.0000 s | 0 | 60.00 |
| 128 | 96 000 | 96 000 | 2.0000 s | 0 | 60.00 |
| 256 | 96 000 | 96 000 | 2.0000 s | 0 | 60.00 |
| 512 | 96 000 | 95 744 | 1.9947 s | 256 | 59.84 |
| 1024 | 96 000 | 95 232 | 1.9840 s | 768 | 59.52 |

The absorbable burst is exactly `capacity - (capacity mod callbackFrames)`: the
only loss against the nominal 2.0 s is the partial callback that no longer fits,
never more than one buffer.

### LR-6 — long tiers

| simulated | frames produced = consumed | peak occupancy | late events | drops | wall clock (Debug) |
|---|---|---|---|---|---|
| 300 s | 14 399 488 | 52 224 | 3 328 | 0 | 3 s |
| 1 800 s (default) | 86 399 488 | 51 712 | 20 143 | 0 | 16 s |
| 7 200 s | 345 599 488 | 51 712 | 80 593 | 0 | 61 s |
| 14 400 s | 691 199 488 | 51 712 | 161 210 | 0 | 122 s |

The peak occupancy is identical at 30 minutes, 2 hours and 4 hours, and is set
by the injected 1 s pause rather than by elapsed time. **Nothing accumulates
with duration** — not occupancy, not allocations, not counter drift. The 300 s
tier peaks 512 frames higher only because its deterministic jitter sequence
differs; the bound is the stress profile, not the run length.

## 5. Worst case observed and real margin

* **Worst occupancy under nominal service:** 2 048 frames (42.7 ms) — **2.1 %**
  of the ring. Minimum headroom 1.957 s.
* **Worst occupancy under 4× jittered service:** 6 656 frames (138.7 ms) —
  **6.9 %** of the ring.
* **Worst occupancy of a lossless take:** 74 752 frames (1.557 s, LR-3), from a
  deliberately injected 1.5 s pause.
* **Hard limit:** a total writer outage of **1.9947 s ≈ 59.8 consecutive missed
  30 Hz service ticks** at 512-frame buffers, or exactly 2.000 s at ≤256-frame
  buffers. Beyond it the take fails explicitly.

So the real margin of the current design is: the UI thread may be late by about
**sixty consecutive timer periods**, and a single service pass can recover a
full 2 s backlog, because the drain loop is not rate-limited per tick.

## 6. Behaviour at overflow

Verified exactly, at both levels:

1. The producer rejects the **whole** offending callback: `droppedFrames` is
   exactly one buffer (512), `overflowEvents == 1`, and no partial frames are
   written.
2. `acceptedDeviceFrames` equals the contiguous pre-overflow prefix, and that
   prefix drains **intact and in order** — verified sample by sample on the
   drained PCM, and again after re-reading the file, where every sample must
   match the expected value and the file must end exactly at the expected
   length (EOF after the last expected frame, no trailing bytes). The file
   check compares `float` values and length, not raw bytes. There is no silent
   corruption.
3. Capture is terminal afterwards: later callbacks change no counter at all, and
   a stale writer failure cannot terminalize a newer session.
4. The application reports `RecordingPhase::failed` with
   `"Recording stopped because the capture buffer overflowed"`, calls cleanup
   exactly once, **never finalizes and never commits**, stops the transport,
   and accepts a new Record afterwards. "Never commits" is asserted
   explicitly: no source, no clip, the session stays non-dirty, and the undo
   history is unchanged — same cursor, same state token, same `canUndo`/
   `canRedo` as before the take, so the failed take contributes no Undo/Redo
   entry.

This is the existing 0.8.2 error/no-commit policy; 0.8.3-B only proves it.

## 7. RT safety of the instrumentation

The added RT work is, per capturing callback: one relaxed `fetch_add`, one
relaxed `load`, and at most one relaxed `store` (plus two more `fetch_add`s on
the single terminal overflow path). Explicitly verified:

* no intercepted `operator new`/`new[]` call in the RT region — asserted in
  every scenario, and separately for the telemetry reader itself. The default
  run covers 86.4 M frames; the 14 400 s tier, run manually for this report,
  covers 691 M. See the scope limits under *Methodology*: this is not a proof
  about `malloc`, over-aligned or `nothrow` allocation;
* no mutex, no lock of any kind — all counters are
  `std::atomic<std::uint64_t>` with `is_always_lock_free` statically asserted;
* no filesystem or device I/O, no syscalls;
* no string formatting, no logging, no exceptions (`capture`/`drain` stay
  `noexcept`);
* no unbounded work: every added operation is O(1) and branch-bounded.

Sanitizers, all clean: ThreadSanitizer on the two-thread SPSC test and on
LR-1…LR-6; AddressSanitizer + UndefinedBehaviorSanitizer on both suites.

## 8. Remaining risks

| Priority | Risk | Why it is still open |
|---|---|---|
| P0 | **Real UI-timer lateness is still unmeasured.** | The harness proves the ring absorbs ~2 s of writer outage; it does not establish how late JUCE's 30 Hz timer actually gets in this application. Synchronous main-thread work that shares the timer — project save, audio import/decode, waveform rebuild, `pollDeviceLifecycle` reprepare — or a native modal dialog could plausibly approach that budget on slow storage. This is now the only thing standing between the measurement and a verdict of sufficient. |
| P0 | No physical long take yet (LR-03 of the 0.8.3-A matrix). | 30/60/120-minute takes on real interfaces remain to be run; the simulation cannot show audible continuity or real driver behaviour. |
| P1 | Real storage write latency is not modelled. | Lateness is injected into the schedule; it is not derived from the cost of a real write to a slow, USB or network volume. The production short-write loop is exercised, but not under real back-pressure. |
| P1 | JUCE's own WAV writer is outside the simulated path. | `writeFromAudioSampleBuffer` and WAV header formatting are covered by the existing media/IO fault tests, not by the endurance harness. |
| P1 | Telemetry is not surfaced anywhere. | The counters stop at `RealtimeAudioEngine::recordingTelemetry()`; no application accessor, UI or diagnostic log consumes them, so a user-reported glitch still cannot be explained from a session. Closing the 0.8.3-A P2 gap means adding a consumer *and* the accessor layer it justifies. |
| P1 | The harness models `serviceRecording()` instead of calling it. | `ServiceWriter` mirrors the adapter's drain loop (bounded chunks until empty, stop at the first write failure) but the adapter's own function is never invoked. The measured headroom is therefore a property of the ring plus a faithful *model* of the service: if the real loop ever gained a per-tick cap or an early break, these scenarios would stay green. A seam that drives the real `serviceRecording()` is deferred. |
| P2 | `setServiceBudgetFrames()` is a setter where a derivation would do. | The budget is per-take configuration that could be computed inside `prepare()` from capacity, device rate and buffer size, removing two public APIs. It is also the one piece of telemetry state not zeroed with the ring, so a direct `RealtimeCapture::begin()` user inherits a stale or zero budget and gets a meaningless `lateServiceEvents`. Production is unaffected (the adapter re-installs it per take). Deferred. |
| P2 | Every endurance figure is 48 kHz. | Only the callback size is varied (64…1024). The headroom in *seconds* is rate-invariant by construction (capacity = rate × 2 s) and LR-4 proves a rate-independent quantisation rule, so the conclusions scale, but no scenario validates 44.1/88.2/96 kHz. The existing capture tests cover 44 100 and 44 117 Hz for integrity, not endurance. Deferred. |
| P2 | The writer-failure path is modelled but never exercised. | `ServiceWriter` has a write-failure branch and the scenarios assert it never fires, but no scenario injects `failWrite`, and the model does not call `failRecording(writerFailed)` the way the real service does. Writer failure under sustained endurance is deferred. |
| P2 | Device/configuration change during capture (DF-01…DF-04). | Out of scope for 0.8.3-B; unchanged from the 0.8.3-A audit. |

## 9. Conclusion on the 30 Hz UI-serviced writer

The design is **structurally sound and fails safely**: nominal operation uses
2.1 % of the ring, 4× jitter uses 6.9 %, four simulated hours accumulate
nothing, loss requires a continuous ~2 s writer outage, and beyond that limit
the take fails explicitly with the pre-overflow audio intact and nothing
committed.

What is *not* yet established is the real distribution of UI-thread lateness in
this application, which is the quantity the 2 s budget is spent against. The
margin is large in relative terms and small in absolute terms: about sixty
timer periods, and a single blocking main-thread operation is the whole budget.

Verdict: **DISEÑO ACTUAL JUSTO / REQUIERE MÁS PRUEBAS.** The architecture should
not be changed on this evidence; the next measurement, not a rewrite, is what
closes the question.

### If the next measurement shows it is insufficient

In order of cost, with the evidence to justify each:

1. **Raise `recordingCaptureRingSeconds`.** It is now one named constant, and
   LR-4 measures the resulting margin for any value. 8 s stereo float at 48 kHz
   is ~3 MB — negligible against the existing 512 MB preparation budget — and it
   buys a 4× larger outage budget with no change to the writer model, the state
   machine or the RT contract.
2. **Instrument the real timer.** Record observed service intervals (the
   telemetry already carries the backlog side of this) during the application's
   own worst-case main-thread operations, and publish the distribution.
3. **Only then** consider moving the drain off the UI timer to a dedicated
   service thread. The SPSC ring already supports a separate consumer thread —
   `concurrentProducerConsumer` and the TSan runs prove it — so this stays a
   service-ownership change rather than a redesign of capture.

## 10. Running it

```sh
cmake --build build --target vitadaw_recording_endurance_tests
./build/vitadaw_recording_endurance_tests                      # 1 800 s simulated
VITADAW_ENDURANCE_SECONDS=14400 ./build/vitadaw_recording_endurance_tests
ctest --test-dir build -R recording                            # full recording battery
```

The suite prints its metric tables on success; those tables are the source of
every number in this document.
