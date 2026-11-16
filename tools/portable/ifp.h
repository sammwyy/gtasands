#ifndef GTASADS_IFP_H
#define GTASADS_IFP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t keep_every;
    uint32_t max_frames;
    uint8_t drop_translation;
    const char *const *remove;
    size_t remove_count;
    const char *const *rigid_clips;
    size_t rigid_count;
} ifp_options;

int ifp_convert(const uint8_t *source, size_t size, const ifp_options *options,
                uint8_t **output, size_t *length, uint32_t *keys_in, uint32_t *keys_out);

#endif
