#ifndef GTASADS_WORLDPLAN_H
#define GTASADS_WORLDPLAN_H

#include "ipl.h"
#include "img.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t id;
    char name[128];
    uint8_t available;
} worldplan_model;

typedef struct {
    int32_t model, linked;
    double position[3], rotation[9];
    uint8_t is_lod, area;
} worldplan_instance;

typedef struct {
    char path[256];
    uint8_t kind;
} worldplan_file;

typedef struct {
    int32_t id;
    char name[128], txd[128];
    double draw_distance;
    int64_t flags;
    uint8_t on, off;
} worldplan_object;

typedef struct {
    char name[128], parent[128];
} worldplan_parent;

typedef struct {
    char name[128];
    double draw_distance;
    int64_t flags;
    uint64_t offset;
    uint32_t size;
    uint16_t txd, pack;
    uint8_t on, off;
} worldplan_model_entry;

typedef struct {
    char name[128];
    int16_t parent;
    uint16_t pack;
} worldplan_txd_entry;

int worldplan_parse_dat(const char *source, size_t length,
                        worldplan_file **files, size_t *count);
int worldplan_instances(const ipl_instance *text, size_t text_count,
                        const ipl_instance *stream, size_t stream_count,
                        const worldplan_model *models, size_t model_count,
                        const double *bounds, int keep_lods,
                        worldplan_instance **output, size_t *count,
                        uint32_t skipped[3]);
int worldplan_resolve(const char *name, const img_entry *main, size_t main_count,
                      const img_entry *interior, size_t interior_count,
                      img_entry *found, int *pack);
int worldplan_sectors(const worldplan_instance *instances, size_t count, int sector,
                      int16_t *ox, int16_t *oy, uint16_t *width, uint16_t *height,
                      uint16_t **counts, size_t **order);
int worldplan_tables(const int32_t *ids, size_t count,
                     const worldplan_object *objects, size_t object_count,
                     const worldplan_parent *parents, size_t parent_count,
                     const img_entry *main, size_t main_count,
                     const img_entry *interior, size_t interior_count,
                     worldplan_model_entry **models,
                     worldplan_txd_entry **txds, size_t *txd_count);

#endif
