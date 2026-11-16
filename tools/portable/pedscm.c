#include "pedscm.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pedscm_zone zone;
    uint16_t seen;
} entry;

static int read_value(const uint8_t *data, size_t length, size_t off, int32_t *value,
                      size_t *next) {
    if (off >= length) return 0;
    uint8_t type = data[off];
    if (type == 4 && off + 1 < length) {
        *value = (int8_t)data[off + 1];
        *next = off + 2;
    } else if (type == 5 && off + 2 < length) {
        *value = (int16_t)((uint16_t)data[off + 1] | (uint16_t)data[off + 2] << 8);
        *next = off + 3;
    } else if (type == 1 && off + 4 < length) {
        *value = (int32_t)((uint32_t)data[off + 1] |
                           (uint32_t)data[off + 2] << 8 |
                           (uint32_t)data[off + 3] << 16 |
                           (uint32_t)data[off + 4] << 24);
        *next = off + 5;
    } else return 0;
    return 1;
}

static void label_at(const uint8_t *data, size_t length, size_t offset, char out[9]) {
    size_t n = length > offset + 3 ? length - offset - 3 : 0;
    if (n > 8) n = 8;
    for (size_t i = 0; i < n; ++i) {
        if (!data[offset + 3 + i]) {
            n = i;
            break;
        }
        out[i] = (char)toupper(data[offset + 3 + i]);
    }
    out[n] = 0;
}

static entry *zone_for(entry **zones, size_t *count, size_t *capacity,
                        const char label[9]) {
    for (size_t i = 0; i < *count; ++i)
        if (!strcmp((*zones)[i].zone.label, label)) return &(*zones)[i];
    if (*count == *capacity) {
        size_t n = *capacity ? *capacity * 2 : 32;
        if (n <= *capacity || n > SIZE_MAX / sizeof **zones) return NULL;
        entry *reallocated = realloc(*zones, n * sizeof *reallocated);
        if (!reallocated) return NULL;
        *zones = reallocated;
        *capacity = n;
    }
    entry *z = &(*zones)[(*count)++];
    memset(z, 0, sizeof *z);
    memcpy(z->zone.label, label, 9);
    z->zone.population = -1;
    z->zone.races = -1;
    return z;
}

int pedscm_scan(const uint8_t *source, size_t length, pedscm_zone **zones,
                size_t *count, int32_t *start, int32_t *end) {
    if (!source || !zones || !count || !start || !end || length > INT32_MAX - 8192)
        return 0;
    *zones = NULL;
    *count = 0;
    *start = *end = 0;
    const uint16_t opcodes[4] = {0x0767, 0x076c, 0x076a, 0x0874};
    size_t first = SIZE_MAX, last = 0;
    for (size_t i = 0; i + 2 < length; ++i)
        if (source[i] == (uint8_t)opcodes[0] &&
            source[i + 1] == opcodes[0] >> 8 && source[i + 2] == 9) {
            if (first == SIZE_MAX) first = last = i;
            else if (i - last <= 4096) last = i;
            else break;
        }
    if (first == SIZE_MAX) return 1;
    *start = (int32_t)first - 8192;
    *end = (int32_t)last + 8192;
    entry *items = NULL;
    size_t used = 0, capacity = 0;
    for (unsigned opcode = 0; opcode < 4; ++opcode) {
        for (size_t i = 0; i + 2 < length; ++i) {
            if (source[i] != (uint8_t)opcodes[opcode] ||
                source[i + 1] != opcodes[opcode] >> 8 || source[i + 2] != 9 ||
                (int32_t)i < *start || (int32_t)i > *end) continue;
            char name[9] = {0};
            label_at(source, length, i, name);
            int32_t a, b;
            size_t next;
            if (!read_value(source, length, i + 11, &a, &next))
                continue;
            if (opcode == 1 && (!read_value(source, length, next, &b, &next) ||
                                a < 0 || a >= 10))
                continue;
            entry *z = zone_for(&items, &used, &capacity, name);
            if (!z) {
                free(items);
                return 0;
            }
            if (opcode == 0 && !z->zone.has_population) {
                z->zone.population = a;
                z->zone.has_population = 1;
            } else if (opcode == 1 && !(z->seen & (1u << a))) {
                z->seen |= (uint16_t)(1u << a);
                z->zone.gang[a] = b < 0 ? 0 : b > 255 ? 255 : (uint8_t)b;
            } else if (opcode == 2)
                z->zone.dealer = a < 0 ? 0 : a > 255 ? 255 : (uint8_t)a;
            else if (opcode == 3 && z->zone.races < 0)
                z->zone.races = a & 15;
        }
    }
    pedscm_zone *result = used ? malloc(used * sizeof *result) : NULL;
    if (used && !result) {
        free(items);
        return 0;
    }
    for (size_t i = 0; i < used; ++i)
        result[i] = items[i].zone;
    free(items);
    *zones = result;
    *count = used;
    return 1;
}
