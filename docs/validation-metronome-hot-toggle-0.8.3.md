# 0.8.3 — Metronome hot toggle

Base: `1b518788995d4e4341c3f304b358219a20baba77`, `main`.
This increment concerns metronome enable/disable during Play and Recording.

## Audit before implementation

- UI: `MainWindow.cpp`, `metronomeEnabled_.onClick`, dispatches
  `SetMetronomeEnabled`; the control is already enabled during Play/Recording.
- Command path: `CommandDispatcher` → `DawApplication::handle` → `execute` →
  `JuceAudioDeviceAdapter::trySetMetronomeEnabled` →
  `RealtimeAudioEngine::trySetMetronomeEnabled` → existing bounded command FIFO.
  Play is already admitted. The recording-busy allowlist in `handle` rejected
  this command during a take. The loopback-test exclusion remains unchanged.
- RT: `consumeCommands` applies the boolean on the audio thread. The existing
  release/acquire queue transfers the command; enabled state is published by
  the transport exchange. No new shared state or synchronization is needed.
- Scheduling: `prepareMetronomeEvents` reads enabled state and the exact
  project positions. Enabling between beats waits for the next grid event,
  rather than replaying a partial tick or resetting the musical phase.
- DSP: prepared normal/accent sine tables have a quadratic decay of about
  4 ms (`ceil(deviceRate / 250)` frames at standard rates). Previously OFF
  called `clearMetronomeRuntime`, abruptly discarding a voice inside its table.
  That introduces an extra waveform discontinuity if the current value is
  nonzero. Level smoothing alone cannot repair a discarded voice.
- Output: both legacy and prepared processing paths add the click to master,
  after master inserts and before master gain/output metering.
- Input: the production JUCE callback passes raw input and output separately
  to `processBlock`. Capture copies raw input into its ring before output is
  cleared/rendered. Monitoring stages raw input independently, then mixes it
  into output after the master render. Neither route feeds output back to
  capture in software. Physical acoustic/electrical feedback is still input.

## Minimal change

Admit `SetMetronomeEnabled` through the recording-busy allowlist. Keep the
existing command and queue; level changes during Recording retain their
existing admission policy.

When disabling during running transport, clear pending scheduling but let
already-started voices finish their prepared envelope, with at most about
4 ms of remaining tail. Enabled state becomes false at command consumption;
no subsequent beats start while disabled. When Stopped/Paused, the existing
full clear remains. Re-enabling does not reset any active voice or clock phase.

No Stop/Play/Seek, device restart, reprepare, extra timer, filesystem access,
lock or allocation is introduced by the toggle. Existing run-until-stop
behavior is preserved. No changes to capture, ring, writer service, placement,
watchdog, recovery, C2/C2-2b, Monitoring policy, arm state, Double Stop, VERSION,
README or the three characterization-red tests.

## Automated regression coverage

`LoopMetronomeTests`:

- Start Play OFF with real backing audio, enable between beats, disable inside
  the next tick, and verify exact monotonic transport position and applied state.
- At 44.1/48/96 kHz and three cut positions, compare every output sample against
  backing audio plus a continuously enabled reference's entire natural tick.
  Verify no mid-beat onset, no truncated tail, no later tick, no backing-audio
  interruption, and zero callback allocations.
- Deliver 2,000 enable/disable attempts concurrently with 2,000 64-frame audio
  blocks. Saturation may reject commands; final OFF must apply and position
  must remain monotonic. Transport snapshots have their single non-RT reader
  on the control thread, matching the exchange contract.

`ProjectMediaIntegrationTests`:

- Use the existing virtual device and real production callback, dispatcher,
  adapter, writer, finalization and WAV reader. No audio hardware is needed.
- Repeat with Monitoring manually OFF and ON. Record 61,440 frames at 48 kHz
  / 256 frames, toggling ON/OFF four times within the same take/session.
- Verify state, accepted-frame count, exact position, unchanged placement,
  arm, history and Monitoring; zero callback allocations, drops and overflow.
- Make input silent around an enabled beat and prove click energy reaches the
  output, even with Monitoring ON. Read the published WAV and compare every
  sample to the known dyadic input, including exact silence around that beat.
- Stop finalizes normally and leaves Monitoring in its manually selected state.

Both new regressions failed against the previous production code: the click
continuity comparison detected the cut, and the real command path rejected the
toggle during Recording. They pass after the two production changes.

An initial TSan run found a test-harness error: the new concurrent test called
the single-reader transport snapshot API from both threads. The test was
corrected to read only from the control thread; no production change or
sanitizer suppression was made for that finding.

## Validation — 2026-10-28

- Build: `cmake --build build -j 8`, PASS including Debug `vitadaw_app`.
- Diagnostics app: `cmake --build build-diag-083b2 --target vitadaw_app -j 8`,
  PASS (RelWithDebInfo, existing diagnostics option ON).
