#ifndef GTASADS_DTEX_H
#define GTASADS_DTEX_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    DTEX_AUTO = 0,
    DTEX_A3I5 = 1,
    DTEX_PAL4 = 2,
    DTEX_PAL16 = 3,
    DTEX_PAL256 = 4,
    DTEX_A5I3 = 6,
    DTEX_DIRECT = 7
} dtex_format;

typedef struct {
    dtex_format format;
    uint8_t alpha_threshold;
    uint16_t auto_colors;
    uint8_t opaque;
    uint8_t additive;
} dtex_options;

int dtex_encode(const uint8_t *rgba, uint16_t width, uint16_t height,
                dtex_options options, uint8_t **output, size_t *length);
void dtex_free(void *output);

#endif
