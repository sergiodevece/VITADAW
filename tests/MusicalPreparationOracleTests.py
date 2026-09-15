"""Independent exact musical preparation oracle using unbounded Fraction."""
import math
import random
import subprocess
import sys
from fractions import Fraction

PPQ = 15360
rng = random.Random(600123)

cases = [
    (48000.0, [(0, 123.0)], [0, PPQ - 1, PPQ, PPQ + 1]),
    (48000.0, [(0, math.nextafter(123.0, math.inf))], [PPQ, 7 * PPQ]),
    (48000.0, [(0, math.nextafter(123.0, 0.0))], [PPQ, 7 * PPQ]),
    (44100.0, [(0, 120.0), (777, 123.5),
               (2 * PPQ, 97.25),
               (9 * PPQ, 211.0)],
              [0, 776, 777, PPQ, 2 * PPQ, 5 * PPQ, 9 * PPQ, 17 * PPQ]),
]
for _ in range(40):
    rate = math.nextafter(rng.uniform(1.0, 1048576.0), math.inf)
    events = [(0, math.nextafter(rng.uniform(20.0, 400.0), math.inf))]
    queries = [0, rng.randrange(1, 200000), rng.randrange(200000, 1000000)]
    cases.append((rate, events, queries))
for _ in range(20):
    ticks = [0, rng.randrange(1, 50000), rng.randrange(50001, 100000)]
    bpms = [rng.choice([60.0, 97.25, 120.0, 123.0, 123.5, 211.0])
            for _ in ticks]
    events = list(zip(ticks, bpms))
    cases.append((rng.choice([44100.0, 48000.0, 96000.0]), events,
                  ticks + [ticks[-1] + 100000]))

lines = []
expected = []
for rate, events, queries in cases:
    lines.append(f"{rate.hex()} {len(events)} {len(queries)}")
    lines.extend(f"{tick} {bpm.hex()}" for tick, bpm in events)
    lines.extend(str(tick) for tick in queries)
    anchors = [Fraction(0)]
    for index in range(1, len(events)):
        previous_tick, previous_bpm = events[index - 1]
        anchors.append(anchors[-1] +
            (events[index][0] - previous_tick) *
            Fraction(rate) / (256 * Fraction(previous_bpm)))
    for query in queries:
        segment = max(i for i, event in enumerate(events) if event[0] <= query)
        tick, bpm = events[segment]
        expected.append(anchors[segment] +
            (query - tick) * Fraction(rate) / (256 * Fraction(bpm)))

process = subprocess.run([sys.argv[1]], input="\n".join(lines) + "\n",
                         text=True, capture_output=True, check=True)
actual_lines = process.stdout.splitlines()
assert len(actual_lines) == len(expected), (len(actual_lines), len(expected), process.stderr)
for actual_line, reference in zip(actual_lines, expected):
    words = actual_line.split()
    assert words[0] == "1", (actual_line, reference)
    frame = int(words[1])
    numerator = (int(words[2], 16) << 64) | int(words[3], 16)
    denominator = (int(words[4], 16) << 64) | int(words[5], 16)
    actual = Fraction(frame) + Fraction(numerator, denominator)
    assert actual == reference, (actual, reference)

assert expected[2] == Fraction(960000, 41)
print(f"{len(expected)} exact musical boundaries passed against independent Fraction")
