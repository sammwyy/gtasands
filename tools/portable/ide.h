#ifndef GTASADS_IDE_H
#define GTASADS_IDE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t id;
    char name[128], txd[128];
    double draw_distance;
    int64_t flags;
    int32_t on, off;
} ide_object;

typedef struct {
    char name[128], parent[128];
} ide_parent;

int ide_parse(const char *text, size_t length, ide_object **objects, size_t *object_count,
              ide_parent **parents, size_t *parent_count);

#endif