- Focused Debug CTest selection: 9/9 PASS, 26.55 s (Loop/Metronome, Project
  Media Integration, Input Monitoring Device Lifecycle, Transport State, Input
  Monitoring, Recording Capture, Recording State Protocol, Recording Application,
  Transport Timeline Foundation).
- Final `ctest --test-dir build --output-on-failure -LE characterization-red`:
  98/98 PASS, 185.66 s.
- Full `ctest --test-dir build --output-on-failure`: 98 PASS and exactly the
  three known characterization-red failures (H-2, H-1, R-2), 164.12 s; expected
  CTest exit 8. No additional failures.
- ASan + UBSan (`build-asan`, existing `-fsanitize=address,undefined`): final
  focused selection 4/4 PASS, 56.62 s, no sanitizer reports.
- TSan (`build-tsan-o1`, existing `-fsanitize=thread -O1`): final focused
  selection 4/4 PASS, 35.69 s, no sanitizer reports. Both sanitizer selections
  cover Loop/Metronome, Project Media Integration, Input Monitoring and Recording
  Application; the metronome suite includes concurrent producer/consumer use.
- The configured CTest count remains 101; assertions extend two existing test
  executables. No new CTest registration or CMake change.
- Final sanitizer logs checked for AddressSanitizer errors, UBSan runtime errors
  and ThreadSanitizer warnings: none. No suppressions added.
- Full diff reviewed; `git diff --check`: PASS. Four tracked files modified
  (application admission, metronome RT branch and two test files), plus this
  new validation document. Historical untracked artifacts remain outside the
  increment. Physical smoke subsequently approved by the user as recorded below.

## Physical smoke reported by the user — 2026-10-28

PASS, on the real application at 48 kHz / 512 frames, using the updated
diagnostics build. The user approved ON/OFF during both Play and Recording:
no transport Stop/restart, playhead movement or phase reset; the same take/session
continues; Input Monitoring remains unchanged; no problematic switching clicks.
OFF lets an already-started voice finish naturally and cancels later beats;
ON re-enters the existing grid. The metronome is not printed in the captured WAV.

A subsequently displayed physical loopback measurement may show
`Failed / Signal Too Low`. That result belongs to the separate loopback
measurement test and does not constitute a Metronome Hot Toggle failure.

The automated counters above remain automated evidence; the hardware result
here records the user's functional acceptance.

## Repeatable physical smoke procedure

Use headphones and a real input signal without master-output loopback into
the input. A microphone or cable that physically returns the metronome will
legitimately record it as raw input; that is not software capture contamination.

For recording-service telemetry, the updated development diagnostics app is:
`build-diag-083b2/vitadaw_app_artefacts/RelWithDebInfo/VitaDAW.app`.
Launch its executable from Terminal with stall injection disabled:

```sh
env -u VITADAW_DIAG_SERVICE_STALLS_MS -u VITADAW_DIAG_SERVICE_STALL_FIRST_MS -u VITADAW_DIAG_SERVICE_STALL_SPACING_MS /Users/serx/Documents/ChatGPT/VITADAW/build-diag-083b2/vitadaw_app_artefacts/RelWithDebInfo/VitaDAW.app/Contents/MacOS/VitaDAW
```

Confirm the startup banner says `stall plan: none`. Keep existing log history;
record the new session IDs and summaries from
`~/Library/Logs/VitaDAW/recording-service-diagnostics.log` (unless overridden by
`VITADAW_DIAG_SUMMARY_FILE`). The ordinary Debug app is also updated, but does
not emit recording-service diagnostics.

1. Set 48 kHz / 512 frames. Open a project with playable audio. Note Monitoring
   state and set the metronome OFF. Play for at least 30 s, toggle ON/OFF at
   least ten times, including rapid changes. Confirm continuous backing audio,
   no Stop/restart/playhead jump, grid-aligned ticks and no extra switch pop.
2. With Monitoring manually OFF, arm one track, save the project, and Record a
   real signal for at least 60 s. Toggle ON/OFF at least ten times in the same
   take. Confirm no interruption, new session, arm change or Monitoring change.
   Stop and verify the take publishes and Monitoring remains OFF.
3. Turn the metronome OFF for playback and audition the recorded clip/WAV,
   including quiet input passages around toggles. Verify the output click has
   not been printed. Listen to the WAV separately if there is any ambiguity.
4. Repeat steps 2–3 with Monitoring manually ON. Confirm it remains ON across
   Record, all toggles and Stop. Keep output volume/physical routing appropriate
   for monitoring the real input without feedback.

For each take, record: build, interface/input/output, rate, buffer, duration,
number/timing of ON/OFF changes, Monitoring before/during/after, session ID,
terminal phase/failure, dropped frames, overflow events, maximum service gap,
injected-stall count, audible anomalies and WAV-isolation result. Expected:
`complete / none`, zero drops, zero overflow, zero injected stalls and no
`deviceStalled`. The injected-stall count is not a general hardware-xrun counter.
