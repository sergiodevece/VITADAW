# VitaDAW 0.8.3-C2-2 — Recording Callback Watchdog

Fixes finding **M-1** of audit 0.8.3-C1: a take stuck in `capturing` when the
device stops calling back without any notification. Baseline `57d18d7`, on top
of the uncommitted C1 and C2-1 work. **The real-time path is unchanged**: no
file of the RT core is touched, and the audio callback is byte-identical to
`57d18d7`.

Traceability: M-1 (C1 §7) → `red_m1_stalled_callbacks` (C2-1, RED) →
`m1_stop_pending_stall_fails_take` (C2-2, GREEN). The original contract
assertion is kept verbatim at the end of that case.

> **Update, 0.8.3-C2-2b.** The findings in §9 (M-1b, P-1, C-1) are fixed, and
> the watchdog now also supervises `prepared`. That phase changes the RT state
> protocol of `RealtimeCapture`; see
> [`audit-recording-state-hardening-0.8.3C2-2b.md`](audit-recording-state-hardening-0.8.3C2-2b.md).
> The rest of this document is the C2-2 record.

## 1. Exact cause of M-1

1. **Nothing reports the stall.** JUCE 9.0.2 marks a dead CoreAudio device
   `running = false` and notifies nobody (`juce_CoreAudio_mac.cpp:1416-1420`).
   JUCE also drops `audioDeviceError` before it reaches VitaDAW
   (`CallbackMaxSizeEnforcer`, C2-1 §6.1).
2. **The only exits need a callback.** While a take is `capturing`, Stop and
   Cancel are RT commands: `tryRequestStop()` enqueues; `tryCancelRecording()`
   enqueues unless the take is still `prepared` (`RealtimeAudioEngine.cpp:642-650`).
   A command is consumed only inside `processBlock`.
3. **Nothing measured liveness.** There was no watchdog. The application kept
   the take busy, the writer stayed open, and the UI offers no Cancel control.

## 2. Mechanism

| Element | Choice | Why |
|---|---|---|
| Heartbeat | The **existing** producer counter `acceptedFrames_` (`RecordingSnapshot::acceptedDeviceFrames`) | Every capturing callback increments it, digital silence included. It is a lock-free atomic the producer already owns, so the callback gains **no instruction at all** |
| Evaluation | `JuceAudioDeviceAdapter::superviseRecordingCallbacks()`, called from `serviceRecording()` on the message thread, before the drain | It sits next to the existing writer-failure supervision. Running before the drain means that if it fails the take, the same pass still drains every frame already published |
| Progress rule | Fail when, across two observations, the session is the same, the phase is `capturing`, and the accepted-frame count has not changed for at least **T** | A late message thread can delay detection but cannot cause it (§3) |
| Session identity | State keyed by `RecordingSessionId`; re-armed on any session or count change; cleared whenever the take is not `capturing` | Heartbeats of different takes never mix |
| Terminal transition | The existing direct `RealtimeAudioEngine::failRecording(session, deviceStalled)`, which is the capture's terminal CAS | One terminal transition; first wins against Stop, overflow, cancel or device events |
| Cleanup | Unchanged: `DawApplication::synchroniseRecording()` sees `failed` in the same timer tick, then reports, runs `discardRecordingWithDiagnostics()` (closing the writer and retaining the temporary), stops the projected transport and clears the session | No new cleanup path |
| Reason | New `RecordingFailure::deviceStalled` → "Recording stopped because the audio device stopped delivering audio" | Explicit and distinct from `deviceLost`. The reason is never serialised, so adding it has no compatibility impact |

**Phase scope: `capturing` only.** A `prepared` take is deliberately not
supervised (§9: M-1b, P-1). *Since C2-2b: `prepared` and `capturing`; progress
also includes the phase (C2-2b §6).*

**Production diff:**

| File | Change |
|---|---|
| `RecordingTypes.h` | +3: the enumerator |
| `DawApplication.cpp` | +2: the message |
| `JuceAudioDeviceAdapter.h` | +14: watchdog state and method |
| `JuceAudioDeviceAdapter.cpp` | +56: threshold function, one call, supervisor |
| `RecordingServiceDiagnostics.h` | +1: diagnostic name |

Insertions only. `RealtimeCapture`, `RealtimeAudioEngine` and
`audioDeviceIOCallbackWithContext` are untouched.

## 3. Timeout and its justification

`T = max(1 s, 10 × deviceBufferFrames / deviceSampleRate)`, both taken from the
take's own preflight placement.

