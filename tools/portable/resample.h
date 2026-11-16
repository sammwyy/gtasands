#ifndef GTASADS_RESAMPLE_H
#define GTASADS_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

int resample_pcm16(const int16_t *input, size_t count,
                   uint32_t source_rate, uint32_t target_rate,
                   int16_t **output, size_t *output_count);

#endif
