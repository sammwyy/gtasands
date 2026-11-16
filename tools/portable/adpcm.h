#ifndef GTASADS_ADPCM_H
#define GTASADS_ADPCM_H

#include <stddef.h>
#include <stdint.h>

int adpcm_encode(const int16_t *pcm, size_t count, int with_checkpoints,
                 uint8_t **output, size_t *length, uint8_t **checkpoints,
                 size_t *checkpoint_length);

#endif
