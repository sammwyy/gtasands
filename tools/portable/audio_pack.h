#ifndef GTASADS_AUDIO_PACK_H
#define GTASADS_AUDIO_PACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t bank;
    const uint8_t *data;
    size_t size;
} dsfx_entry;

typedef struct {
    uint8_t pack;
    uint16_t index;
} dsbl_entry;

int dsfx_pack(const dsfx_entry *entries, size_t count,
              uint8_t **output, size_t *length);
int dsbl_pack(const char *const *names, size_t pack_count,
              const dsbl_entry *lookups, size_t bank_count,
              uint8_t **output, size_t *length);

#endif
