#ifndef GTASADS_DSND_H
#define GTASADS_DSND_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t rate, sample_rate;
    uint8_t format, checkpoints;
    double volume, max_seconds;
    int32_t loop;
    int16_t headroom;
} dsnd_options;

int dsnd_encode(const int16_t *pcm, size_t count, const dsnd_options *options,
                uint8_t **output, size_t *length, uint8_t **checkpoints,
                size_t *checkpoint_length);
int dsnd_encode_simple(const int16_t *pcm, size_t count,
                       uint32_t rate, uint32_t sample_rate,
                       uint8_t format, int checkpoints,
                       double volume, double max_seconds,
                       int32_t loop, int16_t headroom,
                       uint8_t **output, size_t *length);
int dsbk_convert(const uint8_t *raw, size_t length, const dsnd_options *options,
                 uint8_t **output, size_t *output_length, uint16_t *sounds);

typedef struct {
    uint32_t track;
    const uint8_t *data;
    size_t length;
} dsst_entry;

int dsst_pack(const dsst_entry *entries, size_t count, int seekable,
              uint8_t **output, size_t *output_length);
void audio_decrypt(const uint8_t *source, size_t length, uint64_t offset, uint8_t *output);
void audio_decrypt_parts(const uint8_t *source, size_t length,
                         uint32_t offset_low, uint32_t offset_high,
                         uint8_t *output);
int audio_aligned_offset(const uint8_t *encrypted, size_t length, uint64_t start,
                         uint64_t expected, uint64_t *result);

#endif
