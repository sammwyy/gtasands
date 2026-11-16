#ifndef GTASADS_PEDBUILD_H
#define GTASADS_PEDBUILD_H

#include <stddef.h>
#include <stdint.h>

typedef int (*pedbuild_read_file)(void *context, const char *path,
                                  uint8_t **data, size_t *length);

typedef struct {
    void *context;
    pedbuild_read_file file;
    double bounds[4];
} pedbuild_options;

typedef struct {
    uint32_t models, stats, anims, groups, zones, nodes, links;
    uint32_t road_nodes, road_links, scripted;
    int32_t script_start, script_end;
    uint16_t grid_width, grid_height;
} pedbuild_stats;

int pedbuild_convert(const pedbuild_options *options, uint8_t **output,
                     size_t *length, pedbuild_stats *stats);

#endif
