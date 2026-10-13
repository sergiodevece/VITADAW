# VitaDAW 0.8.3-C1 — Device Lifecycle & Reconfiguration Audit

Read-only audit of how VitaDAW behaves when the audio device changes, stops,
fails or is lost **during Recording**, at `57d18d7` (0.8.3-B2). Nothing in
production was changed; this document is the only artefact of the phase. It
covers VitaDAW's own code and, where VitaDAW depends on it, the JUCE 9.0.2
CoreAudio backend that delivers the events (`build/_deps/juce-src`).

Guiding question: **is there any path on which a take that looks correct hides
an interruption of capture?**

> **Status after 0.8.3-C2-2b.** The text below is the C1 record, kept for
> traceability. Later phases corrected or closed these points:
>
> | C1 statement | Now | Source |
> |---|---|---|
> | `audioDeviceError` reaches the adapter (§3.1) | **Wrong.** JUCE 9.0.2's `CallbackMaxSizeEnforcer` drops it; the adapter never sees a device error | C2-1 §6.1, `t2a_juce_drops_device_errors` |
> | When the device disappears, JUCE reopens a fallback (§3.3, DF-1, M-2) | **Wrong.** JUCE retries the vanished device and ends with none. M-2 is not reproduced; the real defect is **R-2**: a reconnected interface is not reopened | C2-1 §6.1, `f_interface_loss…`, `red_r2_…` |
> | M-1: a stalled take is stuck until quit | **Fixed** by the watchdog (`deviceStalled` after T) | C2-2 |
> | Stop/Cancel cannot end a take without callbacks (§5) | Fixed for `capturing` (C2-2) and `prepared` (M-1b, C2-2b) | C2-2, C2-2b |
> | — | New findings P-1, P-1b, C-1, X-1 (state protocol) | **Fixed** in C2-2b |
> | H-1, H-2 | **Open** (RED), for C2-4 | C2-1 |
>
> Physical evidence for the watchdog (S-6 sleep, S-8, S-9) is in C2-2b §12.

## 1. Executive summary

**Every device event that JUCE notifies is fail-safe.** Device stop, device
error, device restart, device swap and any reconfiguration that VitaDAW
observes all terminalize an active take as `failed / deviceLost` *before* the
engine is re-prepared. The accepted frames stay in the ring until the writer
drains them into the retained temporary, the take is never published, the
transport stops, and a new take needs an explicit Record with a full
preflight. Voluntary reconfiguration during a take is rejected (buffer size)
or never reconfigures the device (Monitoring); VitaDAW has no device or
sample-rate selector at all.

**The gap is the class of discontinuities that are *not* notified:**

| | Discontinuity | Notified to VitaDAW? | Outcome today |
|---|---|---|---|
| H-1 | CoreAudio processor overload (xrun) losing input cycles | No — JUCE counts it (`getXRunCount()`), VitaDAW never reads it | Take completes and is **published as continuous** |
| H-2 | External sample-rate change, before JUCE's asynchronous restart | Late — the restart runs later on the message thread | Frames at the new hardware rate are captured as the old rate; the take then fails, but a Stop inside the window **publishes a mis-rated tail** |
| M-1 | Device stops calling back with no notification | No — JUCE marks a dead device `running = false` and notifies nobody | Take stays `capturing`; Stop and Cancel cannot complete; if callbacks resume, the take continues with an **unmarked splice** |

JUCE already supplies the signals needed to detect all three (`getXRunCount()`
and `AudioIODeviceCallbackContext::hostTimeNs`). VitaDAW ignores both. Closing
the gap is small in code, but it needs two decisions that belong to the
product, not to an implementer (§6.5). Verdict: **REQUIERE DECISIÓN DE
ARQUITECTURA**. Everything else in the C2 plan can be implemented now.

## 2. Current architecture

