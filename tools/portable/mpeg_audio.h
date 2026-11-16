#ifndef GTASADS_MPEG_AUDIO_H
#define GTASADS_MPEG_AUDIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct mpeg_audio mpeg_audio;

mpeg_audio *mpeg_audio_open(const uint8_t *source, size_t length);
int mpeg_audio_next(mpeg_audio *audio, int16_t **samples, size_t *count,
                    uint32_t *rate, double *time);
void mpeg_audio_close(mpeg_audio *audio);

#endif
