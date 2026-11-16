#ifndef GTASADS_VEHBUILD_H
#define GTASADS_VEHBUILD_H

#include <stddef.h>
#include <stdint.h>

typedef int (*vehbuild_read_file)(void *context, const char *path,
                                  uint8_t **data, size_t *length);

typedef struct {
    void *context;
    vehbuild_read_file file;
} vehbuild_options;

typedef struct {
    uint32_t vehicles, colours, surfaces, no_handling, water, track_nodes;
} vehbuild_stats;

int vehbuild_convert_sources(const uint8_t *const sources[10],
                             const size_t sizes[10], uint8_t **output,
                             size_t *length, vehbuild_stats *stats);
int vehbuild_convert(const vehbuild_options *options, uint8_t **output,
                     size_t *length, vehbuild_stats *stats);

#endif
