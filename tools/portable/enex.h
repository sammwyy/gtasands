#ifndef GTASADS_ENEX_H
#define GTASADS_ENEX_H

#include "ipl.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double values[11];
    int32_t flags, area, sky, peds, on, off;
    char name[128];
    uint16_t source_area;
} enex_entry;

int enex_parse(const char *text, size_t length, const ipl_instance *placed,
                size_t placed_count, enex_entry **entries, size_t *count);
int enex_pack(enex_entry *entries, size_t count, uint8_t **output,
               size_t *length, uint32_t *exterior, uint32_t *linked);

#endif