**A false positive needs a real gap of at least T.** The watchdog stores the
time at which it *first observed* the current count. A callback after that
moment changes the count and re-arms it. Firing therefore requires that no
callback arrived between that first observation and the evaluation at least T
later. Message-thread lateness only moves observations later; it cannot invent
a gap. B2 measured message-thread delays of up to 643 ms while capture continued
correctly, and `m1_message_thread_delay_with_live_callbacks` blocks the timer
for 1.5 s, beyond T, without a failure.

**Why a 1 s floor:**
- it is far above one callback period for any common buffer (at most 93 ms at
  4096 frames / 44.1 kHz);
- it is far above OS scheduling jitter on an audio thread;
- sub-second interruptions that resume are continuity questions with their own
  policy (D1/D2, C2-4), so the watchdog stays out of that domain;
- it fits the C2-1 acceptance bound of 2.5 s with a wide margin.

**Why ten periods:** so that very large device buffers never trip it on a single
late callback.

| Buffer | 44.1 kHz | 48 kHz | 96 kHz |
|---|---|---|---|
| ≤ 2048 | 1 s | 1 s | 1 s |
| 4096 | 1 s (10 periods = 0.93 s) | 1 s (0.85 s) | 1 s (0.43 s) |
| 8192 | **1.86 s** | **1.71 s** | 1 s (0.85 s) |

**Measured.** In the virtual-device tests (256 frames / 48 kHz, T = 1 s) the
take is failed **1014 ms after the last callback**: T plus at most one timer
pass.

**Not a hard time guarantee.** Detection happens at T, plus up to one service
period (about 33 ms), plus any delay of the message thread itself. A message
thread blocked for X seconds during a stall delays detection by up to X.

## 4. Detection limits

| Interruption | Outcome |
|---|---|
| Callbacks absent for longer than T + one observation interval, while the message thread runs | Detected; the take fails as `deviceStalled` |
| Callbacks absent for less than T, then resume | **Never claimed by the watchdog, by design.** `m1_short_pause_is_left_to_continuity_checks` shows a 400 ms pause: the take is published with an undetected splice. This is the H-1 window and stays RED until C2-4 |
| Callbacks absent for at least T but resuming before an observation sees the full interval | May or may not be detected, depending on where the observations fall. Only gaps longer than T + the observation interval are certain |
| A stall that starts and ends while the message thread is blocked | Not detected: the first observation afterwards already sees progress. This is a splice, for C2-4 |
| System sleep and wake | Not verified. It depends on how `std::chrono::steady_clock` and CoreAudio behave across sleep → physical smoke S-6 |
| A `prepared` take that never receives its first callback | **Not supervised** (§9, M-1b). *Supervised since C2-2b: fails as `deviceStalled` after T* |

**M-1 does not resolve H-1.** The watchdog guarantees liveness, not continuity.

## 5. Terminal behaviour

1. `failRecording(session, deviceStalled)` performs the capture's terminal CAS.
2. In the same timer tick, the application shows "Recording stopped because the
   audio device stopped delivering audio; recording media retained for
   recovery".
3. Cleanup closes the writer, retains the temporary with exactly the accepted
   frames and its `temporary` recovery marker, publishes nothing, commits
   nothing, and stops the projected transport.
4. **If the device resumes**, the capture is `idle`; the already-consumed
   `beginRecord` cannot run again; a queued Stop or Cancel is inert. The take is
   not revived and the transport does not restart
   (`m1_device_resumes_after_stall_failure`).
5. **A new take needs an explicit Record.** Its preflight detaches and
   re-attaches the callback, which closes the lifecycle gate and invalidates
   every older queued command (`CommandLifecycleGate::close`).

## 6. Concurrency

| Crossing | Result | Evidence |
|---|---|---|
| Watchdog and Stop | Stop consumed first: the take completes; the watchdog's CAS is a no-op. Stop unconsumed: the watchdog fails the take, nothing is published | `m1_stop_pending_stall_fails_take`; C2-1 `g1`/`g2` |
| Watchdog and Cancel | Cancel queued while capturing cannot run; the watchdog fails the take; the Cancel is inert on resume | `m1_cancel_pending_stall_fails_take` |
| Watchdog and overflow | Mutually exclusive by construction: overflow needs live callbacks, the watchdog needs none. The first terminal transition keeps its reason | `m1_overflow_wins_over_watchdog` |
| Watchdog and `audioDeviceStopped` | The timer runs the lifecycle poll before the service pass, so a pending notification wins (`deviceLost`). A notification after the watchdog fired finds the take terminal | `m1_watchdog_and_stop_notification` |
| Watchdog and a late callback | A callback before T re-arms the watchdog. A callback during the transition either is accepted before the CAS (and drained in the same pass) or finds the take terminal and is refused | `m1_late_callback_rearms_watchdog`; `m1_concurrent_callbacks_then_stall` (own thread, TSan-clean) |
| Watchdog and shutdown | The shutdown path terminalizes and retains the take; the watchdog plays no part | `m1_shutdown_during_stall` |
| Watchdog and a new session | State is keyed by session and cleared outside `capturing` | `m1_device_resumes_after_stall_failure` |

