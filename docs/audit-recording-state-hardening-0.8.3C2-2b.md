# VitaDAW 0.8.3-C2-2b — Recording State Concurrency Hardening

Closes findings **P-1**, **C-1** and **M-1b** from the watchdog audit
([`audit-recording-watchdog-0.8.3C2-2.md`](audit-recording-watchdog-0.8.3C2-2.md) §9).

- **Baseline:** `57d18d7`, on top of the uncommitted C1, C2-1 and C2-2 work.
- **What changes:** the state protocol of `RealtimeCapture`, its use by
  `RealtimeAudioEngine`, and the adapter watchdog, which now also supervises
  `prepared`.
- **What does not change:** H-1, H-2 and R-2 stay RED and untouched.

Traceability:

| Finding | RED evidence before | GREEN now |
|---|---|---|
| P-1 | `red_p1_stale_begin_revives_cleaned_session` (exit 3) | `p1_stale_begin_cannot_revive_cleaned_session`; contract kept verbatim |
| M-1b | `red_m1b_prepared_stall` (exit 3) | `m1b_prepared_stall_terminalizes`; preconditions and contract kept verbatim |
| C-1 | `c1_prepared_transition_under_concurrency` reported by TSan (label `characterization-tsan`) | Same case, TSan-clean with no suppression; label removed |

The new Core suite `vitadaw_recording_state_protocol_tests` was also run
against an unmodified `57d18d7` tree (§7.4). Four of its cases fail there, for
P-1, P-1b and C-1; all of them pass now.

## 1. Defects

### 1.1 The approved findings, as they were at `57d18d7`

| ID | Defect | Evidence before | Reachable in the app |
|---|---|---|---|
| **P-1** | `RealtimeCapture::begin()` prepared an idle ring itself (`if (canPrepareRequest() && !prepareRequest(request)) return false;`). A take terminalized while `prepared` is cleaned up, and the ring returns to idle. Its still-queued `beginRecord` then re-bound and began the dead session when callbacks resumed. | `red_p1_…` exit 3. Engine case `staleBeginAfterCleanup` fails at `57d18d7` (revived session, transport started) | Yes, through a programmatic Cancel, and through any failure of a prepared take — including the M-1b watchdog. The failure path's queued Stop contained it only by accident |
| **C-1a** | Data race. `begin()` wrote `session_`, `track_`, `layout_`, `projectStart_` and `deviceSampleRate_` as plain fields while the status still read `prepared`. `snapshot()` read them, and it was called by the message thread, by `failRecording()` on the service thread, and by `processBlock()` on the callback itself. | TSan: `RealtimeCapture.cpp:103` (`begin`) against `:255` (`snapshot`), in `c1_…` and in the `57d18d7` run of the new suite | Yes; undefined behaviour in C++ |
| **C-1b** | Lost terminal transition. `begin()` checked `isPrepared()` and later published `capturing` with an unconditional `store`. A cancel or failure landing in between was overwritten. | At `57d18d7`, `cancelRacesBeginOnTheEngine` fails within a few hundred rounds. For example, round 85 ended **`capturing`, failure `none`, 32 frames, transport playing**, after `failRecording()` had returned true | Yes; the window is short |
| **C-1c** | Incoherent snapshot: a snapshot could say `prepared` while already carrying `begin()`'s values | At `57d18d7`, `snapshotDuringBegin` fails: "`prepared` snapshot: start 27003, rate 48000" | Yes |
| **C-1d** | `failRecording()` checked the session with `snapshot()` and then called the untagged `fail()`: a check-then-act gap | Code inspection | Yes |
| **M-1b** | The watchdog skipped `prepared`, Stop is an RT command, and the UI has no Cancel. A device that never calls back after Record left the take `prepared` for ever. | `red_m1b_…` exit 3 | Yes |

### 1.2 Pre-existing defects found during this phase

| ID | Defect | Evidence | Reachable in the app |
|---|---|---|---|
| **P-1b** | The `cancelRecord` command called the untagged `capture_.cancel()`. Take N is cancelled, then fails by another path and is cleaned up, while its Cancel is still queued. That stale Cancel then failed take N+1, prepared in the meantime. | At `57d18d7`, `staleCancelCannotFailANewSession` fails | No: every Record preflight closes the lifecycle gate, which drops the stale Cancel. Reachable through the Core API |
| **X-1** | Failures the callback detects used the untagged `fail()` after a separate phase read: missing input and overflow in `capture()`, and the rate check in `processBlock()`. A callback in flight across the control thread's cleanup and its next `prepareRequest()` could fail the next take. | Code inspection. It cannot be reproduced without pre-empting the callback inside `capture()` | No, for the same preflight reason. Closed by construction (§5) |

### 1.3 Introduced and fixed within this phase

