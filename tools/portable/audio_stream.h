#ifndef GTASADS_AUDIO_STREAM_H
#define GTASADS_AUDIO_STREAM_H

#include <stddef.h>
#include <stdint.h>

int audio_stream_convert(const uint8_t *encrypted, size_t length,
                         uint32_t offset_low, uint32_t offset_high,
                         uint32_t target_rate, uint8_t format,
                         int seekable, double volume, double max_seconds,
                         uint8_t **output, size_t *output_length);

#endif
