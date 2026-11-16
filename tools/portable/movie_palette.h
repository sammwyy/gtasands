#ifndef GTASADS_MOVIE_PALETTE_H
#define GTASADS_MOVIE_PALETTE_H

#include <stdint.h>

void movie_palette_build(const uint32_t histogram[32768],
                         uint8_t palette[768], uint8_t lookup[32768]);
void movie_palette_frame(const uint8_t *rgb, uint8_t *pixels,
                         const uint8_t lookup[32768]);

#endif
