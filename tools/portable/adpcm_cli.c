#include "adpcm.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: adpcm_cli INPUT.pcm OUTPUT.adpcm [CHECKPOINTS.bin]\n");
        return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END) || ftell(input) < 0) {
        if (input) fclose(input);
        return 1;
    }
    size_t bytes = (size_t)ftell(input);
    if (bytes % 2) {
        fclose(input);
        return 1;
    }
    rewind(input);
    uint8_t *raw = malloc(bytes ? bytes : 1);
    int16_t *samples = malloc(bytes ? bytes : 1);
    int ok = raw && samples && fread(raw, 1, bytes, input) == bytes;
    fclose(input);
    if (ok)
        for (size_t i = 0; i < bytes / 2; ++i)
            samples[i] = (int16_t)(raw[i * 2] | (uint16_t)raw[i * 2 + 1] << 8);
    free(raw);
    uint8_t *encoded = NULL, *checks = NULL;
    size_t length = 0, checkpoint_length = 0;
    if (ok) ok = adpcm_encode(samples, bytes / 2, argc == 4,
                              &encoded, &length, &checks, &checkpoint_length);
    free(samples);
    FILE *output = ok ? fopen(argv[2], "wb") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(encoded, 1, length, output) == length;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    if (argc == 4 && ok) {
        FILE *checkpoint_file = fopen(argv[3], "wb");
        if (!checkpoint_file) ok = 0;
        else {
            ok = fwrite(checks, 1, checkpoint_length, checkpoint_file) == checkpoint_length;
            if (fclose(checkpoint_file)) ok = 0;
            if (!ok) remove(argv[3]);
        }
    }
    free(encoded);
    free(checks);
    if (!ok) fprintf(stderr, "failed to convert ADPCM\n");
    return ok ? 0 : 1;
}
