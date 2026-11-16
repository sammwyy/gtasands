#ifndef GTASADS_PEDSCM_H
#define GTASADS_PEDSCM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char label[9];
    int32_t population, races;
    uint8_t gang[10], dealer;
    uint8_t has_population;
} pedscm_zone;

int pedscm_scan(const uint8_t *source, size_t length, pedscm_zone **zones,
                size_t *count, int32_t *start, int32_t *end);

#endif