| Component | Role in the device lifecycle | Thread |
|---|---|---|
| JUCE CoreAudio backend | Runs the IO proc, detects setup changes, restarts the device, reports errors | CoreAudio IO thread; property-listener threads; message thread (async restart) |
| `juce::AudioDeviceManager` | Owns the device; forwards callbacks under `audioCallbackLock`; reopens a fallback device when the current one disappears | IO thread for callbacks; message thread for open/close |
| `JuceAudioDeviceAdapter` | Receives the JUCE callbacks; turns uncontrolled stops/errors into a pending event; certifies the configuration; re-prepares the engine | callbacks as JUCE delivers them; everything else on the message thread |
| `RealtimeAudioEngine` | Lifecycle gate (`unavailable/initializing/operational/stopped/error`); terminalizes capture on any transition away from operational | RT for `processBlock`; quiescent control calls for transitions |
| `RealtimeCapture` | SPSC ring and terminal CAS (`stop` vs `fail`, first terminal wins) | RT producer, message-thread consumer |
| `DawApplication` | Consumes the terminal capture state; cleans up; never commits a failed take | message thread (30 Hz timer) |

Each timer tick runs `pollDeviceLifecycle()` **before**
`synchroniseTransport()` (`JuceApplication.cpp` timer callback), so a failure
set while device events are processed is observed and cleaned up in the same
tick.

Only one armed track is recorded per take, so **multitrack recording under a
device change does not apply** to this version.

## 3. Event and transition graph

### 3.1 JUCE callbacks received by the adapter

| Event | Receiver (file:line) | Thread | State changed | If a take is active | Core notified | Fail-safe | Reconfigures without stopping the take | Writer and material |
|---|---|---|---|---|---|---|---|---|
| `audioDeviceIOCallbackWithContext` | `JuceAudioDeviceAdapter.cpp:2547` | CoreAudio IO | none of its own; calls `processBlock` with the **cached** `deviceSampleRate_` and **ignores the context** (`hostTimeNs`) | captures every delivered block | — | — | — | — |
| `audioDeviceAboutToStart` | `:2564` | the thread that starts the device (message thread for JUCE restarts and our own attach), serialised by JUCE before the first callback | sets `deviceSampleRate_` from the device; **clears any pending lifecycle event**; `deviceInitialising()` or `deviceInitialisingPreservingTransport()` | **fails it as `deviceLost`** and stops the clock (`RealtimeAudioEngine.cpp:275-295, 317-334`) | yes, directly | yes | no | ring untouched; drained and retained at cleanup |
| `audioDeviceStopped` | `:2581` | the thread that stops the device | if inside our own detach: `deviceInitialisingPreservingTransport()` (fails the take); otherwise only `pendingLifecycleEvent_ = stopped` | own detach: failed at once; uncontrolled: failed at the next poll (`deviceStopped`, `:482-496`) | directly, or via the poll | yes | no | as above |
| `audioDeviceError` | `:2593` | unspecified JUCE thread | `pendingLifecycleEvent_ = error` only | failed at the next poll: detach, then `deviceError()` (`:465-481`) | via the poll | yes | no | as above |
| `changeListenerCallback` | `:2600` | message thread | `refreshState()` only | none by itself | — | — | — | — |

### 3.2 `pollDeviceLifecycle()` (`JuceAudioDeviceAdapter.cpp:462-571`), message thread

1. **Pending `error`** → detach, clear monitoring demand, `deviceError()` → take
   failed `deviceLost`; device marked error.
2. **Pending `stopped`** → `deviceStopped()` → take failed `deviceLost`; device
   marked error.
3. **Configuration diverged** — rate, buffer or device object differ from the
   certified ones (`:498-546`) → `detachAudioCallback(true)`. JUCE's
   `removeAudioCallback` calls `audioDeviceStopped()` after removal
   (`juce_AudioDeviceManager.cpp:1053-1069`). With the preserve flag set,
   that fails the take as `deviceLost` **before** `reprepareForCurrentDevice()`
   runs; then the engine is re-prepared and the callback re-attached.
4. **Monitoring route lost** → monitoring off; does not affect a take.

`reprepareForCurrentDevice()` (`:2913-2984`) and `configure` never touch the
capture ring. Only `resetRecordingCapture()` (cleanup) and
`prepareRecordingCapture()` (preflight) reset it
(`RealtimeAudioEngine.cpp:614, 680`). Every frame accepted before a device
event therefore survives until `synchroniseRecording()` drains it into the
temporary.

### 3.3 JUCE 9.0.2 CoreAudio behaviour that matters

