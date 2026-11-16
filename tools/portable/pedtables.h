#ifndef GTASADS_PEDTABLES_H
#define GTASADS_PEDTABLES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char model[24], txd[24];
    uint16_t id, cars;
    uint8_t type, stat, anim, race, flags;
} pedtable_model;

typedef struct {
    double value[10];
} pedtable_stat;

typedef struct {
    char name[16], clips[6][24];
} pedtable_anim;

typedef struct {
    const uint16_t *members;
    size_t count;
} pedtable_group;

typedef struct {
    int16_t bounds[4];
    uint8_t population, races, dealer, gang[10];
} pedtable_zone;

typedef struct {
    const pedtable_model *models;
    size_t model_count;
    const pedtable_stat *stats;
    size_t stat_count;
    const pedtable_anim *anims;
    size_t anim_count;
    const pedtable_group *groups;
    size_t group_count;
    const pedtable_group *cars;
    size_t car_count;
    const int32_t *popcycle;
    size_t pop_count;
    const pedtable_zone *zones;
    size_t zone_count;
} pedtable_input;

int pedtables_encode(const pedtable_input *input, uint8_t **output, size_t *length,
                     size_t offsets[9]);

#endif
