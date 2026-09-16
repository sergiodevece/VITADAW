"""Independent exact musical preparation oracle using unbounded Fraction."""
import math
import random
import struct
import subprocess
import sys
from fractions import Fraction

PPQ = 15360
rng = random.Random(600123)

cases = [
    (48000.0, [(0, 123.0)], [(0, 4, 4), (2, 7, 8)],
     [0, PPQ - 1, PPQ, PPQ + 1, 8 * PPQ]),
    (48000.0, [(0, math.nextafter(123.0, math.inf))], [(0, 3, 4)],
     [PPQ, 7 * PPQ]),
    (48000.0, [(0, math.nextafter(123.0, 0.0))], [(0, 7, 8)],
     [PPQ, 7 * PPQ]),
    (44100.0, [(0, 120.0), (777, 123.5),
               (2 * PPQ, 97.25),
               (9 * PPQ, 211.0)],
              [(0, 4, 4), (3, 7, 8), (9, 3, 4)],
              [0, 776, 777, PPQ, 2 * PPQ, 5 * PPQ, 9 * PPQ, 17 * PPQ]),
]
for _ in range(40):
    rate = math.nextafter(rng.uniform(1.0, 1048576.0), math.inf)
    events = [(0, math.nextafter(rng.uniform(20.0, 400.0), math.inf))]
    queries = [0, rng.randrange(1, 200000), rng.randrange(200000, 1000000)]
    cases.append((rate, events, [(0, 4, 4)], queries))
for _ in range(20):
    ticks = [0, rng.randrange(1, 50000), rng.randrange(50001, 100000)]
    bpms = [rng.choice([60.0, 97.25, 120.0, 123.0, 123.5, 211.0])
            for _ in ticks]
    events = list(zip(ticks, bpms))
    cases.append((rng.choice([44100.0, 48000.0, 96000.0]), events,
                  [(0, 4, 4), (2, 7, 8), (6, 3, 4)],
                  ticks + [ticks[-1] + 100000]))

lines = []
expected = []
def bits(value):
    return struct.unpack(">Q", struct.pack(">d", value))[0]

for rate, events, signatures, queries in cases:
    lines.append(f"{rate.hex()} {len(events)} {len(signatures)} {len(queries)}")
    lines.extend(f"{tick} {bpm.hex()}" for tick, bpm in events)
    lines.extend(f"{bar} {numerator} {denominator}"
                 for bar, numerator, denominator in signatures)
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
        position = (anchors[segment] +
            (query - tick) * Fraction(rate) / (256 * Fraction(bpm)))
        signature_anchors = [(signatures[0][0], 0,
                              PPQ * 4 // signatures[0][2],
                              signatures[0][1])]
        for bar, numerator, denominator in signatures[1:]:
            previous_bar, previous_tick, previous_beat, previous_numerator = signature_anchors[-1]
            start_tick = previous_tick + (bar - previous_bar) * previous_beat * previous_numerator
            signature_anchors.append((bar, start_tick, PPQ * 4 // denominator,
                                      numerator))
        signature_index = max(i for i, item in enumerate(signature_anchors)
                              if item[1] <= query)
        start_bar, start_tick, ticks_per_beat, numerator = signature_anchors[signature_index]
        delta = query - start_tick
        bar = start_bar + delta // (ticks_per_beat * numerator)
        beat = (delta % (ticks_per_beat * numerator)) // ticks_per_beat
        within = delta % ticks_per_beat
        signature = signatures[signature_index]
        floor_value = position.numerator // position.denominator
        ceil_value = floor_value + (position.denominator != 1)
        remainder = position.numerator % position.denominator
        nearest_value = floor_value + (2 * remainder >= position.denominator)
        expected.append((position, query, floor_value, ceil_value, nearest_value,
                         bar, beat, within, bits(bpm), signature[1], signature[2]))

process = subprocess.run([sys.argv[1]], input="\n".join(lines) + "\n",
                         text=True, capture_output=True, check=True)
actual_lines = process.stdout.splitlines()
assert len(actual_lines) == len(expected), (len(actual_lines), len(expected), process.stderr)
for actual_line, reference in zip(actual_lines, expected):
    position = reference[0]
    words = actual_line.split()
    assert words[0] == "1", (actual_line, reference)
    frame = int(words[1])
    numerator = (int(words[2], 16) << 64) | int(words[3], 16)
    denominator = (int(words[4], 16) << 64) | int(words[5], 16)
    actual = Fraction(frame) + Fraction(numerator, denominator)
    assert actual == position, (actual, position)
    returned = (int(words[6]), int(words[7]), int(words[8]), int(words[9]),
                int(words[10]), int(words[11]), int(words[12]), int(words[13], 16),
                int(words[14]), int(words[15]))
    assert returned == reference[1:], (returned, reference[1:])

assert expected[2][0] == Fraction(960000, 41)
print(f"{len(expected)} exact musical boundaries passed against independent Fraction")
