#include "audio_tables.h"

#include <stdlib.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int audio_table_names(const uint8_t *input, size_t length, size_t width,
                       audio_table_name **names, size_t *count) {
    if (!input || !names || !count || !width || width > 52) return 0;
    *names = NULL;
    *count = 0;
    size_t n = length / width;
    if (n > SIZE_MAX / sizeof(audio_table_name)) return 0;
    audio_table_name *result = n ? calloc(n, sizeof *result) : NULL;
    if (n && !result) return 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *src = input + i * width;
        size_t used = 0;
        while (used < width && src[used]) ++used;
        memcpy(result[i].name, src, used);
    }
    *names = result;
    *count = n;
    return 1;
}

int audio_table_entries(const uint8_t *input, size_t length,
                         audio_table_lookup **entries, size_t *count) {
    if (!input || !entries || !count) return 0;
    *entries = NULL;
    *count = 0;
    size_t n = length / 12;
    if (n > SIZE_MAX / sizeof(audio_table_lookup)) return 0;
    audio_table_lookup *result = n ? malloc(n * sizeof *result) : NULL;
    if (n && !result) return 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *src = input + i * 12;
        result[i] = (audio_table_lookup){src[0], read32(src + 4), read32(src + 8)};
    }
    *entries = result;
    *count = n;
    return 1;
}
