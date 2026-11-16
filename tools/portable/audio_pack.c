#include "audio_pack.h"

#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

int dsfx_pack(const dsfx_entry *entries, size_t count,
              uint8_t **output, size_t *length) {
    if (!output || !length || count > UINT16_MAX || (count && !entries) ||
        count > (UINT32_MAX - 8) / 12) return 0;
    size_t total = 8 + 12 * count;
    for (size_t i = 0; i < count; ++i) {
        if ((entries[i].size && !entries[i].data) ||
            entries[i].size > UINT32_MAX - total ||
            (i && entries[i - 1].bank > entries[i].bank))
            return 0;
        total += entries[i].size;
    }
    uint8_t *out = malloc(total);
    if (!out) return 0;
    memcpy(out, "DSFX", 4);
    put16(out + 4, 1);
    put16(out + 6, (uint16_t)count);
    size_t offset = 8 + count * 12;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *record = out + 8 + i * 12;
        put32(record, (uint32_t)offset);
        put32(record + 4, (uint32_t)entries[i].size);
        put16(record + 8, entries[i].bank);
        put16(record + 10, 0);
        if (entries[i].size) memcpy(out + offset, entries[i].data, entries[i].size);
        offset += entries[i].size;
    }
    *output = out;
    *length = total;
    return 1;
}

int dsbl_pack(const char *const *names, size_t pack_count,
              const dsbl_entry *lookups, size_t bank_count,
              uint8_t **output, size_t *length) {
    if (!output || !length || pack_count > UINT8_MAX || bank_count > UINT16_MAX ||
        (pack_count && !names) || (bank_count && !lookups)) return 0;
    size_t total = 12 + 16 * pack_count + 4 * bank_count;
    uint8_t *out = calloc(total, 1);
    if (!out) return 0;
    memcpy(out, "DSBL", 4);
    put16(out + 4, 1);
    out[6] = (uint8_t)pack_count;
    put16(out + 8, (uint16_t)bank_count);
    for (size_t i = 0; i < pack_count; ++i) {
        if (!names[i]) {
            free(out);
            return 0;
        }
        for (size_t j = 0; j < 15 && names[i][j]; ++j)
            out[12 + i * 16 + j] = (uint8_t)names[i][j];
    }
    for (size_t i = 0; i < bank_count; ++i) {
        uint8_t *record = out + 12 + pack_count * 16 + i * 4;
        record[0] = lookups[i].pack;
        put16(record + 2, lookups[i].index);
    }
    *output = out;
    *length = total;
    return 1;
}