| Behaviour | Source (`juce_CoreAudio_mac.cpp`) | Consequence for VitaDAW |
|---|---|---|
| An input block is **dropped silently** when the IO proc cannot take its try-lock, or after a buffer-size change has been detected | `:1582` (`if (! lock.isLocked() \|\| detectedSetupChange) return;`), `:1588-1592` | Frames never reach VitaDAW. Benign when followed by a restart (the take then fails); see M-1 and H-2 for the other cases. |
| External sample-rate change → `deviceSetupChanged()` → `triggerAsyncUpdate()`; the restart (`handleAsyncUpdate`) runs **later on the message thread** | `:1858-1871`, `:2235-2263` | Between the hardware change and the restart, callbacks continue at the new rate while VitaDAW still passes the old cached rate → **H-2**. |
| Device death sets `running = false` and **notifies nobody** | `:1416-1420` | Without a device-list change, VitaDAW is never told → **M-1**. |
| Processor overloads are counted and exposed through `getXRunCount()` | `:1390`, `:2154-2157` | Available, unused by VitaDAW → **H-1**. |
| Host time of every block is passed as `context.hostTimeNs` | `:1608-1615` | Available, ignored by VitaDAW (`:2547-2550`) → could detect H-1, H-2 and M-1 on resume. |
| Separate input and output devices are combined in a **private aggregate device** created by JUCE | `:2267-2268` | Even without a user aggregate, a USB input with built-in output already runs through an aggregate (DF-5). |
| When the current device disappears, `AudioDeviceManager` closes it and **reopens a fallback** (saved state or default) | `juce_AudioDeviceManager.cpp:271-311` | See M-2. |

## 4. Device scenarios

### DF-1 — Interface disconnected during Recording

- **Detection.** CoreAudio's device-list change makes JUCE close the device
  (`audioDeviceStopped` → pending `stopped`), then reopen a fallback
  (`audioDeviceAboutToStart` → take failed `deviceLost`; the pending event is
  cleared). If no fallback opens, the next poll fails the take via
  `deviceStopped()`.
- **Latency.** The take is terminalized when JUCE processes the list change
  on the message thread, and cleaned up at the next 30 Hz tick: tens of
  milliseconds in normal conditions, longer when the message thread is busy
  (0.8.3-B2 measured up to 643 ms in the background). No frame is captured in
  that interval: the removed device has stopped calling back.
- **Transport and capture.** Clock stopped without rewind; capture
  `failed / deviceLost`.
- **Finalization.** Never finalized or published. `synchroniseRecording()`
  drains the ring, then `discardRecordingWithDiagnostics()` closes the WAV
  header and retains the temporary.
- **Message.** "Recording stopped because the audio device was lost; recording
  media retained for recovery".
- **New take.** Possible without restarting, on whatever device JUCE reopened —
  see M-2. Whether VitaDAW can return to the original interface once it
  reappears needs hardware: JUCE does not switch back on its own and VitaDAW
  has no device selector.

### DF-2 — Input disappears

- `RealtimeCapture::capture()` fails with `missingInput` only for a null input
  array, too few channels or a null channel pointer
  (`RealtimeCapture.cpp:121, 126`). JUCE's CoreAudio backend always passes its
  own fixed-size channel buffers, sized when the device starts, so on macOS
  this failure is **unreachable in production**. A topology change goes
  through a JUCE restart instead, and fails the take as `deviceLost`.
- VitaDAW does **not** treat silence as a fault, which is correct: an
  unplugged jack on a running interface delivers zeros that are
  indistinguishable from a quiet source.
- **Hypothesis (hardware only).** JUCE copies only the channels present in the
  IO proc's input list and never clears its input buffers (`:1595-1601`). If a
  running device ever delivered a cycle without one of the active channels,
  that channel would repeat the previous block until the restart. Not
  observed; listed for physical testing (S-5).

### DF-3 — Sample-rate change

- **Initiated by VitaDAW.** Never during a take: there is no rate selector, the
  buffer-size command is rejected, and Monitoring does not reconfigure (§6.1).