**C-1e.** In the first C2-2b version, `begin()` compared the plain `track_` after
its claim. Suppose a failure lands while `begin()` holds `starting`, and cleanup
and the next `prepareRequest()` follow at once. Their writes then race that
read: nothing orders them, because the claim is an acquire-only CAS.

- **Found by** review.
- **Confirmed by** TSan `-O1`: write at `prepareRequest` `:78` against read at
  `begin` `:118`, in `cleanupRacesBegin`.
- **Invisible to** TSan `-O0` (§8.2).
- **Fixed:** `track_` is now an atomic.

## 2. Protocol

### 2.1 Before (`57d18d7`)

- **Status word:** a 32-bit `{phase, failure}`.
- **Plain fields:** session, track, layout, start, rate and placement.
- **Publication:** `prepareRequest()` and `begin()` wrote the plain fields, then
  published with a `store`. `clearPreparedRequest()` stored idle without a CAS.
- **Terminal transitions:** only `stop()` (a single CAS) and `fail()` (a CAS
  loop) were CASes. Neither named a session.

### 2.2 After

The status word is a 64-bit `phase | failure << 8 | session << 16`:

- `RealtimeCapture::maximumSession` = 2^48 − 1; larger ids are refused.
- The internal phase `starting` reads as `prepared` publicly.

Every transition is a CAS on that word. The only exceptions are `reset()` and
`prepare()`, which the control thread runs on an inactive ring:

```
idle ──prepareRequest(S)  [control, CAS, release]──────────────▶ prepared(S)
prepared(S) ──clearPreparedRequest  [control, CAS]─────────────▶ idle
prepared(S) ──begin: claim  [callback, CAS, acquire]───────────▶ starting(S)
starting(S) ──begin: request mismatch  [callback, CAS, release]─▶ prepared(S)
starting(S) ──begin: publish  [callback, CAS, release]─────────▶ capturing(S)
capturing(S) ──stop  [callback, CAS]───────────────────────────▶ complete(S)
prepared|starting|capturing(S) ──fail / failSession(S)  [any, CAS]──▶ failed(S, reason)
complete|failed ──reset  [control, store, release]─────────────▶ idle
```

Properties, each established by the CAS that implements it:

1. **Only the prepared session begins.** `begin(S)` claims only the exact value
   `prepared(S)`. An idle ring, a terminal take or another session fails the
   claim before anything is written. This closes P-1.
2. **A terminal transition is never lost.** The publication CAS expects
   `starting(S)`. A failure that landed while `starting` was held has replaced
   it, so the publication fails, `begin()` returns false and the failure stands.
   This closes C-1b.
3. **Terminal is final.** Nothing leaves `complete` or `failed` except `reset()`
   to idle, and `begin()` cannot claim from either. A late `begin()` cannot
   revert a terminal transition, and a cleaned-up session cannot be begun again.
4. **Session-scoped failure.** `failSession(S)` acts only on `active(S)` and
   reports whether it made the transition. It is used by `failRecording()`
   (writer and watchdog), by Cancel, and by every failure the callback detects
   (X-1).
5. **Lifecycle failure.** `fail()`, without a session, is kept only for lifecycle
   events (device lost, restarted, unavailable), which end whichever take is
   active, including a prepared one.

### 2.3 Exact RT diff and the motive for each change

`RealtimeCapture` (`.h`/`.cpp`):

| Function | Before | After | Motive |
|---|---|---|---|
| status word | `atomic<uint32>` `{phase, failure}` | `atomic<uint64>` `{phase, failure, session}`, plus `starting` | Make session, phase and transition one atomic step (P-1, C-1b, C-1d) |
| `session_` | plain field | removed; the tag is the only copy | No plain session to race (C-1a) |
| `track_`, `layout_` | plain | `atomic<TrackId>`, `atomic<AudioChannelLayout>` | The callback reads them (C-1a, C-1e) |
| `projectStart_`, `deviceSampleRate_` | plain | `atomic<int64_t>`, `atomic<double>` | Written by the callback, read by the control thread (C-1a) |
| `prepareRequest()` | plain writes + `store(prepared)` | writes + CAS `idle → prepared(S)` (release) | Publication; refuses a non-idle ring atomically |
| `clearPreparedRequest()` | `store(idle)` + field clears | CAS `prepared → idle` | Can never undo a take the callback has claimed |
| `begin()` | auto-prepare; `isPrepared()` check; plain writes; unconditional `store(capturing)` | claim CAS; validate; zero; write; publish CAS | P-1, C-1a, C-1b |
| `capture()` | `isCapturing()`; untagged `fail()` | one status read; `failSession(observed session)` | X-1 |
| `stop()` | one strong CAS | CAS loop that keeps the session tag | The word now carries the tag |
| `fail()` | CAS loop over `prepared`/`capturing` | the same, `starting` included, tag kept | A lifecycle event during `begin()` wins |
| `failSession()` | — | new: session-scoped CAS loop, returns whether it transitioned | P-1b, C-1d, X-1, watchdog |
| `drain()` | refuses idle and `prepared` | also refuses `starting` | `begin()` is zeroing the ring then |
| `snapshot()` | plain reads, any thread | control thread only; session from the tag; no begin values while `prepared` | C-1a, C-1c |
| accessors | — | `activeSession()`, `capturingSession()`, `capturingSampleRate()`; `isPrepared()` and `isActive()` include `starting` | Lock-free state the callback uses instead of `snapshot()` |

