#include "ogg_decode.h"

#include <limits.h>
#include <stdlib.h>

#include "vendor/stb_vorbis.c"

int ogg_decode_vorbis(const uint8_t *source, size_t length,
                      int16_t **pcm, size_t *samples,
                      uint32_t *sample_rate, uint16_t *channels) {
    if (!source || length > INT_MAX || !pcm || !samples ||
        !sample_rate || !channels) return 0;
    int rate = 0, count = 0;
    short *decoded = NULL;
    int frames = stb_vorbis_decode_memory((const unsigned char *)source,
                                          (int)length, &count, &rate, &decoded);
    if (frames < 0 || count < 1 || count > 8 || rate < 1 ||
        (size_t)frames > SIZE_MAX / ((size_t)count * sizeof *decoded)) {
        free(decoded);
        return 0;
    }
    *pcm = decoded;
    *samples = (size_t)frames;
    *sample_rate = (uint32_t)rate;
    *channels = (uint16_t)count;
    return 1;
}