- **External** (Audio MIDI Setup, another application). The JUCE listener
  schedules an asynchronous restart (§3.3). Until it runs:
  - callbacks continue at the new hardware rate;
  - VitaDAW passes the **cached old rate** to `processBlock`, so the engine's
    `sampleRateChanged` check (`RealtimeAudioEngine.cpp:713-715`) cannot fire;
  - the frames are written to a WAV whose header declares the old rate, and the
    transport advances at the old rate.

  When the restart runs, `audioDeviceAboutToStart` fails the take as
  `deviceLost`. **Normal outcome:** a failed take whose retained temporary ends
  with a mis-rated tail. **Race:** if a Stop is consumed inside the window and
  finalization runs before the restart, the take is **published with the
  mis-rated tail** (H-2).
- The reported reason is always `deviceLost`. `sampleRateChanged` is
  unreachable in production and has no test (L-1).

### DF-4 — Buffer-size change

- **Initiated by VitaDAW.** `SetAudioBufferSize` is a command, and the
  application's global gate rejects it while a take is busy
  (`DawApplication.cpp:171-183`). A test covers it with a *prepared* take
  (`InputMonitoringDeviceLifecycleTests.cpp:1183-1201`), asserting that no
  hardware is touched.
- **External or by the driver.** JUCE detects a block-size mismatch inside the
  IO proc, drops blocks from then on and schedules a restart
  (`:1588-1592`); the restart fails the take as `deviceLost`. The dropped
  blocks belong to a take that will fail. **No silent continuation.** A Stop
  pressed inside that window is never consumed (no callbacks), so the take
  fails rather than publishing.
- Changing the buffer does not by itself corrupt anything; a restart ends the
  take.

### DF-5 — macOS aggregate devices

- JUCE already uses a private aggregate whenever input and output devices
  differ (§3.3), so aggregate behaviour applies to ordinary setups too.
- A member disappearing, a channel-count change or a nominal-rate change
  reaches VitaDAW as a JUCE restart or a device-list change, so it fails the
  take (DF-1, DF-3, DF-4), provided CoreAudio reports it.
- **What depends on CoreAudio's reporting** (hardware only):
  - whether a member's drop-out is reported before blocks with missing or
    stale input are delivered (DF-2 hypothesis);
  - drift correction between members, which resamples and is not a
    discontinuity;
  - whether a dead member makes the whole aggregate stop calling back without
    a notification (M-1).
- **Can be simulated automatically:** device stop, error, restart, rate and
  buffer change, a device swap and a channel-count change, all with the
  existing hardware-free `VirtualAudioDevice`.
- **Needs hardware:** real member removal, drift, Bluetooth members, and
  overload behaviour.

## 5. Concurrency and transition risks

| Crossing | What happens | Assessment |
|---|---|---|
| Device lost **and** Stop | Stop is consumed only by the RT callback; device events fail the capture from the control side. The capture CAS lets the first terminal transition win. | Stop first: all frames up to Stop are already in the ring, so finalizing is correct. Loss first, or Stop never consumed: the take fails. Never a silent splice. **Safe.** |
| Device lost **and** overflow | Both are failures; first CAS wins, the other reason is lost. | Same outcome either way; only the reported reason differs. **Safe.** |
| Configuration change **and** Record starting | A restart closes the lifecycle gate; a queued `beginRecord` is cancelled, and a `prepared` take fails `deviceLost`. Covered in Core (`RecordingCaptureTests.cpp:270-296`). | **Safe.** |
| Device lost **during finalization** | Finalization starts only after the capture completed; a later loss cannot fail it (the CAS only leaves `prepared/capturing`). The project commit then runs against a device that may be gone. | Audio integrity is safe. The commit path under device loss is **untested** (T-8). |
| Writer close **and** device restart | The writer lives on the message thread and is untouched by device events. Restarts never reset the ring. | **Safe.** |
| Device recovery **and** new Record | Record needs an operational engine and a full preflight. JUCE's fallback can make the engine operational again on **another device**. | Explicit action is required (contract C met), but the source may change silently (M-2). |
| Reconfiguration **with Monitoring on** | Monitoring never reconfigures during a take (`JuceAudioDeviceAdapter.cpp:1403`). A device restart turns Monitoring off and fails the take; neither depends on the other. Tested: `InputMonitoringDeviceLifecycleTests.cpp:551-573, 575-807`. | **Safe.** |
| Callback stall **and** Stop / Cancel | Both are RT commands (`RealtimeAudioEngine.cpp:642-650`); with no callbacks, neither is consumed. | Take stuck until quit (M-1). |
| Physical change during **multitrack** recording | One armed track per take. | **Not applicable.** |

