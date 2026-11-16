#ifndef GTASADS_AUDIO_TABLES_H
#define GTASADS_AUDIO_TABLES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[53];
} audio_table_name;

typedef struct {
    uint32_t pack, offset, size;
} audio_table_lookup;

int audio_table_names(const uint8_t *input, size_t length, size_t width,
                       audio_table_name **names, size_t *count);
int audio_table_entries(const uint8_t *input, size_t length,
                         audio_table_lookup **entries, size_t *count);

#endif