`RealtimeAudioEngine.cpp`:

| Place | Before | After | Motive |
|---|---|---|---|
| `processBlock` | `capture_.snapshot()`; untagged `fail(sampleRateChanged)` | `capturingSession()` and `capturingSampleRate()`; `failSession(observed session, …)` | The callback no longer reads control-owned fields (C-1a, X-1). Cheaper: two loads instead of copying a whole snapshot |
| `beginRecord` | `if (begin(...)) clock_.record();` | **code unchanged**, comment only | `begin()`'s result was already checked: a refused begin starts neither capture nor transport |
| `cancelRecord` | untagged `capture_.cancel()` + transport stop | `failSession(command's session)` + the same unconditional transport stop | P-1b. The stop stays because the transport projection replays the command as a stop (§5) |
| `tryCancelRecording()` | `prepared` → untagged `cancel()`; `capturing` → untagged command | `prepared`/`starting` → `failSession(S)`; `capturing` → command carrying S | P-1b, C-1d |
| `failRecording()` | `snapshot()` check, then `fail()` | `failSession(S, reason)` | C-1d |
| `.h` | — | comment: `recordingSnapshot()` is for the control thread only | Documents the thread contract |

**RT budget.** The callback gains no lock, wait, allocation, I/O or string work,
and no dependency on UI or JUCE.

- **Types:** every new type is `static_assert`-ed `is_always_lock_free`.
- **Cost per operation:** constant and small.
  - `begin()` runs once per take: exactly two CASes (claim, then publication
    or give-back), two relaxed loads and 14 relaxed stores.
  - `capture()` reads the status once, as before.
  - The rate check in `processBlock` is one acquire and one relaxed load,
    instead of a whole snapshot copy.
  - A session-scoped CAS runs only on a failure path.
- **Optimized arm64:** the CASes are single LSE instructions: `casa`/`casl` in
  `begin()`, and `casal` in `fail`, `failSession` and `stop`. There is no
  LL/SC loop, so `compare_exchange_weak` never fails spuriously there.
- **Retries:** the loops in `stop`, `fail` and `failSession` retry only when
  another thread changed the word, and a take has a bounded number of
  transitions.
- **Unrelated processing:** no other audio processing in the callback changed.

**Adapter** (`JuceAudioDeviceAdapter.*`, already modified by C2-2; message
thread only):

- the watchdog also supervises `prepared`;
- its progress signature includes the phase;
- its timeout reads the preflight's `placement.deviceSampleRate`, which is
  known before `begin()`.

**Test fixtures.** Because `begin()` no longer prepares a request itself, the
three direct `begin()` calls in `RecordingCaptureTests.cpp` and the endurance
fixture's `tryRequestRecord` now call `prepareRequest()` first. No assertion was
changed.

## 3. Why these atomics

| Field | Writers | Readers | Choice |
|---|---|---|---|
| `status_` | all | all | `atomic<uint64>`, the single arbiter. 48 bits of session fit with the phase and the reason in one lock-free word, so "still session S, still active" and "terminalize it" are one CAS |
| `track_` | control (`prepareRequest`, `reset`, `prepare`) | control (`snapshot`); callback (`begin`) | atomic: a `begin()` that lost its claim can overlap cleanup (C-1e) |
| `layout_` | control | callback (`capture`), control (`drain`, `snapshot`) | atomic, for the same reason, and because a `capture()` in flight can overlap the next `prepareRequest()` |
| `projectStart_`, `deviceSampleRate_` | callback (`begin`), control (zeroing) | control (`snapshot`), callback (`processBlock`) | atomic: two writers on two threads |
| `placement_` | control | control (`snapshot`) only | **plain**: one thread, so no race is possible |
| ring indices, telemetry, budget | unchanged | unchanged | already atomics |

**Configuration is complete before it is published, and immutable for the
session.**

- The request metadata is fixed before the release CAS to `prepared`, and it
  changes again only after the take is inactive.
- The two begin values are written once, while `starting` is held, before the
  release CAS to `capturing`.
- No field of a live session is ever rewritten. The atomics exist only for
  stale readers: a `begin()` that lost its claim, or a callback in flight
  across cleanup.

Rejected alternatives:

- **A mutex or a wait:** forbidden on the callback.
- **A seqlock:** it would need retry loops, and only four fields cross threads.
- **"Reads only while prepared":** it is exactly the assumption C-1 broke.