**Error priority.** There is no explicit priority between concurrent terminal
causes: the first CAS wins (`RealtimeCapture`). Every failure leads to the
same safe outcome — no commit, media retained, transport stopped — so the
order affects only the reason shown.

No finding here requires a lock in the RT path, and none is proposed.

## 6. Proposed safety contract

### 6.1 A — Voluntary reconfiguration during Record

| Requirement | Today |
|---|---|
| Device change rejected or preceded by a controlled stop | **Met by absence**: VitaDAW has no device selector. |
| Sample-rate change rejected | **Met by absence**: no rate selector. |
| Buffer change rejected | **Met**: command gate (`DawApplication.cpp:171`), tested with a prepared take. |
| Monitoring never reconfigures a take | **Met by construction**: the input is already open (`:1403`). |
| A take's format never changes silently | **Met for voluntary changes.** |

### 6.2 B — Unexpected device change

| Requirement | Notified events | Unnotified (H-1, H-2, M-1) |
|---|---|---|
| Never accept discontinuous audio as continuous | Met | **Not met** |
| Safe terminal state | Met (`failed / deviceLost`) | **Not met** (H-1: completes; M-1: stuck) |
| No automatic publication of an incomplete take | Met | **Not met** (H-1; H-2 inside the race) |
| Captured material retained per recovery policy | Met: the ring is drained into the retained temporary; markers stay `temporary` | Met for whatever was accepted |
| Resources released | Met (writer closed, ring reset at cleanup) | M-1: only at quit |
| Reason reported | Met, but always "device was lost" (L-1) | **Not met** |
| New take possible after a valid device returns | Met, with M-2 | — |

### 6.3 C — Recovery

**Met.** A failed take is never resumed. A new take requires an explicit
Record and a full preflight, with a new session, a new temporary and a new
marker.

### 6.4 D — RT restrictions

**Met by VitaDAW's own code.** Device events are handled outside VitaDAW's RT
path: by JUCE before the first callback, or on the message thread. The
callback does no locking, I/O, allocation or UI work.

Outside VitaDAW's control: JUCE's `AudioDeviceManager` takes
`audioCallbackLock` around its callback dispatch, and the CoreAudio IO proc
uses a try-lock (§3.3).

Every detection proposed in §10 keeps this contract: integer atomics and O(1)
arithmetic in RT; everything else on the message thread.

### 6.5 Decisions needed before C2 can close H-1 and H-2

- **D1 — Policy for a detected discontinuity.**
  - *(a) Strict:* fail the take terminally, retain the media, do not publish.
    This is the literal reading of contract B.
  - *(b) Marked:* complete the take but record the discontinuity (position and
    size) and surface it.
  - **Recommendation: (a).** It is consistent with every other failure path
    and needs no new project data. The cost is that one overload ends a long
    take, so the decision should be informed by how often xruns actually
    occur on the target hardware (S-4).
- **D2 — Detection signal.**
  - *(a)* Poll `getXRunCount()` from the message thread at each service pass.
    Non-RT and minimal, but blind to H-2 and to stalls.
  - *(b)* Also check host-time continuity in the RT path: compare each block's
    `hostTimeNs` with the previous one plus the expected block duration, and
    the elapsed host time over about one second with the nominal rate. O(1),
    atomics only, but it needs host time passed into the engine, a small
    change to the RT interface.
  - *(c)* Add a message-thread liveness watchdog (M-1).
  - **Recommendation: (a) + (c) first** — no RT change. Then **(b)**, which is
    the only one that closes H-2.

## 7. Findings

