#ifndef GTASADS_OGG_INFO_H
#define GTASADS_OGG_INFO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate;
    uint64_t samples;
    uint16_t small_block, large_block;
    uint8_t channels;
} ogg_vorbis_info;

int ogg_read_vorbis_info(const uint8_t *data, size_t length, ogg_vorbis_info *info);
int ogg_read_stream_info(const uint8_t *data, size_t length,
                         uint32_t *rate, uint32_t *samples,
                         uint32_t *channels);

#endif
