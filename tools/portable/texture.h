#ifndef GTASADS_TEXTURE_H
#define GTASADS_TEXTURE_H

#include "dtex.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    dtex_options options;
} texture_target;

typedef struct {
    char name[33];
    uint16_t width, height;
} texture_info;

int texture_inspect(const uint8_t *input, size_t size,
                    texture_info **textures, size_t *count);
int texture_convert_txd(const uint8_t *input, size_t size, const texture_target *targets,
                        size_t count, uint8_t **output, size_t *length);
uint8_t *texture_resize_rgba(const uint8_t *src, unsigned sw, unsigned sh,
                             unsigned tw, unsigned th);

#endif
