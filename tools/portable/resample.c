#include "resample.h"

#include "resample_coeffs.h"

#include <stdlib.h>

enum { TAPS = 48, PHASES = 512, SINE_STEPS = 8192, COEFFICIENT_BITS = 20 };

static int64_t nearest(int64_t value, int64_t divisor) {
    int negative = value < 0;
    uint64_t absolute = (uint64_t)(negative ? -value : value);
    uint64_t quotient = absolute / (uint64_t)divisor;
    uint64_t remainder = absolute % (uint64_t)divisor;
    if (remainder * 2 > (uint64_t)divisor ||
        (remainder * 2 == (uint64_t)divisor && (quotient & 1))) ++quotient;
    return negative ? -(int64_t)quotient : (int64_t)quotient;
}

static int64_t sine_ratio(int64_t numerator, int64_t denominator) {
    int negative = numerator < 0;
    uint64_t absolute = (uint64_t)(negative ? -numerator : numerator);
    uint64_t cycles = absolute / (uint64_t)denominator;
    uint64_t scaled = (absolute % (uint64_t)denominator) * SINE_STEPS;
    size_t position = (size_t)(scaled / (uint64_t)denominator);
    int64_t value = gt_sine[position] +
                    nearest((int64_t)(gt_sine[position + 1] - gt_sine[position]) *
                            (int64_t)(scaled % (uint64_t)denominator), denominator);
    return (negative ^ (int)(cycles & 1)) ? -value : value;
}

static int kernel(uint32_t source_rate, uint32_t target_rate,
                  int32_t *coefficients) {
    int64_t cutoff_num = (int64_t)target_rate * 23;
    int64_t cutoff_den = (int64_t)source_rate * 25;
    int64_t cutoff = nearest(cutoff_num << 30, cutoff_den);
    for (int phase = 0; phase < PHASES; ++phase) {
        int64_t raw[TAPS], total = 0, rounded = 0;
        for (int tap = 0; tap < TAPS; ++tap) {
            int64_t distance = (tap - (TAPS / 2 - 1)) * PHASES - phase;
            int64_t value = cutoff;
            if (distance) {
                int64_t sine = sine_ratio(distance * cutoff_num,
                                          PHASES * cutoff_den);
                int64_t denominator = nearest(3373259426LL *
                                               llabs(distance), PHASES);
                value = nearest(sine * (1LL << 30), denominator);
                if (distance < 0) value = -value;
            }
            raw[tap] = nearest(value * gt_window[tap], 1LL << 30);
            total += raw[tap];
        }
        if (total <= 0) return 0;
        for (int tap = 0; tap < TAPS; ++tap) {
            int64_t value = nearest(raw[tap] * (1LL << COEFFICIENT_BITS), total);
            coefficients[phase * TAPS + tap] = (int32_t)value;
            rounded += value;
        }
        coefficients[phase * TAPS + TAPS / 2 - 1] +=
            (int32_t)((1LL << COEFFICIENT_BITS) - rounded);
    }
    return 1;
}

int resample_pcm16(const int16_t *input, size_t count,
                   uint32_t source_rate, uint32_t target_rate,
                   int16_t **output, size_t *output_count) {
    if ((!input && count) || !output || !output_count ||
        !source_rate || !target_rate || source_rate > 192000 ||
        target_rate >= source_rate) return 0;
    uint64_t n = ((uint64_t)count * target_rate + source_rate - 1) / source_rate;
    if (n > SIZE_MAX / sizeof(int16_t)) return 0;
    int16_t *result = n ? malloc((size_t)n * sizeof *result) : NULL;
    int32_t *coefficients = malloc((size_t)PHASES * TAPS * sizeof *coefficients);
    if ((n && !result) || !coefficients ||
        !kernel(source_rate, target_rate, coefficients)) {
        free(result);
        free(coefficients);
        return 0;
    }
    for (size_t i = 0; i < (size_t)n; ++i) {
        uint64_t position = ((uint64_t)i * 2 + 1) * source_rate - target_rate;
        uint64_t denominator = (uint64_t)target_rate * 2;
        int64_t center = (int64_t)(position / denominator);
        size_t phase = (size_t)((position % denominator) * PHASES +
                                denominator / 2) / (size_t)denominator;
        if (phase == PHASES) {
            ++center;
            phase = 0;
        }
        int64_t sum = 0;
        for (int tap = 0; tap < TAPS; ++tap) {
            int64_t index = center + tap - (TAPS / 2 - 1);
            if (index < 0) index = 0;
            if ((uint64_t)index >= count) index = (int64_t)count - 1;
            sum += (int64_t)input[index] * coefficients[phase * TAPS + tap];
        }
        int64_t sample = nearest(sum, 1LL << COEFFICIENT_BITS);
        if (sample < -32768) sample = -32768;
        if (sample > 32767) sample = 32767;
        result[i] = (int16_t)sample;
    }
    free(coefficients);
    *output = result;
    *output_count = (size_t)n;
    return 1;
}