| Sev. | ID | Location | Condition | Evidence | Consequence | Minimal recommendation |
|---|---|---|---|---|---|---|
| **HIGH** | H-1 | `JuceAudioDeviceAdapter.cpp:2547-2550` (context ignored); no use of `getXRunCount()` in `src/` | A CoreAudio processor overload loses input cycles during a take | JUCE counts overloads (`juce_CoreAudio_mac.cpp:1390, 2154`) and passes host time (`:1615`); VitaDAW reads neither | The take completes and is **published as continuous** with a hidden gap | D1 + D2(a); then D2(b) |
| **HIGH** | H-2 | `:2547-2560` (cached rate); JUCE async restart `juce_CoreAudio_mac.cpp:1858-1871, 2235-2263` | External sample-rate change during a take | Callbacks continue at the new rate until the restart; the engine's rate check sees only the cached rate | Normally a failed take with a mis-rated retained tail; with a Stop inside the window, a **published mis-rated tail** | D2(b): host-time rate check |
| MEDIUM | M-1 | no watchdog in `src/`; Stop and Cancel are RT commands (`RealtimeAudioEngine.cpp:642-650`) | The device stops calling back with no notification | JUCE device death: `running = false`, no callback (`juce_CoreAudio_mac.cpp:1416-1420`) | Take stuck in `capturing`, cannot be stopped or cancelled until quit; if callbacks resume, an **unmarked splice** | Message-thread watchdog: no accepted-frame progress for longer than a threshold → `failRecording` (direct path that already exists) |
| MEDIUM | M-2 | `RecordingInputSetupPolicy.h:30-36`; aboutToStart clears the pending event (`JuceAudioDeviceAdapter.cpp:2568`); JUCE fallback (`juce_AudioDeviceManager.cpp:297-305`) | Device lost, JUCE reopens a fallback device | The poll re-certifies the fallback; device status returns to active; the preflight silently selects the default input | The next explicit take may record **a different source** (e.g. the built-in microphone) without any indication | Return the input device name from the preflight; warn when it differs from the previous take's device after a loss |
| MEDIUM | M-3 | `tests/` | Coverage | No adapter or application test with an **active** take under device stop, error, restart, rate change or device swap; `sampleRateChanged` has no test | Every fail-safe path in §3 is verified by code reading and Core tests only | T-1 … T-8 (§8) |
| LOW | L-1 | `RealtimeAudioEngine.cpp:275-334`; `DawApplication.cpp:91-113` | Any restart or reconfiguration | Every path reports `deviceLost`; `sampleRateChanged` and `missingInput` are unreachable on CoreAudio | Misleading message ("the audio device was lost" for a rate change); harder diagnosis | Distinguish "stopped or reconfigured" from "lost" in the reason and message |
| LOW | L-2 | `RealtimeAudioEngine.cpp:701-717` | A rate mismatch while capturing | The early return for a format mismatch runs **before** the capture rate check | Unreachable today (every rate change fails the take first). A future path that changed the rate without failing the take would silently skip capture instead of failing | Order the capture rate check before the early return, or assert the invariant |
| INFO | I-1 | §3 | Notified events | Code paths verified end to end | Fail-safe: take failed before re-prepare, accepted frames retained, nothing published | — |
| INFO | I-2 | §6.1 | Voluntary reconfiguration | Gate, fast path, no selectors | Contract A met | — |
| INFO | I-3 | DF-1 | Return to the original interface | No device selector; JUCE does not switch back | Probably needs a VitaDAW restart | Verify physically (S-1) |
| INFO | I-4 | DF-2 | Stale-input hypothesis in JUCE | `juce_CoreAudio_mac.cpp:1595-1601` | Unobserved | Physical S-5 |

**Not classified BLOCKER, with the reason.** H-1 matches the wording
"publicación engañosa de audio discontinuo". It is classified HIGH because
VitaDAW's own transitions are correct: the defect is the absence of detection
for an external fault whose frequency on the target hardware has not been
measured. If S-4 shows overloads in ordinary use, it should be raised to
BLOCKER for the 0.8.3 release. H-2 needs an external rate change and a Stop
inside a window of tens to hundreds of milliseconds.

## 8. Automated coverage

### 8.1 What exists and what it proves

