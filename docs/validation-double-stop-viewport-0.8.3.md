# 0.8.3 — Double Stop viewport synchronization

Stop travels from `AudioStatusComponent::dispatch` through the command
dispatcher and `DawApplication` to the existing transport reducer. The reducer
preserves the position when stopping Playing/Paused and locates frame zero
when already Stopped. The application exposes the accepted projected state
immediately, including two Stops issued before an audio callback.

Previously the timeline repainted that position but retained its horizontal
coordinate transform. There is no automatic playback follow. The UI now handles
explicit Stop completion: an accepted Stop from Stopped ending at zero reveals
the origin through `setVisibleStartSeconds(0)`, updates the visual playhead from
the application projection, and synchronizes the scrollbars. Zoom and vertical
scroll are preserved. Rejected Stops and the first Stop from Playing/Paused do
not reposition the viewport.

`TimelineUiFoundationTests` exercises the existing transport reducer with a
scrolled coordinate transform: first Stop, second Stop, rejected Stop, Paused
Stop, and Stop at zero after manual scrolling. It checks position, playhead
coordinates, viewport origin and unchanged zoom. Native component painting and
scrollbar wiring require the following UI smoke; this test does not instantiate
the JUCE window.

## Manual UI smoke procedure

1. Open a project with audio, seek beyond zero, zoom in, then scroll horizontally
   far enough that the project origin is offscreen. Note the zoom and vertical
   scroll position.
2. Play, then press Stop once. Confirm playback stops at its current position
   and the horizontal viewport does not jump to zero.
3. Press Stop again. Confirm the transport display and playhead show frame zero,
   the left edge of the arrange ruler shows the project origin, and the horizontal
   scrollbar is at its minimum. Zoom and vertical scroll must be unchanged.
4. Repeat with two rapid Stop clicks, and with Pause followed by Stop, Stop.
5. While already Stopped at zero, scroll away manually and press Stop. Confirm
   the origin becomes visible again. During Play, manually scrolling must remain
   possible without the viewport being forced back by transport refreshes.
6. With Monitoring OFF, Record and Stop a short take. Confirm the existing first
   Stop behavior, WAV publication and playback; Monitoring stays OFF. Repeat
   with Monitoring manually ON and confirm it remains ON after Stop. Once the
   take is finalized and transport is Stopped, scroll away and press Stop to
   reveal the origin.

Recording, Input Monitoring and transport engine behavior are covered by their
existing regression suites; this increment changes only UI view synchronization.

## Physical validation reported by the user — 2026-10-28

The user confirmed PASS in the real application with the same Debug build:
horizontal timeline scrolling, playback from a nonzero position, first Stop
preserving the existing semantics and viewport, and second Stop returning the
transport, playhead and horizontal viewport to frame zero. Zoom and vertical
scroll were preserved, with correct visual behavior.

An additional physical loopback test on the same build completed at 48 kHz with
a 512-frame buffer and 4/5 valid takes. This is supplementary evidence of no
regression in diagnostics/monitoring, outside the Double Stop functional
acceptance criteria. The other procedure variants above are retained as
repeatable checks; they were not individually confirmed in this report.

## Automated validation — 2026-10-28

- `cmake --build build -j 8`: PASS, including `vitadaw_app` (Debug).
- Focused CTest selection (Timeline UI, Transport State, Transport Timeline,
  Recording Application, Input Monitoring): 5/5 PASS.
- `ctest --test-dir build --output-on-failure -LE characterization-red`:
  98/98 PASS, 165.04 s.
- `ctest --test-dir build --output-on-failure`: 98 PASS, exactly 3 known
  characterization-red failures (H-2, H-1, R-2), 122.15 s; CTest exit 8 expected.
- Registration count remains 101: new assertions extend the existing Timeline
  UI executable rather than adding a CTest registration.
- Full source diff reviewed; `git diff --check`: PASS.
- Core native visual smoke: PASS, reported by the user above.
