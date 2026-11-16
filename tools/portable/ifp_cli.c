#include "ifp.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "usage: ifp_cli INPUT.ifp OUTPUT.danm [KEEP_EVERY [MAX_FRAMES [DROP_TRANSLATION]]]\n");
        return 2;
    }
    unsigned keep = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1;
    unsigned maximum = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 0;
    unsigned drop = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 0;
    if (!keep || drop > 1) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END) || ftell(input) < 0) {
        if (input) fclose(input);
        return 1;
    }
    size_t size = (size_t)ftell(input);
    uint8_t *raw = malloc(size ? size : 1);
    if (!raw) {
        fclose(input);
        return 1;
    }
    rewind(input);
    int ok = fread(raw, 1, size, input) == size;
    fclose(input);
    uint8_t *converted = NULL;
    size_t length = 0;
    uint32_t before, after;
    ifp_options opts = {keep, maximum, (uint8_t)drop, NULL, 0, NULL, 0};
    if (ok) ok = ifp_convert(raw, size, &opts, &converted, &length, &before, &after);
    FILE *output = ok ? fopen(argv[2], "wb") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(converted, 1, length, output) == length;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(raw);
    free(converted);
    if (!ok) fprintf(stderr, "failed to convert IFP\n");
    return ok ? 0 : 1;
}
