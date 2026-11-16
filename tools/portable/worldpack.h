#ifndef GTASADS_WORLDPACK_H
#define GTASADS_WORLDPACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[24];
    uint16_t txd, draw_distance;
    uint8_t time_on, time_off;
    uint16_t radius;
    uint32_t flags;
} worldpack_model;

typedef struct {
    char name[24];
    int16_t parent;
    uint16_t pack;
} worldpack_txd;

typedef struct {
    double pos[3], rot[9];
    uint16_t model, switch_flags, linked, area;
} worldpack_instance;

typedef struct {
    const uint8_t *record;
    size_t length;
    uint16_t radius;
} worldpack_collision;

typedef struct {
    int16_t ox, oy;
    uint16_t sector, grid_width, grid_height;
    const worldpack_model *models;
    size_t model_count;
    const worldpack_txd *txds;
    size_t txd_count;
    const uint16_t *cell_counts;
    const worldpack_instance *instances;
    size_t instance_count;
    const worldpack_collision *collisions;
} worldpack_input;

int worldpack_encode(const worldpack_input *input, uint8_t **output, size_t *length);

#endif