**RT restrictions.** The watchdog never runs in the callback. Its only effect
on RT-shared state is one atomic CAS through `failRecording()`, the same path a
writer failure already uses. It frees nothing: the writer belongs to the
message thread, and ring storage is released only by a later preflight, after
the callback has been detached under JUCE's callback lock. No lock, allocation,
I/O or string work is added to the callback.

**Residual race, shared by every failure path and unchanged here.** A callback
that publishes frames after the last drain and before cleanup resets the ring
loses those frames from the retained temporary. They are never published. This
can only happen if a callback arrives exactly at the transition. For the
watchdog that is after at least one second of silence.

## 7. Tests

`tests/RecordingDeviceFailureTests.cpp`, CTest names
`vitadaw_device_failure_<case>`.

| Case | Scenario | Result |
|---|---|---|
| `m1_stop_pending_stall_fails_take` | formerly `red_m1`; Stop pending, then a stall | **PASS** — fails at 1014 ms, not before T; message; retained; no clip |
| `m1_sustained_stall_fails_take` | stall with no command | **PASS** |
| `m1_cancel_pending_stall_fails_take` | Cancel pending, then a stall; device resumes | **PASS** — stale Cancel inert |
| `m1_continuous_callbacks_never_trip` | 1.6 s of callbacks | **PASS** — published intact |
| `m1_silent_input_is_not_a_stall` | 1.6 s of digital silence | **PASS** — silent take published |
| `m1_message_thread_delay_with_live_callbacks` | timer blocked 1.5 s, callbacks live | **PASS** — no false failure |
| `m1_late_callback_rearms_watchdog` | 700 ms quiet, one callback, stall | **PASS** — T measured from the late callback |
| `m1_concurrent_callbacks_then_stall` | callbacks on their own thread, then a stall | **PASS**, TSan clean |
| `m1_overflow_wins_over_watchdog` | overflow, then silence | **PASS** — reason stays overflow |
| `m1_watchdog_and_stop_notification` | both orders | **PASS** |
| `m1_shutdown_during_stall` | shutdown during a sub-threshold stall | **PASS** |
| `m1_device_resumes_after_stall_failure` | device resumes; new take | **PASS** — no revival; new take published |
| `m1_short_pause_is_left_to_continuity_checks` | 400 ms pause | **PASS** — watchdog silent; splice printed for C2-4 |
| `red_m1b_prepared_stall` | stall before the first callback after Record | **RED (exit 3)** — stuck in `prepared` (§9) |
| `red_p1_stale_begin_revives_cleaned_session` | cleaned prepared take, device resumes | **RED (exit 3)** — revived dead session (§9) |
| `c1_prepared_transition_under_concurrency` | `beginRecord` consumed on the callback thread while the timer snapshots | **PASS**; under TSan reproduces C-1 (label `characterization-tsan`) |

*Since C2-2b:*

- `red_m1b_prepared_stall` is now `m1b_prepared_stall_terminalizes`, and
  `red_p1_stale_begin_revives_cleaned_session` is now
  `p1_stale_begin_cannot_revive_cleaned_session`. Both are GREEN, with their
  contracts preserved.
- `c1_…` is TSan-clean and no longer labelled.
- The battery figures below are the C2-2 record; current ones are in C2-2b §10.

**Batteries:**

| Battery | Result |
|---|---|
| `ctest` (all) | 94 tests: **89 PASS, 5 FAIL**, exactly the `characterization-red` cases: H-1, H-2, R-2 (unchanged from C2-1) and M-1b, P-1 (new, §9) |
| `ctest -LE characterization-red` | **89/89** |
| The 60 pre-0.8.3-C tests | **60/60** |
| ASan + UBSan, all 31 device cases + capture, application, media-integration and endurance suites | 0 issues |
| TSan, same set | 0 issues, except `c1_prepared_transition_under_concurrency`: 2 reports, the pre-existing C-1 |

## 8. Retention evidence

Every failing scenario goes through `checkFailedAndRetained`, which checks all
of the following:
- the take is `failed` and the reason is shown to the user;
- nothing is committed, the session is not dirty, nothing is published;
- the writer is closed;
- the retained temporary on disk holds **exactly the accepted frames, sample for
  sample**.

