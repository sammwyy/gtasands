#include "dmdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 10) {
        fprintf(stderr, "usage: dmdl_cli INPUT.dff OUTPUT.dmdl [RATIO [MIN_TRIS [MAX_TRIS [cluster|qem|wedge [MIN_AREA [MAX_SHIFT [GROUND_QEM]]]]]]]\n");
        return 2;
    }
    dmdl_options opts = {0};
    opts.ratio = argc > 3 ? strtod(argv[3], NULL) : 1.0;
    opts.min_tris = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 64;
    opts.max_tris = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0;
    opts.decimate = argc > 6 && !strcmp(argv[6], "qem") ? 1 :
                    argc > 6 && !strcmp(argv[6], "wedge") ? 2 : 0;
    opts.min_area = argc > 7 ? strtod(argv[7], NULL) : 0;
    opts.max_shift = argc > 8 ? strtod(argv[8], NULL) : 0;
    opts.ground_qem = argc > 9 && atoi(argv[9]);
    opts.uvs = opts.colors = opts.skin = 1;
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
    if (ok) ok = dmdl_convert(raw, size, &opts, &converted, &length, &before, &after);
    FILE *output = ok ? fopen(argv[2], "wb") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(converted, 1, length, output) == length;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(raw);
    free(converted);
    if (!ok) fprintf(stderr, "failed to convert DFF\n");
    return ok ? 0 : 1;
}
