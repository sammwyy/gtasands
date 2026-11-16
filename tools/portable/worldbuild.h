#ifndef GTASADS_WORLDBUILD_H
#define GTASADS_WORLDBUILD_H

#include <stddef.h>
#include <stdint.h>

typedef int (*worldbuild_read_file)(void *context, const char *path,
                                    uint8_t **data, size_t *length);
typedef int (*worldbuild_read_range)(void *context, uint32_t pack,
                                     uint32_t offset_low, uint32_t offset_high,
                                     uint32_t length, uint8_t *destination);

typedef struct {
    void *context;
    worldbuild_read_file file;
    worldbuild_read_range range;
    double bounds[4];
    uint16_t sector;
    uint8_t use_bounds, keep_lods;
} worldbuild_options;

typedef struct {
    uint32_t models, txds, instances, sectors, col_models, col_faces;
    uint32_t col_dups, skip_models, skip_lods, skip_bounds;
    uint64_t col_bytes;
} worldbuild_stats;

int worldbuild_convert(const worldbuild_options *options,
                       uint8_t **output, size_t *length,
                       worldbuild_stats *stats);

#endif