`m1_device_resumes_after_stall_failure` additionally re-reads the temporary
after the device resumes, and it is unchanged.

## 9. Residual findings and the decision they need

> **Status: implemented in 0.8.3-C2-2b**, with a different design from the
> bundle proposed below:
>
> - `begin()` claims and publishes with CASes on a status word tagged with the
>   session, instead of a `consumeCommands` precondition;
> - auto-prepare is removed for every caller;
> - Cancel and every failure path are session-scoped.
>
> See [`audit-recording-state-hardening-0.8.3C2-2b.md`](audit-recording-state-hardening-0.8.3C2-2b.md).

Working out where the watchdog may safely act exposed three pre-existing
defects around the `prepared` phase. None was introduced by C2-2; all are
reproduced.

| ID | Defect | Evidence | Consequence today |
|---|---|---|---|
| **M-1b** | A stall before the first callback after Record leaves the take `prepared` forever: Stop is an RT command and the UI has no Cancel | `red_m1b_prepared_stall` | Same symptom as M-1, on retrying Record against a dead device |
| **P-1** | After a prepared take is terminalized and cleaned up, its still-queued `beginRecord` re-prepares and begins the dead session when callbacks resume. `RealtimeCapture::begin()` auto-prepares an idle ring, and nothing closes the lifecycle gate in between | `red_p1_stale_begin_revives_cleaned_session`: session 1 revived to `complete` | Contained only **incidentally**: the failure path's queued Stop completes the revived session in the same callback, with zero frames. Without that Stop, the ring would capture with no writer and the transport would run |
| **C-1** | `RealtimeCapture::begin()` writes `session_`, `track_`, `layout_`, `projectStart_` and `deviceSampleRate_` as plain fields while the take is still `prepared`, and publishes `capturing` with an unconditional store. `snapshot()` reads those fields in `prepared` | TSan, `RealtimeCapture.cpp:103/105` against the read at `serviceRecording()`'s first line — the same lines at `57d18d7` | A C++ data race (benign on current hardware, undefined by the standard). A cancel or failure landing between `begin()`'s check and its store is overwritten |

**Why the watchdog stops at `prepared`.** Failing a prepared take would rely on
P-1's incidental containment. That is exactly the case of "the existing
finalization cannot guarantee it" that this phase was told to stop at.

**Proposed minimal bundle, "C2-2b — prepared-phase hardening".** It needs your
approval, because it touches the RT path. All of it is lock-free, with no
allocation and no I/O.

1. `consumeCommands`: begin only if `capture_.isPrepared()`. One atomic load. It
   matches the documented production contract that the request is always bound
   before enqueueing, and **closes P-1**.
2. `RealtimeCapture::begin()`: publish `capturing` with a CAS from `prepared`.
   **Closes the lost-terminal part of C-1.**
3. `RealtimeCapture`: do not rewrite fields already bound by `prepareRequest()`.
   Make `projectStart_` and `deviceSampleRate_` atomics, or have `snapshot()`
   skip them while `prepared`. **Closes the data race of C-1.**
4. Extend the watchdog to `prepared`. **Closes M-1b.**

The direct users of `RealtimeCapture::begin()` in the Core tests would keep the
auto-prepare path; only the engine's command consumer would require a bound
request.

## 10. Physical smokes required

| ID | Procedure | Pass criterion |
|---|---|---|
| S-6 | Close the lid, or force sleep, during a take; wake | Either a lifecycle event or the watchdog ends the take; the temporary is retained; no clip. Record which reason appears, and whether the take resumed with a splice (H-1) |
| S-6b | Dead-but-enumerated device during a take (for example a USB hub losing power while the device stays listed, if reproducible) | `deviceStalled` within about T; retained; Record possible once a device works. **Do not press Record while the device is still dead: that is M-1b** *(fixed in C2-2b; see its PS-4)* |
| S-8 | Long takes under heavy CPU load and in the background (P-2 conditions) | **No** `deviceStalled` on a healthy device |
| S-9 | Largest buffer the interface supports | No false failure; detection (if provoked) at about 10 periods |

## 11. Recommended next steps

1. **Physical smoke** of the capturing watchdog (S-6, S-8, S-9).
2. **Decision on C2-2b** (M-1b, P-1, C-1). Recommended. Small, RT-safe, and it
   completes the stall family. *Done: 0.8.3-C2-2b.*
3. **C2-R2** (reopen a reconnected interface), then **C2-3** (xrun telemetry), then
   **C2-4** (host-time continuity: H-1, H-2, and the sub-threshold window in §4).
