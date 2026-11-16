#ifndef GTASADS_PEDPACK_H
#define GTASADS_PEDPACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t models, stats, anims, groups, zones;
    uint32_t nodes, links, road_nodes, road_links;
    int16_t gx0, gy0;
    uint16_t gw, gh, cell;
    const uint8_t *parts[14];
    size_t lengths[14];
} pedpack_input;

int pedpack_encode(const pedpack_input *input, uint8_t **output, size_t *length);
int pedpop_rescale(int32_t *rows, size_t count);
int pedpop_parse(const char *text, size_t length, int32_t **rows, size_t *count);

typedef struct {
    int16_t x, y, z;
    uint8_t width;
    uint32_t flags, cell;
    const uint16_t *links;
    size_t link_count;
} pedpack_path_node;

int pedpack_paths(const pedpack_path_node *nodes, size_t count, size_t cell_count,
                  int road, uint8_t **node_blob, size_t *node_length,
                  uint8_t **link_blob, size_t *link_length,
                  uint8_t **cells, size_t *cells_length, uint32_t *links);

#endif
