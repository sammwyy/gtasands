#include "pedpath.h"

#include <stdlib.h>

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)read16(p) | (uint32_t)read16(p + 2) << 16;
}

int pedpath_parse(const uint8_t *data, size_t length, const double bounds[4], int vehicles,
                  pedpath_node **output, size_t *count) {
    if (!data || !bounds || !output || !count || length < 20) return 0;
    *output = NULL;
    *count = 0;
    uint32_t n = read32(data), nveh = read32(data + 4), nnavi = read32(data + 12);
    if (nveh > n || (uint64_t)n * 28u + (uint64_t)nnavi * 14u + 20u > length)
        return 0;
    size_t link_off = 20u + (size_t)n * 28u + (size_t)nnavi * 14u;
    size_t first = vehicles ? 0 : nveh, last = vehicles ? nveh : n;
    pedpath_node *nodes = last > first ? malloc((last - first) * sizeof *nodes) : NULL;
    if (last > first && !nodes) return 0;
    size_t used = 0;
    for (size_t i = first; i < last; ++i) {
        const uint8_t *p = data + 20 + i * 28 + 8;
        int16_t x = (int16_t)read16(p), y = (int16_t)read16(p + 2);
        if (x < bounds[0] * 8 || x > bounds[2] * 8 ||
            y < bounds[1] * 8 || y > bounds[3] * 8) continue;
        pedpath_node *node = &nodes[used];
        node->index = (uint32_t)i;
        node->x = x;
        node->y = y;
        node->z = (int16_t)read16(p + 4);
        int32_t base = (int16_t)read16(p + 8);
        node->width = p[14];
        node->flags = read32(p + 16);
        node->link_count = (uint8_t)(node->flags & 15);
        if (base < 0 || (uint64_t)(base + node->link_count) * 4u > length - link_off) {
            free(nodes);
            return 0;
        }
        for (size_t j = 0; j < node->link_count; ++j) {
            const uint8_t *link = data + link_off + (size_t)(base + j) * 4;
            node->links[j] = (pedpath_link){read16(link), read16(link + 2)};
        }
        ++used;
    }
    *output = nodes;
    *count = used;
    return 1;
}
