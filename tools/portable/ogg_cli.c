#include "ogg_decode.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: ogg_cli INPUT.OGG OUTPUT.PCM\n");
        return 2;
    }
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
    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(input);
        return 1;
    }
    int ok = fread(data, 1, (size_t)size, input) == (size_t)size;
    fclose(input);
    int16_t *pcm = NULL;
    size_t samples = 0;
    uint32_t rate = 0;
    uint16_t channels = 0;
    if (ok) ok = ogg_decode_vorbis(data, (size_t)size, &pcm, &samples,
                                    &rate, &channels);
    FILE *output = ok ? fopen(argv[2], "wbx") : NULL;
    if (!output) ok = 0;
    else {
        size_t bytes = samples * channels * sizeof *pcm;
        ok = fwrite(pcm, 1, bytes, output) == bytes;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    if (ok) printf("%u Hz, %u channels, %zu samples\n",
                   rate, channels, samples);
    else fprintf(stderr, "failed to decode OGG\n");
    free(pcm);
    free(data);
    return ok ? 0 : 1;
}