The fields the callback reads are atomics. The single-thread field stays plain.

## 4. Memory guarantees

1. **Request publication.** `prepareRequest()` writes the metadata, then performs
   a release CAS to `prepared(S)`. `begin()`'s acquire claim reads that value,
   so the callback sees the request it validates.
2. **Begin publication.** `begin()` zeroes the indices and counters and writes
   the start position and rate, then performs a release CAS to `capturing(S)`.
   - Every later change of the status word up to the next `reset()` is an RMW:
     `stop`, `fail` or `failSession`. These continue the release sequence
     (C++20 [intro.races]).
   - So any acquire load that reads `capturing(S)`, `complete(S)` or
     `failed(S)` sees `begin()`'s writes. This covers `snapshot()`, `drain()`
     and the CAS loops.
   - This is why a capturing or terminal snapshot carries the begin values and
     a consistent frame count.
3. **No begin values while `prepared`.** `snapshot()` reports `prepared` (which
   includes `starting`) without start, rate or frames, whatever `begin()` is
   writing at that moment. A prepared snapshot is therefore always coherent
   (C-1c).
4. **Terminal transitions.** CAS with release on success and acquire on
   failure, so a retry re-reads the current word with its publications.
5. **Inactive bookkeeping.** `reset()` and `prepare()` store idle (release) only
   from an inactive state. No RMW can change an inactive word: `begin()` needs
   `prepared`, `stop()` needs `capturing`, and the failures need an active
   phase. The plain store is therefore never lost.
6. **PCM.** The SPSC ring is unchanged:
   - `capture()` writes samples, then stores `writeFrame_` (release);
   - `drain()` reads `writeFrame_` (acquire), then the samples, then stores
     `readFrame_` (release);
   - `capture()` reads `readFrame_` (acquire) before computing free space.
7. **Relaxed loads** of `track_`, `layout_`, start and rate take their ordering
   from the acquire on the status word that precedes them. Atomicity matters
   only in the stale cases, where a reader that lost its claim overlaps a
   rewrite. That reader's publication or `failSession` then fails, whatever
   value it read.

## 5. Protection against stale commands

Four layers, in order:

1. **Generation (unchanged).** Every lifecycle transition, and every Record
   preflight (it detaches the callback), closes `CommandLifecycleGate`.
   `consumeCommands()` skips commands of an older generation. In the
   application, this alone already drops every command older than the current
   take.
2. **Session tag.**
   - `beginRecord(S)` claims only `prepared(S)`.
   - `cancelRecord(S)` and `failRecording(S)` act only on `active(S)`.
   - A failure the callback detects acts only on the session it read as
     `capturing`.
   - A stale command can therefore neither start nor end a later take, even
     within one generation and even when that take is already prepared.
3. **FIFO for the untagged transport effects.** Stop, and the transport stop of
   a Cancel, carry no session.
   - Commands are consumed in order, and a Cancel for S is enqueued only while
     S is capturing. Take S' can be prepared only after S is cleaned up.
   - So when a stale Cancel is consumed, the later take is **at most prepared**:
     its `beginRecord` was enqueued after the Cancel, and stopping a stopped
     transport is harmless.
   - The same order means that a Stop consumed after `beginRecord(S')` was issued
     after it, so it belongs to S'.
   - The transport projection already replays a Cancel as a stop, which is why
     its transport stop stays unconditional.
4. **The result of `begin()` is honoured.** `consumeCommands()` calls
   `clock_.record()` only if `begin()` returned true. A refused begin starts
   neither capture nor transport (`staleBeginAfterCleanup`,
   `cancelBeforeFirstCallback`).

**Assumption: session ids are unique per take.**

- The adapter guarantees it: `nextRecordingSession_` increases for the whole run
  and skips ids whose published file exists, and `0` means exhaustion.
- `RealtimeCapture` does not enforce it. A caller that reused one id for two
  takes would defeat layer 2 for that pair.

## 6. Watchdog for `prepared` (M-1b)

| Question | Answer |
|---|---|
| Scope | `prepared` and `capturing` (`superviseRecordingCallbacks`) |
| When the deadline starts | At the first service pass that observes the take's signature (session, phase, accepted frames). For a prepared take, that is the first timer tick after Record was accepted. This is after the preflight has returned, so any device reopen in `setAudioDeviceSetup` is excluded |
| What counts as progress | A change of session, phase or accepted frames. Accepted frames are **not** assumed to measure progress in `prepared`: none exist before the first callback, and the snapshot reports 0 there. The first callback is visible as `prepared → capturing`, and it also adds frames from then on |
| Session association | The signature includes the session, and the failure is `failRecording(session, deviceStalled)`, which is session-scoped. If the take already moved on (Stop consumed, Cancel, device event, a new take), the CAS has no effect |
| Stop pending in `prepared` | Stop is queued and cannot be consumed without callbacks. After T the watchdog fails the take; cleanup retains the empty temporary. If callbacks resume, the stale `beginRecord` is refused and the Stop finds nothing to stop (`m1b_prepared_stall_terminalizes`, `m1b_device_resumes_after_prepared_failure`) |
| Cancel in `prepared` | Direct, with no command: `failSession(S, cancelled)`. The watchdog plays no part |
| Threshold | **Unchanged.** T = max(1 s, 10 × buffer / rate), computed from the preflight's placement. There is no technical evidence for a different value (§9) |
| False positive | Needs at least T with no first callback after the preflight returned. Message-thread delays can only delay detection. A 700 ms start-up is tolerated, and the deadline restarts at the first callback (`m1b_first_callback_rearms_watchdog`: failed 1005 ms after the first callback, not 300 ms after it) |

