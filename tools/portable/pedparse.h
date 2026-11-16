#ifndef GTASADS_PEDPARSE_H
#define GTASADS_PEDPARSE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t id;
    char model[128], txd[128], type[32], stat[32], anim[128];
    uint32_t cars, flags;
} pedparse_ide_entry;

typedef struct {
    char name[128];
    double values[10];
} pedparse_stat_entry;

typedef struct {
    char name[128];
    double bounds[4];
} pedparse_zone_entry;

int pedparse_ide(const char *text, size_t length,
                 pedparse_ide_entry **entries, size_t *count);
int pedparse_stats(const char *text, size_t length,
                   pedparse_stat_entry **entries, size_t *count);
int pedparse_zones(const char *text, size_t length,
                   pedparse_zone_entry **entries, size_t *count);

typedef struct {
    char name[128];
    uint8_t kind;
} pedparse_anim_event;

int pedparse_animgrp(const char *text, size_t length,
                     pedparse_anim_event **events, size_t *count);

typedef struct {
    char name[128];
    uint16_t index;
} pedparse_named_model;

typedef struct {
    uint16_t group, index;
} pedparse_group_member;

int pedparse_groups(const char *text, size_t length,
                    const pedparse_named_model *models, size_t model_count,
                    uint16_t max_groups, int skip_slashes,
                    pedparse_group_member **members, size_t *member_count,
                    uint16_t *group_count);

#endif
