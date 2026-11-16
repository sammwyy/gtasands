#ifndef GTASADS_OGG_DECODE_H
#define GTASADS_OGG_DECODE_H

#include <stddef.h>
#include <stdint.h>

int ogg_decode_vorbis(const uint8_t *source, size_t length,
                      int16_t **pcm, size_t *samples,
                      uint32_t *sample_rate, uint16_t *channels);

#endif
