#ifndef GTASADS_PEDPATH_H
#define GTASADS_PEDPATH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t area, node;
} pedpath_link;

typedef struct {
    uint32_t index;
    int16_t x, y, z;
    uint8_t width, link_count;
    uint32_t flags;
    pedpath_link links[15];
} pedpath_node;

int pedpath_parse(const uint8_t *data, size_t length, const double bounds[4], int vehicles,
                  pedpath_node **output, size_t *count);

#endif
