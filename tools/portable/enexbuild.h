#ifndef GTASADS_ENEXBUILD_H
#define GTASADS_ENEXBUILD_H

#include <stddef.h>
#include <stdint.h>

typedef int (*enexbuild_read_file)(void *context, const char *path,
                                   uint8_t **data, size_t *length);

typedef struct {
    void *context;
    enexbuild_read_file file;
} enexbuild_options;

typedef struct {
    uint32_t entries, exterior, linked;
} enexbuild_stats;

int enexbuild_convert(const enexbuild_options *options, uint8_t **output,
                      size_t *length, enexbuild_stats *stats);

#endif