## 7. Tests

### 7.1 Core suite, single interleaving

`tests/RecordingStateProtocolTests.cpp`, CTest `vitadaw_recording_state_protocol_tests`.

| Case | Property | `57d18d7` |
|---|---|---|
| `beginOnlyFromThePreparedSession` | idle, another session, another track or layout, or an invalid rate: refused without effect; no begin while capturing | n/a (new API) |
| `beginRefusedAfterTerminalStates` | after cancel, `failSession`, Stop, a failure while capturing, and cleanup: refused, state unchanged; session-id range | n/a |
| `staleBeginAgainstANewPreparedSession` | take 30's command against prepared take 31: refused; 31 intact | n/a |
| `failSessionIsScopedAndFinal` | only its own session, only once; the first reason stays | n/a |
| `lateCallbackAfterFailure` | nothing captured, reason kept, exact retained frames | n/a |
| `stopBeforeAnyFrame` | complete with 0 frames | n/a |
| `staleBeginAfterCleanup` | **P-1**, engine, with no Stop queued: not revived, transport not started | **FAIL** |
| `staleBeginCannotStartANewPreparedSession` | queue `[begin 80 (stale), Stop, begin 81]`: only 81 begins, with its own request and frames | pass |
| `staleCancelCannotFailANewSession` | queue `[cancel 90 (stale), begin 91]`: 91 records | **FAIL** (P-1b) |
| `cancelBeforeFirstCallback` | terminal at once; `beginRecord` inert; next take normal | pass |
| `stopBeforeFirstCallback` | `beginRecord` and Stop in one callback: complete with 0 frames, transport stopped | pass |
| `lateCallbackAfterEngineFailure` | watchdog failure, then two late callbacks | pass |
| `healthyFirstTakeAndSilence` | first take of a fresh engine: exact, no drops. Digital silence captured as such | pass |
| `repeatedRecordFailRecoveryCycles` | 30 cycles on one engine, rotating normal, Cancel before callback, writer failure, device loss with restart, and Stop before callback. Each ends in its own state with exactly its own frames; cleanup leaves the ring idle, the counters zero and the transport stopped | pass |

### 7.2 Core suite, concurrency

Each round runs the same way:

- **Start:** both threads meet at a rendezvous, and the last to arrive picks a
  common start time 20 µs ahead.
- **Offset:** one side is then delayed by a calibrated, pseudo-random sweep of
  ±2 µs (±4 µs on the engine). Across rounds, either operation can come first,
  down to the nanoseconds that decide a collision.
- **Assertions:** each round checks **only allowed outcomes**, never a winner.
- **Coverage evidence:** the distributions below show that the decisive
  interleavings occurred.

| Case | Allowed | Required every round | Debug | RelWithDebInfo | TSan `-O0` | TSan `-O1` |
|---|---|---|---|---|---|---|
| `failureRacesBegin` (20 000 rounds) | `begin()` true or false; block captured or not | failed with the control thread's reason; retained = accepted, exact | begin won 8630; **failure inside `starting`: 1198** | 8798; **843** | 6794; **2546** | 7723; **1527** |
| `cleanupRacesBegin` (20 000) | same | failure, `reset()` and the next `prepareRequest()` while `begin()` may still run: the next take is prepared with exactly its own request, and the last take records exactly | cleanup inside `starting` in ≥ 1274 rounds | ≥ 500 | ≥ 2283 | ≥ 1535 |
| `stopRacesFailure` (20 000) | Stop or failure wins | exactly one terminal; `failSession()`'s result matches it | 10368 / 9632 | 10791 / 9209 | 10321 / 9679 | 9978 / 10022 |
| `snapshotDuringBegin` (4 000) | any phase sequence `prepared → capturing → complete` | every snapshot is one whole take; no begin values while prepared; begin values and a whole, non-decreasing count once begun; never back to prepared | 3989 rounds saw the transition | 3998 | 3915 | 3997 |
| `cancelRacesBeginOnTheEngine` (3 000) | `begin` refused; or begun, then failed directly or by command | the control call succeeds; ends failed with its reason; exact frames; cleanup leaves idle and stopped; a final take records normally | 1109 with the block / 1891 without | 1287 / 1713 | 461 / 2539 | 615 / 2385 |

