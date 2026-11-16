#include "mpeg_audio.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/pl_mpeg.h"

struct mpeg_audio {
    plm_t *decoder;
    uint8_t *source;
};

mpeg_audio *mpeg_audio_open(const uint8_t *source, size_t length) {
    if (!source || !length) return NULL;
    mpeg_audio *audio = calloc(1, sizeof *audio);
    if (!audio) return NULL;
    audio->source = malloc(length);
    if (audio->source) {
        memcpy(audio->source, source, length);
        audio->decoder = plm_create_with_memory(audio->source, length, 0);
    }
    if (!audio->decoder) {
        mpeg_audio_close(audio);
        return NULL;
    }
    plm_set_video_enabled(audio->decoder, 0);
    return audio;
}

int mpeg_audio_next(mpeg_audio *audio, int16_t **samples, size_t *count,
                    uint32_t *rate, double *time) {
    if (!audio || !samples || !count || !rate || !time) return 0;
    *samples = NULL;
    *count = 0;
    plm_samples_t *frame = plm_decode_audio(audio->decoder);
    int sample_rate = plm_get_samplerate(audio->decoder);
    if (!frame || !sample_rate) return 0;
    int16_t *output = malloc((size_t)frame->count * sizeof *output);
    if (!output) return 0;
    for (size_t i = 0; i < frame->count; ++i) {
        float left = frame->interleaved[i * 2];
        float right = frame->interleaved[i * 2 + 1];
        float mono = (left + right) * 0.5f;
        float rounded = nearbyintf(mono * 32768.0f);
        if (rounded > 32767) rounded = 32767;
        if (rounded < -32768) rounded = -32768;
        output[i] = (int16_t)rounded;
    }
    *samples = output;
    *count = frame->count;
    *rate = (uint32_t)sample_rate;
    *time = frame->time;
    return 1;
}

void mpeg_audio_close(mpeg_audio *audio) {
    if (!audio) return;
    if (audio->decoder) plm_destroy(audio->decoder);
    free(audio->source);
    free(audio);
}
