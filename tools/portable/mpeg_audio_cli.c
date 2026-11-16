#include "mpeg_audio.h"
#include "resample.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: mpeg_audio_cli INPUT.MPG OUTPUT.PCM [RATE]\n");
        return 2;
    }
    uint32_t target = 0;
    if (argc == 4) {
        char *end;
        errno = 0;
        unsigned long parsed = strtoul(argv[3], &end, 10);
        if (errno || end == argv[3] || *end || !parsed || parsed > UINT16_MAX)
            return 2;
        target = (uint32_t)parsed;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) {
        if (input) fclose(input);
        return 1;
    }
    long length = ftell(input);
    if (length <= 0) {
        fclose(input);
        return 1;
    }
    rewind(input);
    uint8_t *data = malloc((size_t)length);
    if (!data) {
        fclose(input);
        return 1;
    }
    int ok = fread(data, 1, (size_t)length, input) == (size_t)length;
    fclose(input);
    mpeg_audio *audio = ok ? mpeg_audio_open(data, (size_t)length) : NULL;
    free(data);
    int16_t *samples = NULL;
    size_t count = 0, capacity = 0;
    uint32_t source_rate = 0;
    while (audio && ok) {
        int16_t *frame = NULL;
        size_t n = 0;
        uint32_t rate = 0;
        double time = 0;
        if (!mpeg_audio_next(audio, &frame, &n, &rate, &time)) break;
        if (source_rate && source_rate != rate) ok = 0;
        source_rate = rate;
        if (n > SIZE_MAX - count || count + n > SIZE_MAX / sizeof *samples)
            ok = 0;
        if (ok && count + n > capacity) {
            size_t needed = count + n;
            size_t next = capacity > (SIZE_MAX / sizeof *samples) / 2 ?
                          needed : capacity * 2;
            if (next < needed) next = needed;
            int16_t *more = realloc(samples, next * sizeof *samples);
            if (more) {
                samples = more;
                capacity = next;
            } else ok = 0;
        }
        if (ok) {
            for (size_t i = 0; i < n; ++i) samples[count + i] = frame[i];
            count += n;
        }
        free(frame);
    }
    mpeg_audio_close(audio);
    if (ok && count && target && target < source_rate) {
        int16_t *downsampled = NULL;
        size_t next = 0;
        ok = resample_pcm16(samples, count, source_rate, target,
                            &downsampled, &next);
        if (ok) {
            free(samples);
            samples = downsampled;
            count = next;
            source_rate = target;
        }
    }
    FILE *output = ok && count ? fopen(argv[2], "wbx") : NULL;
    if (!output) ok = 0;
    else {
        for (size_t i = 0; i < count && ok; ++i) {
            uint8_t value[2] = {(uint8_t)samples[i],
                                (uint8_t)((uint16_t)samples[i] >> 8)};
            ok = fwrite(value, 1, sizeof value, output) == sizeof value;
        }
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(samples);
    if (ok) printf("%zu samples at %u Hz\n", count, source_rate);
    else fprintf(stderr, "failed to decode MPEG audio\n");
    return ok ? 0 : 1;
}
