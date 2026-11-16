#!/usr/bin/env python3
"""Generate integer sine and Blackman lookup tables for portable audio resampling."""
import argparse
import math
from pathlib import Path


OUTPUT = Path(__file__).resolve().parent / 'portable/resample_coeffs.h'
SINE_STEPS = 8192
TAPS = 48
SCALE = 1 << 30


def table(name, values):
    rows = [', '.join(str(value) for value in values[i:i + 8])
            for i in range(0, len(values), 8)]
    return f'static const int32_t {name}[{len(values)}] = {{\n' + \
        ''.join(f'    {row},\n' for row in rows) + '};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    args = parser.parse_args()
    sine = [round(math.sin(math.pi * i / SINE_STEPS) * SCALE)
            for i in range(SINE_STEPS + 1)]
    window = [round((0.42 - 0.5 * math.cos(2 * math.pi * i / (TAPS - 1)) +
                     0.08 * math.cos(4 * math.pi * i / (TAPS - 1))) * SCALE)
              for i in range(TAPS)]
    args.output.write_text('#ifndef GTASADS_RESAMPLE_COEFFS_H\n'
                           '#define GTASADS_RESAMPLE_COEFFS_H\n'
                           '#include <stdint.h>\n'
                           + table('gt_sine', sine)
                           + table('gt_window', window)
                           + '#endif\n')


if __name__ == '__main__':
    main()
