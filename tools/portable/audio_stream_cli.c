#include "audio_stream.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static int parse(const char *text, unsigned long long maximum,
                 unsigned long long *result) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *end || value > maximum) return 0;
    *result = value;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 7) {
        fprintf(stderr, "usage: audio_stream_cli INPUT.ENC OUTPUT.DSND OFFSET [RATE [FORMAT [SEEKABLE]]]\n");
        return 2;
    }
    unsigned long long offset, rate = 11025, format = 2, seekable = 0;
    if (!parse(argv[3], ULLONG_MAX, &offset) ||
        (argc >= 5 && (!parse(argv[4], UINT16_MAX, &rate) || !rate)) ||
        (argc >= 6 && !parse(argv[5], 2, &format)) ||
        (argc >= 7 && !parse(argv[6], 1, &seekable))) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) {
        if (input) fclose(input);
        return 1;
    }
    long size = ftell(input);
    if (size <= 0 || size > INT_MAX) {
        fclose(input);
        return 1;
    }
    rewind(input);
    uint8_t *encoded = malloc((size_t)size);
    if (!encoded) {
        fclose(input);
        return 1;
    }
    int ok = fread(encoded, 1, (size_t)size, input) == (size_t)size;
    fclose(input);
    uint8_t *sound = NULL;
    size_t sound_size = 0;
    if (ok) ok = audio_stream_convert(encoded, (size_t)size,
                                      (uint32_t)offset,
                                      (uint32_t)(offset >> 32),
                                      (uint32_t)rate, (uint8_t)format,
                                      (int)seekable, 1.0, 0,
                                      &sound, &sound_size);
    free(encoded);
    FILE *output = ok ? fopen(argv[2], "wbx") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(sound, 1, sound_size, output) == sound_size;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(sound);
    if (ok) printf("%zu bytes\n", sound_size);
    else fprintf(stderr, "failed to convert audio stream\n");
    return ok ? 0 : 1;
}
