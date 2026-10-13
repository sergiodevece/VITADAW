# VitaDAW 0.8.3-C2-1 — Device Failure Characterization Tests

Test-only phase following audit 0.8.3-C1
([`audit-recording-device-reconfiguration-0.8.3C1.md`](audit-recording-device-reconfiguration-0.8.3C1.md)),
at `57d18d7`. **No production code was changed.** The phase adds one test
executable and its CMake registration, and establishes which C1 contracts hold
today and which fail reproducibly.

**Outcome.** 19 cases: **15 GREEN** (contracts that hold) and **4 RED**
(contracts that fail today, each reproduced past all its preconditions). The
60 pre-existing tests still pass. Running the cases against the real JUCE code
also **corrected two statements of C1** (§6.1).

## 1. Test matrix

`tests/RecordingDeviceFailureTests.cpp`, executable
`vitadaw_recording_device_failure_tests <case>`, one CTest per case named
`vitadaw_device_failure_<case>`. RED cases carry the CTest label
`characterization-red`.

| Case | C1 ref. | Kind | Initial state | Events injected | Expected | Result | What it demonstrates |
|---|---|---|---|---|---|---|---|
| `t1_device_stop_fails_take_and_recovers` | T-1, T-6 | GREEN | capturing, 20 blocks | uncontrolled device stop, timer; device restart, timer; new take | `failed / deviceLost`, transport stopped, no commit, writer closed, temporary == accepted frames; device re-certified; new take published intact | **PASS** | Loss is fail-safe; recovery after a *stop* works without restart |
| `t2a_juce_drops_device_errors` | T-2 | GREEN (JUCE probe) | capturing | device reports `audioDeviceError` through JUCE | the error never reaches the adapter; a stream that keeps running yields an intact take | **PASS** | JUCE 9.0.2 swallows device errors (§6.1) |
| `t2b_direct_error_path_is_fail_safe` | T-2 | GREEN | capturing | `adapter.audioDeviceError()` called directly (the existing seam) | take failed, retained, not published | **PASS** | The adapter's own error handling is fail-safe *if* reached; recovery printed (§6.3) |
| `t3_restart_fails_take_without_silent_continuation` | T-3 | GREEN | capturing, 18 blocks | stop + start (JUCE's restart), blocks after restart | failed at `aboutToStart`; **no frame after the restart is appended**; retained == frames before | **PASS** | No silent continuation across a restart |
| `t4_divergence_fails_before_reprepare` | T-4 | GREEN | capturing | device reports a new rate before restarting; blocks; timer | failed before re-prepare; ring not reset before drain; engine then re-prepared | **PASS** | Divergence path is fail-safe and keeps every accepted frame |
| `t5_buffer_change_rejected_while_capturing` | T-5 | GREEN | capturing | `SetAudioBufferSize{512}` | rejected, no `open()`, take completes normally | **PASS** | Voluntary reconfiguration is refused while capturing (C1 tested only *prepared*) |
| `t8_device_loss_during_finalization` | T-8 | GREEN (invariant) | Stop consumed | device stop before the timer | committed ⇔ complete, published and intact; otherwise failed with a reason | **PASS** — observed: committed and published | A take completed before the loss is finalized consistently |
| `c_rate_window_then_restart_fails_take` | H-2 | GREEN (characterization) | capturing at 48 kHz | 100 blocks at 44.1 kHz host spacing while the device still reports 48 kHz; timer; late restart | the window goes undetected; the restart fails the take; nothing published | **PASS** — observed: 25 600 window frames in the retained temporary under a 48 kHz header | Late restart is fail-safe; retained media is contaminated |
| `e_suspected_xruns_do_not_abort` | H-1 / D1 | GREEN | capturing | xrun counter −1, 0, increments, reset | take completes intact | **PASS** | D1: a suspected xrun alone is not a failure |
| `e_missing_host_time_is_not_a_failure` | H-1 / D2 | GREEN | capturing | `hostTimeNs == nullptr` throughout | take completes intact | **PASS** | Absent host time is not a discontinuity |
| `f_interface_loss_fails_take_and_leaves_no_device` | DF-1, M-2 | GREEN | capturing on Interface A | Interface A removed from the device list (JUCE's own handling) | take failed and retained; JUCE opens **no** fallback; device error reported; Record refused | **PASS** | No silent source change on this path (§6.1) |
| `g1_stop_then_device_loss_publishes` | T-7 | GREEN | capturing | Stop consumed, then device stop | complete, published intact | **PASS** | Stop wins when it came first |
| `g2_device_loss_before_stop_is_consumed` | T-7 | GREEN | capturing | Stop queued, device stop before any callback | failed, retained, not published | **PASS** | An unconsumed Stop cannot publish |
| `g3_overflow_then_device_loss` | G | GREEN | ring overflowed | device stop, timer | failed **as overflow** (first terminal wins), retained, telemetry reset | **PASS** | Terminal precedence is first-wins |
| `g4_stop_pending_then_restart` | G | GREEN | capturing, Stop queued | device restart | failed `deviceLost`, not published | **PASS** | A restart cancels a queued Stop safely |
| `red_h2_stop_inside_rate_window` | H-2 | **RED** | capturing at 48 kHz | 200 blocks at 48 kHz spacing, 200 at 44.1 kHz spacing (device still reports 48 kHz), Stop inside the window, timer | not published | **FAIL (exit 3)** — observed: complete, published | **Reproduces H-2** |
| `red_h1_host_time_gap` | H-1 | **RED** | capturing | 100 blocks, then a **1 s** host-time gap, 100 blocks, Stop | failed, not published | **FAIL (exit 3)** — observed: complete, published | **Reproduces H-1** |
| `red_m1_stalled_callbacks` | M-1 | **RED** at C2-1 → **GREEN since C2-2**, renamed `m1_stop_pending_stall_fails_take` | capturing, 10 blocks | callbacks stop with no notification; Stop; ≥ 2.5 s and ≥ 80 timer passes; Cancel | terminal state without quitting | C2-1: **FAIL (exit 3)** — Stop and Cancel "accepted", capture still `capturing`, writer open. C2-2: **PASS** — failed `deviceStalled` 1014 ms after the last callback | **Reproduced M-1; now its acceptance test** |
| `red_r2_reconnected_interface_unusable` | I-3 → R-2 | **RED** | take failed after Interface A vanished | Interface A reappears; 5 timer passes; Record | new take possible without restart | **FAIL (exit 3)** — observed: no device opened, Record rejected "No audio device is available" | **New defect R-2**, confirming C1's I-3 |

Every RED case exits **3** only at its contract assertion, after printing
`OBSERVED:` with the measured state. A precondition failure exits **1**,
like any broken test, so a RED result cannot be confused with a setup error.

## 2. Coverage before C2-1

As established in C1 §8.1. Device loss, restart and divergence with an
**active** take were proven only in Core (`RecordingCaptureTests`), never
through the real adapter and JUCE. Device errors were tested only through a
**direct** call to `adapter.audioDeviceError()` — a seam that, as §6.1 shows,
hid the fact that JUCE never delivers them. The buffer-change rejection was
tested with a *prepared* take only.

## 3. Infrastructure reused

- **The adapter's existing test friend `PersistenceIntegrationAccess`.** The new
  executable defines its own body for it, as every suite does, so **no
  production declaration was added**.
- **The virtual-device pattern** of `InputMonitoringDeviceLifecycleTests`: no
  thread; `start` stores JUCE's callback; `stop`, `restart` and `render` go
  through that callback. Extended with:
  - `context.hostTimeNs`;
  - a settable `getXRunCount()`;
  - a device type with two interfaces that can disappear and reappear through
    `callDeviceChangeListeners()`.
- **Real code on the path:** `JuceAudioDeviceAdapter`, `DawApplication`,
  `CommandDispatcher`, and **JUCE's own `AudioDeviceManager`**, including its
  callback forwarding (`CallbackMaxSizeEnforcer` → `CallbackHandler`), its lock,
  and its device-list fallback (`audioDeviceListChanged`,
  `initialiseDefault`, `insertDefaultDeviceNames`).
- **The timer order** of `JuceApplication`: `pollDeviceLifecycle()` and then
  `synchroniseTransport()`, reproduced by `Rig::tick()`.

## 4. New tests

One file, `tests/RecordingDeviceFailureTests.cpp` (19 cases), and 40 lines in
`CMakeLists.txt` registering them. Each case's comment states its scenario,
initial state, injected events, expected sequence, and any model it relies on.
Every take uses an exact dyadic sample pattern, so "retained temporary ==
accepted frames" and "published == the take" are checked sample by sample on
the files on disk.

## 5. RED / GREEN results

| Battery | Result |
|---|---|
| `ctest` (all) | 79 tests: **75 PASS, 4 FAIL** — the 4 failures are exactly the `characterization-red` cases |
| `ctest -LE characterization-red` (green baseline) | **75/75 PASS**: the 60 pre-existing tests + 15 new GREEN |
| `ctest -L characterization-red` | 4/4 FAIL as expected, each exit 3 with its `CONTRACT VIOLATED [H-2 / H-1 / M-1 / R-2]` |
| ASan + UBSan, all 19 cases | 0 issues; GREEN exit 0, RED exit 3 |
| TSan, all 19 cases | 0 warnings; GREEN exit 0, RED exit 3 |

No pre-existing test regressed. No assertion was weakened. Three
precondition failures during development were each traced to a cause before
anything was changed:
- g3 was my own loop bug: the take is `prepared` until the first callback.
- t2 and f exposed the two C1 corrections below.

## 6. Causes identified

### 6.1 Corrections to audit C1

**JUCE 9.0.2 never delivers device errors to VitaDAW.** JUCE gives every device
`AudioDeviceManager`'s `CallbackMaxSizeEnforcer` as its callback
(`juce_AudioDeviceManager.cpp:157, 196`). It forwards `audioDeviceAboutToStart`,
the IO callback and `audioDeviceStopped`, but **not** `audioDeviceError`, whose
base implementation is empty (`juce_AudioIODevice.cpp:53`). CoreAudio's errors
(`juce_CoreAudio_mac.cpp:1494, 1520`) are therefore lost.

- The `error` branch of `pollDeviceLifecycle()` is **unreachable in
  production**.
- C1 §3.1 and §6.2 described device errors as a notified, fail-safe event. That
  holds only when JUCE also stops the device, as in "Failed to start device".
  An error that leaves the device silent becomes the M-1 stall.

**This raises the priority of M-1:** VitaDAW cannot rely on error
notifications at all.

**The silent fallback described as M-2 does not happen on this path.** When
the interface in use disappears, JUCE's `audioDeviceListChanged()` calls
`initialiseDefault(preferredName, &currentSetup)`. That copies the current
setup, which **names** the vanished device, and `insertDefaultDeviceNames()`
fills only *empty* names (`juce_AudioDeviceManager.cpp:271-311`). JUCE
therefore retries the missing device and ends with **no device**.

VitaDAW always holds explicit device names, so the next Record is refused
rather than recorded from another source (`f_…` GREEN). M-2, as written in C1,
is **not reproduced**.

A weaker, untested form remains: the first take after start-up records from
the system default input without saying which one it is. Because the fallback
does not happen, the actual consequence is R-2.

### 6.2 Defects reproduced

| ID | Cause (code) | Reproduced by |
|---|---|---|
| **H-1** | `audioDeviceIOCallbackWithContext` ignores `context.hostTimeNs` (`JuceAudioDeviceAdapter.cpp:2547-2550`) | `red_h1_host_time_gap` |
| **H-2** | Callbacks keep running at the new hardware rate while JUCE reports the old one (`juce_CoreAudio_mac.cpp:1858-1871, 2101-2103`); the adapter passes its cached rate | `red_h2_stop_inside_rate_window` (published) and `c_rate_window_then_restart_fails_take` (contaminated retained media) |
| **M-1** | No liveness check; Stop and Cancel are RT commands (`RealtimeAudioEngine.cpp:642-650`) | `red_m1_stalled_callbacks` |
| **R-2** (new) | JUCE reacts to list changes only while it has a current device (`juce_AudioDeviceManager.cpp:273`); VitaDAW has no reopen or device-selection path | `red_r2_reconnected_interface_unusable` |

### 6.3 Observations without a RED test

- **Latent L-3.** If an error *were* delivered, the error branch would detach the
  callback, and nothing re-attaches it: `t2b` observes "callback registered: no,
  engine not operational, Record rejected" after a device restart. Unreachable
  with JUCE 9.0.2 (§6.1), so it is not a RED test.
- **Same path.** A Record whose preflight succeeds but whose RT request is
  rejected leaves an empty retained temporary ("recording media retained for
  recovery"). Only reachable through L-3.
- **T-8.** A take completed before the device loss is committed and published.
  The project commit does not depend on the device. This is consistent and
  correct.

## 7. Limitations of the fakes

- The virtual device has **no real timing**: host times are synthetic and
  perfectly regular. Real drivers jitter, so a C2-4 detector must be calibrated
  physically (S-2/S-3).
- **H-2 is a model of JUCE 9's timing, read from its source.** That source keeps
  reporting the old rate until an asynchronous restart. How long that window
  lasts on real hardware, and whether CoreAudio keeps the block size, is not
  proven here.
- **Host-time continuity does not prove the effective sample rate by itself.**
  The H-2 test shows only that the information VitaDAW would need is present in
  the callback context and ignored.
- **The xrun counter is a settable integer.** Nothing here establishes what a
  CoreAudio overload does to input data. No test claims that an xrun means lost
  input.
- **M-1 is modelled as callbacks that simply stop.** Which real events cause it
  (sleep/wake, a dead device that stays enumerated, an error without a stop)
  needs hardware (S-6).
- **R-2 relies on the fake reporting the interface under the same name.** A real
  device might reappear with a different identity.
- **Partial input loss on a running device** (an aggregate member,
  DF-2 / DF-5) is not modelled. The fake cannot represent CoreAudio's
  aggregate behaviour.

## 8. Real JUCE versus simulated

| Exercised for real | Simulated |
|---|---|
| `JuceAudioDeviceAdapter` (callbacks, `pollDeviceLifecycle`, attach/detach, re-prepare, preflight, writer, cleanup, publication) | Device timing and host timestamps |
| `DawApplication` and `CommandDispatcher` policy | CoreAudio's restart timing and its rate-change window (modelled from JUCE source) |
| JUCE `AudioDeviceManager`: callback forwarding, locks, device-list fallback | Device enumeration and identity |
| WAV writing, header finalization, no-replace publication, retained temporaries | Xrun semantics |
| Ring, engine lifecycle gate, terminal CAS | Real device death and stalls |

## 9. False-positive risks

| Case | Risk | Mitigation |
|---|---|---|
| `red_h1_host_time_gap` | A detector tuned for real jitter might accept small gaps | The gap is deliberately extreme (1 s, about 187 cycles), so no reasonable calibration accepts it |
| `red_h2_stop_inside_rate_window` | Short windows might not be enough evidence for a calibrated estimator | 200 blocks (≈ 1.16 s) at the wrong spacing; adjust only with physical data |
| `red_m1_stalled_callbacks` | Wall-clock dependency | ≥ 2.5 s and ≥ 80 passes, well above the 1 s watchdog C1 proposed; it does not dictate the threshold |
| `t2a_juce_drops_device_errors` | Pins a JUCE behaviour | Intended: a JUCE upgrade that changes it must be noticed and re-audited |
| `e_suspected_xruns_do_not_abort` | — | Encodes D1. It must stay green once the xrun monitor exists |

## 10. Recommended order for C2-2 onward

1. **C2-2 — liveness watchdog (M-1).** Now the primary safety net, because JUCE
   delivers no device errors (§6.1). The RED test is ready. Implement on the
   message thread with the existing direct `failRecording()`.
2. **C2-R2 — reopen a reconnected interface (R-2).** New item. Minimal
   contract: when the device-list changes and no device is open, reopen the
   **same named** device from the last certified setup, never a different one,
   and never resume a take. Whether this is automatic or behind an explicit
   "Reconnect" action is a product choice; either satisfies contract C. The RED
   test is ready.
3. **C2-3 — xrun monitor, diagnostic only (D1).** Telemetry and a message; no
   abort. `e_suspected_xruns_do_not_abort` must stay green.
4. **C2-4 — host-time continuity (H-1, H-2).** RED tests ready. Needs physical
   calibration (S-2/S-3) before thresholds are fixed. The contaminated-media
   characterization (`c_…`) should then show the window detected at its start.
5. **C2-5 / C2-7 — reason fidelity and check ordering (L-1, L-2).** Low risk.
6. **C2-6 — input-source visibility.** Lower priority than C1 assumed, because
   the fallback path does not exist (§6.1). It remains useful for the first
   take after start-up.
7. **L-3 — error-branch recovery.** Only if a future JUCE delivers errors; the
   `t2a` probe will flag that.

C2-2 can start safely. Its acceptance test exists, the green baseline is
75/75, and it touches neither the RT path nor the policies the other items
depend on.

## Status after 0.8.3-C2-2

- **M-1 fixed** by the callback watchdog
  ([`audit-recording-watchdog-0.8.3C2-2.md`](audit-recording-watchdog-0.8.3C2-2.md)).
  `red_m1_stalled_callbacks` became `m1_stop_pending_stall_fails_take` (GREEN);
  its original contract assertion is preserved, and the label
  `characterization-red` was removed.
- **Still RED, unchanged:** H-1, H-2, R-2.
- **New RED from C2-2's analysis** (pre-existing defects, not regressions): M-1b
  (stall while `prepared`) and P-1 (a cleaned-up prepared session revived by its
  queued `beginRecord`).
- **New TSan-only reproduction:** `c1_prepared_transition_under_concurrency`
  (label `characterization-tsan`), the pre-existing `begin()`/`snapshot()` data
  race C-1.
- **Batteries:** 94 tests; 89/89 green baseline; the 60 original tests 60/60;
  5 expected RED.

## Status after 0.8.3-C2-2b

- **M-1b and P-1 fixed** by the state-protocol hardening
  ([`audit-recording-state-hardening-0.8.3C2-2b.md`](audit-recording-state-hardening-0.8.3C2-2b.md)).
  - `red_m1b_prepared_stall` became `m1b_prepared_stall_terminalizes`.
  - `red_p1_stale_begin_revives_cleaned_session` became
    `p1_stale_begin_cannot_revive_cleaned_session`.
  - Both are GREEN, their contract assertions are preserved, and the label
    `characterization-red` was removed.
- **C-1 fixed:** `c1_prepared_transition_under_concurrency` is clean under
  ThreadSanitizer, and the label `characterization-tsan` was removed.
- **New GREEN device cases:** six, for the `prepared` watchdog, Stop before the
  first callback, and repeated failure and recovery.
- **New Core suite:** `vitadaw_recording_state_protocol_tests`.
- **Still RED, unchanged:** H-1, H-2, R-2.
- **Batteries:** 101 tests; 98/98 green baseline; the 60 original tests 60/60;
  3 expected RED.

The numbers in §1–§9 above are the C2-1 record and are left as they were.

## Running

```sh
cmake --build build --target vitadaw_recording_device_failure_tests
ctest --test-dir build -LE characterization-red   # green baseline: must be 100 %
ctest --test-dir build -L characterization-red    # expected RED until C2 lands
build/vitadaw_recording_device_failure_tests <case> # one case, with OBSERVED output
```
