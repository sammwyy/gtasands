#include "img.h"

#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *data) {
    return (uint16_t)(data[0] | data[1] << 8);
}

static uint32_t u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

int img_index_size(const uint8_t *header, size_t length, size_t *required) {
    if (!header || !required || length < 8 || memcmp(header, "VER2", 4)) return 0;
    size_t n = u32(header + 4);
    if (n > (SIZE_MAX - 8) / 32) return 0;
    *required = 8 + (size_t)n * 32;
    return 1;
}

int img_parse_index(const uint8_t *index, size_t length, img_entry **entries, size_t *count) {
    size_t needed;
    if (!entries || !count || !img_index_size(index, length, &needed) || needed > length)
        return 0;
    size_t n = (needed - 8) / 32;
    img_entry *out = n ? calloc(n, sizeof *out) : NULL;
    if (n && !out) return 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *record = index + 8 + i * 32;
        memcpy(out[i].name, record + 8, 24);
        out[i].name[24] = 0;
        out[i].offset = (uint64_t)u32(record) * 2048;
        uint16_t stream = u16(record + 4), archive = u16(record + 6);
        out[i].size = (uint32_t)(archive ? archive : stream) * 2048;
    }
    *entries = out;
    *count = n;
    return 1;
}

void img_free(void *entries) {
    free(entries);
}
