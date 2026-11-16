#include "audio_stream.h"

#include "dsnd.h"
#include "ogg_decode.h"
#include "ogg_info.h"

#include <stdlib.h>

enum { TRACK_HEADER = 0x1f84 };

int audio_stream_convert(const uint8_t *encrypted, size_t length,
                         uint32_t offset_low, uint32_t offset_high,
                         uint32_t target_rate, uint8_t format,
                         int seekable, double volume, double max_seconds,
                         uint8_t **output, size_t *output_length) {
    if (!encrypted || length <= TRACK_HEADER || !output || !output_length)
        return 0;
    uint8_t *plain = malloc(length);
    if (!plain) return 0;
    audio_decrypt(encrypted, length,
                  (uint64_t)offset_high << 32 | offset_low, plain);
    const uint8_t *ogg = plain + TRACK_HEADER;
    size_t ogg_length = length - TRACK_HEADER;
    ogg_vorbis_info info;
    int16_t *pcm = NULL;
    size_t samples = 0;
    uint32_t source_rate = 0;
    uint16_t channels = 0;
    int ok = ogg_read_vorbis_info(ogg, ogg_length, &info) &&
             ogg_decode_vorbis(ogg, ogg_length, &pcm, &samples,
                               &source_rate, &channels) &&
             info.sample_rate == source_rate && info.channels == channels &&
             channels <= 2;
    free(plain);
    if (!ok) {
        free(pcm);
        return 0;
    }
    if (info.samples < samples) samples = (size_t)info.samples;
    int16_t *mono = NULL;
    if (channels == 2) {
        mono = malloc(samples * sizeof *mono);
        if (!mono) {
            free(pcm);
            return 0;
        }
        for (size_t i = 0; i < samples; ++i) {
            int32_t sum = (int32_t)pcm[i * 2] + pcm[i * 2 + 1];
            int32_t half = sum / 2;
            if (sum % 2 && half % 2) half += sum > 0 ? 1 : -1;
            mono[i] = (int16_t)half;
        }
    }
    ok = dsnd_encode_simple(mono ? mono : pcm, samples, source_rate,
                            target_rate, format, seekable, volume,
                            max_seconds, -1, 0, output, output_length);
    free(mono);
    free(pcm);
    return ok;
}
