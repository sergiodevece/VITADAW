# VitaDAW 0.6.1 — Musical Time validation

## Scope

This increment hardens the existing musical-time domain without changing the
transport, loop, metronome or audio callback contracts. Project frames remain
the temporal authority. Musical ticks are a prepared, derived coordinate.

## Exact query model

- `exactProjectFrameAtTick()` returns `audio::exact::Position`, including its
  optional subframe component.
- `absoluteTickAt()` returns the greatest tick whose exact boundary is not
  greater than the queried position.
- Tempo segments are half-open. At the exact anchor of the next segment, the
  new segment is selected before its internal tick search begins.
- `projectFrameAt()` implements floor, ceil and nearest directly over the
  rational position. Nearest ties round upward.
- `musicalPositionAt()`, `tempoAt()` and `timeSignatureAt()` use the exact
  inverse; presentation seconds/quarters are not involved.
- Grid enumeration uses an exact inverse for its first tick, advances prepared
  tempo/signature cursors monotonically and converts to double only for output.

Every successfully compiled `PreparedMusicalTimeMap` is exact-certified.
Invalid musical documents, out-of-range coordinates, project-frame overflow
and fixed-width certification failure remain distinct errors.

## Directed tests

`MusicalTimeTests` covers:

- 44.1, 48 and 96 kHz;
- 120 BPM, 123 BPM and both adjacent binary64 values;
- the exact 48 kHz/123 BPM beat boundary `960000/41`;
- forward/inverse round-trips at tick 0, beat boundaries and ticks near `2^40`;
- the greatest-tick inverse invariant for integer project positions;
- exact before/at/after checks around tempo and metric anchors;
- new-segment ownership at an exact tempo anchor;
- 4/4, 3/4 and 7/8 decomposition and changes by bar anchor;
- exact floor/ceil/nearest, including upward ties;
- the documented non-reversibility when multiple ticks round to one integer
  project frame;
- large event collections and grid continuation;
- separate invalid-document, out-of-range, overflow and certification errors.

The independent Python `Fraction` oracle verifies forward positions, inverse
round-trips, segment tempo, time signature, bar/beat/tick decomposition and all
three rounding modes. It runs against normal, forced-portable and fast-math
drivers; all discrete answers are identical.

Persistence tests round-trip 123 BPM and both `nextafter` neighbours by comparing
their `uint64_t` binary representations. PPQ, tempo IDs, signature IDs and bar
anchors remain unchanged under schema v3.

Loop/metronome regression renders the real engine for project/device rate pairs
44.1→48, 48→96 and 96→44.1 kHz. Single-block and partitioned renders are
bit-identical, and loop/metronome consume the same exact anchor.

## Results

- Complete JUCE build: success.
- Complete suite: 43/43 passed.
- Core-only build (`VITADAW_BUILD_APP=OFF`): success.
- Core-only suite: 37/37 passed.
- Directed musical/oracle/loop/persistence selection: 6/6 passed.
- ASan + UBSan core: 37/37 passed, no diagnostics.
- ASan + UBSan JUCE integration and directed musical tests: 12/12 passed,
  no diagnostics.
- UBSan core: 37/37 passed, no diagnostics.
- TSan core: 37/37 passed, no race diagnostics.
- `git diff --check`: clean.

## Remaining limitations

- Musical tempo remains step-only and time signatures remain bar-anchored.
- The musical coordinate domain remains `[0, 2^40]` ticks.
- Configurations whose exact accumulated anchors exceed the certified fixed
  128/256-bit capacity are rejected during preparation.
- Seconds, quarter positions and grid pixels remain presentation doubles and
  are not discrete temporal authorities.
- There is no promise that a rounded integer project frame maps back to the
  tick from which it was rounded.