| Suite | Layer | Contract it proves |
|---|---|---|
| `RecordingCaptureTests` | Core | `deviceStopped`, `deviceInitialising`, `deviceInitialisingPreservingTransport` terminalize a capturing **or prepared** take as `deviceLost`; a stale `beginRecord` cannot start after recovery; `missingInput` on null or insufficient input; the terminal CAS is stable either way round |
| `RealtimeAudioEngineTests` | Core | Lifecycle gate transitions and command cancellation |
| `RecordingApplicationTests` | Application, fake engine | A `deviceLost` capture leads to cleanup, no commit, and a subsequent Record |
| `InputMonitoringDeviceLifecycleTests` | JUCE adapter, virtual device | Buffer change rejected with a **prepared** take, with no hardware mutation; device stop/error and controlled rate change **for Monitoring**; Recording with Monitoring toggled; loopback device loss |
| `ProjectMediaIntegrationTests` | JUCE adapter + application | Overflow → cleanup → new take → published clip (0.8.3-B2) |

**Not proven by any test:** an active take on the real adapter under device
stop, error, restart, rate change or device swap; retention of exactly the
accepted frames after such an event; device loss during finalization; the
`sampleRateChanged` path; xrun, host-time and stall behaviour.

### 8.2 New test matrix

| ID | Scenario | Expected behaviour | Current coverage | Required test | Physical smoke |
|---|---|---|---|---|---|
| T-1 | Uncontrolled device stop while capturing | `failed / deviceLost`; transport stopped; temporary holds exactly the accepted frames; no commit | Core only | JUCE integration: virtual device `stop()` mid-take, then `pollDeviceLifecycle` and `synchroniseTransport` | S-1 |
| T-2 | Device error while capturing | Same as T-1 | Core only | JUCE integration: `audioDeviceError` mid-take | — |
| T-3 | JUCE restart (stop, then aboutToStart) while capturing | Same as T-1; pending event cleared without losing the failure | None | JUCE integration | S-2 |
| T-4 | Configuration diverged (rate or device swap) while capturing | Take failed **before** re-prepare; ring not reset before drain | None | JUCE integration: change the virtual device, then poll | S-2 |
| T-5 | Buffer change while **capturing** | Rejected; no hardware mutation | Prepared only | Extend the existing test | — |
| T-6 | Recovery after device loss | New Record accepted after re-certification; the input device is reported (M-2) | None | JUCE integration | S-1 |
| T-7 | Stop racing device loss | Stop first: published, complete. Loss first: failed, retained | Core CAS only | Core + JUCE integration | — |
| T-8 | Device loss during finalization | Published file and project stay consistent: either committed, or retained and reported | None | JUCE integration | — |
| T-9 | Callback stall (M-1) | Watchdog fails the take; Stop no longer needed | None (no watchdog) | JUCE integration: virtual device stops rendering without notifying | S-6 |
| T-10 | Xrun during a take (H-1) | Per D1 | None | Virtual device reporting `getXRunCount()` | S-4 |
| T-11 | Host-time gap or rate drift (H-1, H-2) | Per D1 | None | Core: synthetic host times; JUCE: virtual device context | S-3 |

T-1 to T-8 test **today's** behaviour and can be written before any production
change. T-9 to T-11 accompany the C2 changes.

## 9. Physical smoke proposal

All on a disposable project, never on user projects; none changes a device the
user is relying on.

| ID | Scenario | Procedure | Pass criterion |
|---|---|---|---|
| S-1 | USB interface unplugged mid-take | Record from the interface; unplug at about 10 s; observe; replug; try to record again | Take fails `deviceLost`; temporary retained and readable; no clip; record which device is active afterwards, and whether the interface can be used again without restarting |
| S-2 | External rate change mid-take | Audio MIDI Setup: change the interface rate during a take | Take fails; inspect the retained temporary's tail for a mis-rated segment (documents H-2) |
| S-3 | Stop immediately after an external rate change | As S-2, pressing Stop as soon as the rate is changed | Documents whether H-2's race is reachable in practice |
| S-4 | Overload frequency | Long takes at 32/64-frame buffers under CPU load, with diagnostics reading `getXRunCount()` (a diagnostics-build addition, C2) | Number of overloads per hour on the target hardware; this informs D1 |
| S-5 | Aggregate member removal | User aggregate (USB input + built-in output); remove the input member mid-take | Take fails; check the temporary for repeated blocks (I-4) |
| S-6 | Sleep or wake, or a stalled device | Close the lid or force sleep during a take, then wake | Documents whether callbacks stop silently (M-1) and whether the take resumes with a splice |
| S-7 | Bluetooth input disconnect | AirPods or a headset as input; disconnect mid-take | Same as S-1 |