### 7.3 Adapter suite (real `JuceAudioDeviceAdapter`, `DawApplication`, JUCE; virtual device)

| Case | Result |
|---|---|
| `m1b_prepared_stall_terminalizes` (formerly RED) | **PASS**: Stop pending, no callback; failed 1018 ms after Record; retained (0 frames); transport stopped |
| `p1_stale_begin_cannot_revive_cleaned_session` (formerly RED) | **PASS**: capture idle, session 0, 0 frames, transport not playing; still cancelled after 10 more callbacks; next take published |
| `c1_prepared_transition_under_concurrency` | **PASS**; TSan-clean at `-O0` and `-O1` |
| `m1b_prepared_stall_without_commands` | **PASS**: not before T; retained; transport stopped |
| `m1b_first_callback_rearms_watchdog` | **PASS**: first callback 700 ms after Record; failed 1005 ms after it |
| `m1b_slow_first_callback_is_not_a_stall` | **PASS**: a 700 ms start-up, then 1.2 s live; published intact |
| `m1b_device_resumes_after_prepared_failure` | **PASS**: 20 resumed callbacks begin nothing; next take published |
| `stop_before_first_callback_leaves_nothing` | **PASS**: complete with 0 frames → "Capture did not complete successfully; recording media retained for recovery" (existing empty-take rule); nothing committed or published; next take published |
| `cycles_of_failure_and_recovery` | **PASS**, five cycles: capturing stall, prepared stall, Cancel before callback, device loss with restart, normal take. Each failure retains exactly its own media |

The C2-2 watchdog cases (`m1_*`) and the C2-1 GREEN cases are unchanged and pass.

### 7.4 The same tests against `57d18d7`

- **Method:** an unmodified tree from `git archive HEAD` in a scratch directory
  (the working tree and the repository were untouched). It ran the engine-level
  cases of the new suite, which use only APIs that already existed there.
  `deviceStalled` did not exist yet, so it was replaced by `writerFailed`, a
  failure of the same kind.
- **Failures in Debug:** `staleBeginAfterCleanup`,
  `staleCancelCannotFailANewSession`, `cancelRacesBeginOnTheEngine` (lost
  failure) and `snapshotDuringBegin` (incoherent snapshot).
- **Under TSan:** both concurrent cases report the data race.
- **Pass there as well (regression guards):** the other six.

## 8. Sanitizers

### 8.1 Results

| Build | Scope | Result |
|---|---|---|
| ASan + UBSan (`build-asan`, `-O0`) | all 101 tests | 98 PASS + 3 RED (H-1, H-2, R-2; `CONTRACT VIOLATED`, exit 3); **0** AddressSanitizer errors, **0** UBSan runtime errors |
| TSan `-O0` (`build-tsan`) | all 101 tests | 98 PASS + 3 RED; **0** ThreadSanitizer warnings. No suppression file exists or was added |
| TSan `-O1` (`build-tsan-o1`, new) | all 101 tests | 98 PASS + 3 RED; **0** ThreadSanitizer warnings |
| TSan, `57d18d7` scratch tree | §7.4 | data race reported in both concurrent cases |

### 8.2 A blind spot in TSan `-O0`

**What:** on this toolchain (Apple clang 21, arm64), ThreadSanitizer does not
report races through **struct copies** that the compiler lowers to
`__tsan_memcpy`, and at `-O0` every struct copy is lowered that way. Scalar
races are reported.

**Probe** (scratch directory), an unsynchronised write against a read:

| Access | `-O0` | `-O1` |
|---|---|---|
| 8-byte struct | **not reported** | reported |
| 32-byte struct | not reported | not reported |
| scalar | reported | reported |

`intercept_intrin=1` (the default) does not change this.

**Consequences:**

- The C2-2 `-O0` evidence for C-1 rested on the scalar fields `session_` and
  `layout_`. This is why C-1e (`track_`, an 8-byte struct) appeared only under
  `-O1`.
- The whole battery was therefore also run under TSan `-O1`.
- `placement_` (about 80 bytes) is still invisible to TSan at both levels. Its
  race-freedom rests on the argument in §3: one thread writes and reads it.

## 9. Residual limits

1. **Session-id uniqueness is an assumption** of the Core protocol (§5),
   enforced by the adapter, not by `RealtimeCapture`.
2. **Stop is untagged.** It is protected by the generation and by FIFO order
   (§5), not by the session.
3. **Stale traces after a lost race.** Both can happen when a failure and its
   cleanup coincide with a callback in flight:
   - a `begin()` that lost its claim to a failure may leave its start, rate and
     zeroed counters in the ring after cleanup;
   - a `capture()` in flight while cleanup runs can advance the indices of the
     now idle ring.

   Neither does any harm:
   - prepared snapshots hide these values, `drain()` ignores idle and
     prepared rings, and the next `begin()` overwrites them;
   - in the application, the next Record's preflight calls `prepare()`, which
     zeroes the whole ring;
   - only a `telemetry()` read in between could show them.
