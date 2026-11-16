import re
from array import array
from functools import lru_cache
from pathlib import Path


TAPS = 48
PHASES = 512
SINE_STEPS = 8192
BITS = 20
TABLE_PATH = Path(__file__).resolve().parent.parent / 'portable/resample_coeffs.h'


def nearest(value, divisor):
    quotient, remainder = divmod(abs(value), divisor)
    if remainder * 2 > divisor or (remainder * 2 == divisor and quotient & 1):
        quotient += 1
    return -quotient if value < 0 else quotient


@lru_cache(maxsize=1)
def _tables():
    source = TABLE_PATH.read_text()
    def values(name, count):
        body = source.split(f'{name}[{count}] = {{', 1)[1].split('}', 1)[0]
        return tuple(map(int, re.findall(r'-?\d+', body)))
    return values('gt_sine', SINE_STEPS + 1), values('gt_window', TAPS)


def _sine_ratio(numerator, denominator, sine):
    cycles, remainder = divmod(abs(numerator), denominator)
    position, fraction = divmod(remainder * SINE_STEPS, denominator)
    value = sine[position] + nearest((sine[position + 1] - sine[position]) *
                                     fraction, denominator)
    return -value if (numerator < 0) ^ bool(cycles & 1) else value


@lru_cache(maxsize=16)
def _kernel(source_rate, target_rate):
    sine, window = _tables()
    cutoff_num, cutoff_den = target_rate * 23, source_rate * 25
    cutoff = nearest(cutoff_num << 30, cutoff_den)
    table = []
    for phase in range(PHASES):
        values = []
        for tap in range(TAPS):
            distance = (tap - (TAPS // 2 - 1)) * PHASES - phase
            value = cutoff
            if distance:
                sin_value = _sine_ratio(distance * cutoff_num,
                                        PHASES * cutoff_den, sine)
                denominator = nearest(3373259426 * abs(distance), PHASES)
                value = nearest(sin_value * (1 << 30), denominator)
                if distance < 0:
                    value = -value
            values.append(nearest(value * window[tap], 1 << 30))
        total = sum(values)
        if total <= 0:
            raise ValueError('invalid resampling kernel')
        coefficients = [nearest(value * (1 << BITS), total) for value in values]
        coefficients[TAPS // 2 - 1] += (1 << BITS) - sum(coefficients)
        table.append(tuple(coefficients))
    return tuple(table)


def downsample(samples, source_rate, target_rate):
    if not 0 < target_rate < source_rate <= 192000:
        raise ValueError('invalid resampling rates')
    count = (len(samples) * target_rate + source_rate - 1) // source_rate
    if not count:
        return array('h')
    kernel = _kernel(source_rate, target_rate)
    result = array('h', bytes(count * 2))
    for i in range(count):
        position = (2 * i + 1) * source_rate - target_rate
        denominator = target_rate * 2
        center, remainder = divmod(position, denominator)
        phase = (remainder * PHASES + denominator // 2) // denominator
        if phase == PHASES:
            center += 1
            phase = 0
        total = 0
        for tap, coefficient in enumerate(kernel[phase]):
            index = min(len(samples) - 1, max(0, center + tap - (TAPS // 2 - 1)))
            total += samples[index] * coefficient
        result[i] = max(-32768, min(32767, nearest(total, 1 << BITS)))
    return result
