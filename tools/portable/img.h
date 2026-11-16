#ifndef GTASADS_IMG_H
#define GTASADS_IMG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[25];
    uint64_t offset;
    uint32_t size;
} img_entry;

int img_index_size(const uint8_t *header, size_t length, size_t *required);
int img_parse_index(const uint8_t *index, size_t length, img_entry **entries, size_t *count);
void img_free(void *entries);

#endif