4. **Tail frames of a failed take.** A callback already in flight when a take
   fails can publish one last block after the final drain; cleanup then resets
   the ring and that block is not in the retained temporary. Scope and decision
   in §13: failed takes only, at most one callback block, never a published
   take.
5. **Prepared-phase false positive.** If a device's first callback after the
   preflight takes at least T (1 s for common buffers), the take fails as
   `deviceStalled` instead of starting late. Virtual tests cover 700 ms, and
   there is no physical measurement of first-callback latency. **Physical smoke
   PS-2.**
6. **TSan coverage.** Large struct copies are invisible to TSan (§8.2), and
   sanitizers only judge the interleavings that ran. The protocol arguments in
   §2–§5 are what cover the rest.
7. **X-1 is closed by construction only.** No test can pre-empt the callback
   inside `capture()`.
8. **Out of scope and unchanged.** H-1 and H-2 (continuity, rate window, C2-4)
   and R-2 (reconnection, C2-R2) stay RED. The empty-take message ("Capture did
   not complete successfully") is the existing finalization rule.

## 10. Batteries

| Battery | Result |
|---|---|
| `ctest`, Debug, 101 tests (94 before, + 6 adapter cases, + 1 Core suite) | **98 PASS, 3 RED**: exactly H-1, H-2, R-2 (`characterization-red`, exit 3, `CONTRACT VIOLATED`) |
| `ctest -LE characterization-red` | **98/98** |
| The 60 pre-0.8.3-C tests | **60/60** |
| RelWithDebInfo with recording-service diagnostics (`build-diag-083b2`) | 98 PASS, 3 RED |
| ASan+UBSan / TSan `-O0` / TSan `-O1` | each 98 PASS, 3 RED; 0 sanitizer reports |
| `characterization-tsan` label | no longer used (0 tests) |

## 11. Physical smoke

A physical smoke is needed. Three things changed:

- the callback's capture path (`begin`, `capture`, the rate check);
- the start of every take;
- the watchdog, which now acts before the first callback.

| ID | Procedure | Pass criterion |
|---|---|---|
| PS-1 | Ten normal Record/Stop takes on the usual interface: short, long (≥ 5 min), and back to back | All published, sample-exact length; no failure message; B2 diagnostics unchanged |
| PS-2 | First Record right after launch; after the interface was idle for several minutes; after switching input channels; with an aggregate or Bluetooth input if available | **No `deviceStalled`**. Record how long the first callback took if the diagnostics show it |
| PS-3 | Record, then Stop immediately (before audio can arrive) | No hang. Either a normal short take or the empty-take message; nothing broken; next take normal |
| PS-4 | If reproducible: a dead but still listed device (S-6b), **then press Record** | Before C2-2b this hung in `prepared`. Now: `deviceStalled` after about T, temporary retained, VitaDAW responsive |
| PS-5 | C2-2 S-6, S-8 and S-9 (sleep/wake, CPU load, largest buffer) | As in C2-2 §10 |

## 12. Physical smokes performed (0.8.3-C consolidation)

Performed by the user on a MacBook Air, macOS, with the diagnostics build
`build-diag-083b2/vitadaw_app_artefacts/RelWithDebInfo/VitaDAW.app`, stall
injection off (every summary reports 0 injected stalls).

**Build identity.** The executable was linked at 2026-10-13 12:00:54 by the
final validation run, after the last functional change of C2-2b (`track_`
atomic, `RealtimeCapture.h` 11:58:02). It was not rebuilt before the smokes,
whose media are timestamped 17:36–17:57. The only later source edits (12:05)
are comments: rebuilding them gives instruction-identical objects
(`RealtimeCapture.cpp.o`, `JuceAudioDeviceAdapter.cpp.o`, compared with
`objdump -d`). The smokes therefore ran the final C2-2b code, on top of
`57d18d7`; that code had no commit of its own at the time.

| Smoke | Setting | Terminal | Evidence | Result |
|---|---|---|---|---|
| Normal recording (C2-2b PS-1, partial) | — | `complete` | Record, Stop, playback correct | PASS |
| Stop at once (PS-3) | several cycles; e.g. session 6, 0.866 s | `complete / none` | max service gap 37.7 ms, peak 2048 frames, 0 dropped, 0 overflow; clips published | PASS |
| UI stress during a take (S-8) | session 7, 48 kHz / 512, 122.688 s; zoom, scroll, minimise, Load / Save As dialogs | `complete / none` | max gap 613.8 ms, peak 29 696 frames, min headroom 66 304 frames (1.381 s), 10 late service events, 0 dropped, 0 overflow; `Recording 000007.wav` published; **no false `deviceStalled`** | PASS |
| Largest buffer (S-9) | session 9, 48 kHz / 4096, 39.239 s | `complete / none` | max gap 38.8 ms, peak 4096 frames, min headroom 91 904 frames (1.915 s), 0 dropped, 0 overflow | PASS |
| Sleep during a take (S-6) | session 10, 48 kHz / 512 | `failed / deviceStalled` after 10.374 s (wall clock, including about T without callbacks) | max gap 164.8 ms, 0 dropped, 0 overflow; UI: "Recording stopped because the audio device stopped delivering audio; recording media retained for recovery"; no clip published | PASS: safe failure |
| Record after wake (S-6) | session 11, 4.821 s | `complete / none` | max gap 40.4 ms, 0 dropped; published without restarting VitaDAW | PASS: recovery |

**What S-6 does and does not show.**

- It shows that macOS sleep stops callbacks without a lifecycle notification
  (M-1 is real on hardware), that the watchdog ends the take safely, and that
  a new take works after waking.
- The retained temporary of session 10 holds 449 024 frames (9.355 s, 877
  whole 512-frame blocks), consistent with the wall-clock time minus the
  watchdog threshold.
- It does **not** validate recovery of the partial content: the accepted-frame
  count was not captured, so equality with the temporary is not established,
  and the recovery flow itself was not exercised. The UI message alone proves
  only that the media was retained.

**Still to be done on hardware (code unchanged since these smokes):**

- PS-1 in full: a take of at least 5 minutes, and ten back-to-back takes.
- PS-2: first Record right after launch, after the interface idled for
  minutes, after switching input channels, and with an aggregate or Bluetooth
  input if available. This is the prepared-watchdog false-positive check.
- PS-4: Record against a dead but listed device (only if reproducible; do not
  force it on the user's hardware).
- S-8 under CPU load (the run above stressed the UI, not the CPU).
- Recovery of the S-6 temporary through the recovery flow, comparing frames.

## 13. Decision: frames lost at cleanup

Read-only investigation of "frames arriving between the last drain and cleanup
are lost".

- **Were they accepted?** Yes. Only frames that `capture()` already wrote and
  published (`writeFrame_` release, `acceptedFrames_` counted) are concerned.
  Audio that never reached the ring is a different matter (H-1).
- **Last drain.** `DawApplication::synchroniseRecording()` calls
  `serviceRecording()`, which drains in a loop until the ring is empty, before
  it reads the terminal phase. `finalizeRecording()` and `shutdownRecording()`
  drain the same way first.
- **Cleanup.** `discardRecordingWithDiagnostics()` closes and syncs the
  writer (it does not drain again), retains the temporary, then
  `resetRecordingCapture()` zeroes the ring.
- **When can a frame be lost?** Only if a callback that read `capturing`
  before the failure publishes its block after the final drain loop. After the
  terminal transition `capture()` refuses every new block, so the exposure is
  at most **one callback block**, in flight at the moment of failure.

| Ending | Exposure |
|---|---|
| Stop (normal) | **None.** The callback consumes Stop before capturing and completes the take itself; no capture follows in that callback, and finalization drains everything first |
| Cancel while capturing | None: consumed by the callback, as Stop |
| Overflow | None of the accepted frames: the overflowing block is rejected whole; earlier frames are drained |
| `deviceStalled` | Only if a callback arrives exactly as the watchdog fires, after at least T of silence |
| `deviceLost` | Only if a callback is in flight as the loss is processed; a stopped device normally has none |
| `writerFailed` | Frames after the failure are not written by design: the writer itself failed |
| Shutdown | One in-flight block at most |

- **Quantifiable?** Yes in principle: `acceptedDeviceFrames` minus the frames
  written. No current path records that difference.
- **Contract.** Nothing lost here is ever published: every case is a failed
  take, and a published take requires `complete`, which the callback itself
  sets before it stops capturing. The recovery policy retains the failed
  take's temporary as best-effort material; C2-2 §6 already lists this
  residual. C2-2 §8's "retained = accepted" holds in the automated tests,
  where no callback is in flight.
- **Classification:** LOW, documented limitation; not an integrity breach and
  not blocking. If recovery must become frame-exact, a later phase can make
  cleanup wait for the callback to observe the terminal state, or record the
  difference in the recovery marker.

## 14. Next phases of 0.8.3-C

1. Remaining physical smokes (§12).
2. **C2-R2** — reopen a reconnected interface (R-2).
3. **C2-3** — xrun telemetry.
4. **C2-4** — host-time continuity: H-1, H-2 and the sub-threshold window of
   C2-2 §4.
5. Optional: frame-exact retention of failed takes (§13).

UX issues found during the smokes are in the existing backlog
([`audit-recording-physical-latency-0.8.3B2.md`](audit-recording-physical-latency-0.8.3B2.md) §10)
and stay outside Recording Reliability.
