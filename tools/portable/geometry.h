#ifndef GTASADS_GEOMETRY_H
#define GTASADS_GEOMETRY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char texture[32];
    uint16_t color;
    uint8_t alpha;
    uint8_t reserved;
    uint32_t first;
    uint32_t count;
} geometry_material;

int geometry_encode(const double *positions, const double *uvs, const double *normals,
                    const uint8_t *colors, const uint8_t *bones,
                    const uint16_t *indices, size_t vertex_count, size_t index_count,
                    const geometry_material *materials, size_t material_count,
                    uint8_t **output, size_t *length);

#endif
