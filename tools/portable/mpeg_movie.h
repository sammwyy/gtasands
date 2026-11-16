#ifndef GTASADS_MPEG_MOVIE_H
#define GTASADS_MPEG_MOVIE_H

#include <stddef.h>
#include <stdint.h>

int mpeg_movie_convert(const uint8_t *source, size_t length,
                       uint16_t fps, uint32_t audio_rate,
                       uint8_t **output, size_t *output_length,
                       uint32_t *frame_count);

#endif
