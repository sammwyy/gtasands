#ifndef GTASADS_TXD_H
#define GTASADS_TXD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[33];
    uint16_t width;
    uint16_t height;
    uint32_t raster;
    uint8_t dxt;
    uint8_t has_alpha;
    uint8_t level_count;
    const uint8_t *palette;
    const uint8_t *level_data;
    size_t level_size;
} txd_texture;

int txd_get(const uint8_t *data, size_t size, size_t index, txd_texture *texture);
int txd_decode(const txd_texture *texture, size_t level,
               uint8_t **rgba, uint16_t *width, uint16_t *height);

#endif
