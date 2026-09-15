"""Independent unbounded-integer/Fraction oracle. Not linked into production/RT."""
import math
import random
import subprocess
import sys
from fractions import Fraction

rng = random.Random(600)
mask = (1 << 256) - 1
commands = []
expected = []

def words(value):
    return " ".join(f"{(value >> bit) & ((1 << 64)-1):x}" for bit in (192, 128, 64, 0))

def case(operation, a, b, result):
    for backend in (0, 1):
        commands.append(f"{operation} {backend} {words(a)} {words(b)}")
        expected.append(result)

values = [0, 1, 2, mask] + [1 << i for i in range(256)]
values += [rng.getrandbits(256) for _ in range(400)]
for index, a in enumerate(values):
    b = values[(index * 37 + 11) % len(values)]
    case("add", a, b, ((a+b) & mask, int(a+b > mask)))
    case("sub", a, b, ((a-b) & mask, int(a < b)))
    case("div", a, b, (*divmod(a, b), 1) if b else (0, 0, 0))
    x, y = a & ((1 << 128)-1), b & ((1 << 128)-1)
    case("mul", x, y, (x*y,))
    shift = index % 258
    case("left", a, shift, ((a << shift) & mask, int(a << shift > mask)))
    case("right", a, shift, (a >> shift,))

rates = [1., 2.**20, 44100., 48000., 96000., 48000.0024,
         float.fromhex('0x1.77000000002dcp-38')]
rates += [math.nextafter(rng.uniform(1., 2.**20), math.inf) for _ in range(150)]
for index, rate in enumerate(rates):
    for operation, inputs, value in [
        ('decode', rate.hex(), Fraction(rate)),
        ('ratio', rate.hex()+' '+rates[(index+1) % len(rates)].hex(),
         Fraction(rate)/Fraction(rates[(index+1) % len(rates)]))]:
        fits = max(value.numerator.bit_length(), value.denominator.bit_length()) <= 128
        commands.append(f'{operation} 0 {inputs}')
        expected.append((value.numerator, value.denominator, 1) if fits else (0, 1, 0))

# Exact source decisions: the production driver returns its integer remainder,
# so the oracle never tests merely an approximate interpolation coefficient.
source_cases = [
    (48000., 96000., float.fromhex('0x1.77000000002dcp-38'),
     9007199254739992., float.fromhex('0x1.1374bc6af9097p-53'),
     9007199254739992, -.5, 1),
]
audit_position = Fraction(source_cases[0][4]) + (Fraction(source_cases[0][5]) - Fraction(1,2)) * Fraction(source_cases[0][2])/48000
assert 1-audit_position == Fraction(69,5070602400912917605986812821504000)
for _ in range(200):
    p, d, s = [rng.choice(rates[:6]) for _ in range(3)]
    length = float(rng.choice([1, 2, 3, 1024, (1 << 53)-1]))
    offset = rng.choice([0., .25, float.fromhex('0x1.0000000000001p-64'), float((1 << 52)+1)])
    frame = rng.choice([0, 1, int(length)-1, int(length)])
    phase = rng.choice([-.5, -.25, 0., .25, .5])
    source_cases.append((p, d, s, length, offset, frame, phase, rng.choice([1, 3, 4096, (1 << 53)-1])))
source_expected = {}
for p, d, s, length, offset, frame, phase, count in source_cases:
    x = Fraction(frame)+Fraction(phase)
    y = Fraction(offset)+x*Fraction(s)/Fraction(p)
    inside = 0 <= x < Fraction(length) and 0 <= y < count
    for backend in (0, 1):
        source_expected[len(commands)] = (inside, y)
        commands.append(f'source {backend} {p.hex()} {d.hex()} {s.hex()} {length.hex()} '
                        f'{offset.hex()} {frame} {phase.hex()} {count}')
        expected.append(None)

clock_expected = {}
clock_rates = [(1.,2.), (2.,1.), (3.,2.), (4.,3.), (147.,160.), (160.,147.),
               (44100.,48000.), (48000.,44100.), (48000.,96000.), (96000.,48000.),
               (48000.,48000.0024),
               (float.fromhex('0x1.bbbbbbbbbbbb9p+15'), float.fromhex('0x1.4cccccccccccbp+15')),
               (float.fromhex('0x1.cb60000000004p+15'), float.fromhex('0x1.5888000000003p+15'))]
for p, d in clock_rates:
    for count in (1, 3, 100003):
        start = float((1 << 53)-200001) if count == 3 else 0.
        end = start + (4. if count == 3 else 1.)
        step = Fraction(p)/Fraction(d)
        final = Fraction(start)+count*step
        distance = (Fraction(end)-Fraction(start))/step
        first_end = -(-distance.numerator//distance.denominator)
        if first_end > count: first_end = 0
        clock_expected[len(commands)] = (final, first_end, 0)
        commands.append(f'clock 0 {p.hex()} {d.hex()} {start.hex()} {count} {end.hex()} 0x0p0 0x0p0')
        expected.append(None)
    # Loop boundaries and phase remain exact after many wraps.
    count=10003; start=1.25; end=33.5
    step=Fraction(p)/Fraction(d); length=Fraction(end)-Fraction(start)
    final=Fraction(start)+(count*step)%length
    wraps=(count*step)//length
    clock_expected[len(commands)]=(final,0,wraps)
    commands.append(f'clock 0 {p.hex()} {d.hex()} {start.hex()} {count} {end.hex()} {start.hex()} {end.hex()}')
    expected.append(None)

process = subprocess.run([sys.argv[1]], input='\n'.join(commands)+'\n',
                         text=True, capture_output=True, check=True)
lines = process.stdout.splitlines()
assert len(lines) == len(expected), (len(lines), len(expected), process.stderr)
for index, (command, line, reference) in enumerate(zip(commands, lines, expected)):
    actual = tuple(int(word, 16) for word in line.split())
    if index in source_expected:
        inside, y = source_expected[index]
        assert actual[0] == 1, ('preparation', command)
        assert bool(actual[1]) == inside, (command, actual, y)
        if inside:
            assert actual[2] == y.numerator//y.denominator, (command,actual,y)
            assert Fraction(actual[3],actual[4]) == y-actual[2], (command,actual,y)
        continue
    if index in clock_expected:
        final, first_end, wraps = clock_expected[index]
        valid, frame, negative, numerator, denominator, got_end, got_wraps = actual
        result=Fraction(frame)+(-1 if negative else 1)*Fraction(numerator,denominator)
        assert valid and result == final and got_end == first_end and got_wraps == wraps, (command,actual,final,first_end,wraps)
        continue
    assert actual == reference, (command, actual, reference)
print(f'{len(expected)} fixed-word/backend checks passed against independent Python integers/Fraction')