## 10. C2 implementation plan

In order; each step is small and independently verifiable.

| Step | Problem solved | Files | Behaviour change | Tests | Regression risk | Proof | Physical |
|---|---|---|---|---|---|---|---|
| C2-1 | M-3 | `tests/InputMonitoringDeviceLifecycleTests.cpp` or `ProjectMediaIntegrationTests.cpp` (existing seams) | **None** | T-1 … T-8 | None (tests only) | Green against today's code; documents the baseline | No |
| C2-2 | M-1 | `JuceAudioDeviceAdapter.cpp` (service pass) | A take whose accepted-frame count does not advance for longer than a threshold (proposed 1 s) fails via the existing direct `failRecording()` | T-9 | A false trigger during a very long legitimate pause — but a running device always calls back, so only a stall can trigger it | T-9 + S-6 | S-6 |
| C2-3 | H-1 (part) | `JuceAudioDeviceAdapter.cpp`; `RecordingTypes.h` if D1 adds a reason | At Record, read `getXRunCount()`; on each service pass, an increase applies D1 | T-10; virtual device xrun counter | Takes failing on overload (intended under D1a) | T-10 + S-4 | S-4 |
| C2-4 | H-1, H-2 | `RealtimeCapture` or engine RT path (host-time check, atomics only); callback passes `hostTimeNs` | A block gap or rate deviation beyond tolerance applies D1 | T-11 (Core, synthetic host times) | False positives on drivers with jittery timestamps; tolerance must be measured | T-11 + S-2/S-3 | S-2, S-3 |
| C2-5 | L-1 | `RealtimeAudioEngine.cpp`, `RecordingTypes.h`, `DawApplication.cpp` messages | Reason "device stopped or reconfigured" kept apart from "device lost" | Existing Core tests updated + T-3/T-4 | Recovery markers that serialise reasons must stay compatible | Tests | No |
| C2-6 | M-2 | `RecordingInputSetupPolicy.h` / `prepareRecording` result; UI message | Preflight reports the input device; warns if it changed since the last take after a loss | T-6 | None for audio | T-6 + S-1 | S-1 |
| C2-7 | L-2 | `RealtimeAudioEngine.cpp:701-717` | Capture rate check before the format early-return (or an assertion) | Core test with a rate mismatch while capturing | Low | Core test | No |

C2-1, C2-2, C2-5, C2-6 and C2-7 need no further decision. C2-3 and C2-4
depend on D1 and D2.

## 11. Acceptance criteria for C2

1. For every scenario in §8.2, a take either completes with continuity
   established, or ends `failed` with a specific reason. **It is never
   published when continuity is not established.**
2. A failed take's temporary holds exactly the frames accepted before the
   failure, with a valid header and a `temporary` recovery marker.
3. No device event can leave a take in `capturing` with no way to end it
   short of quitting.
4. The RT callback gains no lock, allocation, I/O, string or unbounded work.
5. The existing battery (60/60 at `57d18d7`) stays green; T-1 … T-11 are
   added.
6. S-1, S-2, S-4 and S-6 have recorded results on the target hardware.

## 12. Remaining risks

- **Driver behaviour that CoreAudio itself does not report** stays outside any
  software detection, including duplicated input blocks.
- **Overload frequency is unknown** until S-4 runs; D1 should not be finalised
  without it.
- **Host-time quality varies by driver.** The C2-4 tolerance must be
  calibrated physically.
- **Returning to a reconnected interface** probably requires restarting
  VitaDAW (I-3).
- **Content recovery of retained takes** (0.8.3-B2 §8.7) is still not
  validated. After a device change, a retained temporary may end with a
  mis-rated or truncated segment (H-2, DF-4), which matters once recovery
  imports it.
- This audit covers **macOS / CoreAudio** only, which is where VitaDAW is
  validated.
