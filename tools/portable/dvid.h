#ifndef GTASADS_DVID_H
#define GTASADS_DVID_H

#include <stddef.h>
#include <stdint.h>

int dvid_pack(const uint8_t *palette, size_t palette_size,
              const uint8_t *frames, size_t frame_size,
              const uint8_t *audio, size_t audio_size,
              uint16_t fps, uint32_t rate,
              uint8_t **output, size_t *length, uint32_t *frame_count);

#endif
