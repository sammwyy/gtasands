#include "dtex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 6 || argc > 10) {
        fprintf(stderr, "usage: dtex_cli RGBA WIDTH HEIGHT FORMAT OUTPUT [THRESHOLD [OPAQUE [AUTO_COLORS [ADDITIVE]]]]\n");
        return 2;
    }
    const char *formats[] = {"auto", "a3i5", "pal4", "pal16", "pal256", NULL, "a5i3", "direct"};
    int format = 0;
    while (format < 8 && (!formats[format] || strcmp(formats[format], argv[4])))
        ++format;
    long width = strtol(argv[2], NULL, 10), height = strtol(argv[3], NULL, 10);
    long threshold = argc > 6 ? strtol(argv[6], NULL, 10) : 24;
    long opaque = argc > 7 ? strtol(argv[7], NULL, 10) : 0;
    long colors = argc > 8 ? strtol(argv[8], NULL, 10) : 16;
    long additive = argc > 9 ? strtol(argv[9], NULL, 10) : 0;
    if (format == 8 || width < 1 || width > 1024 || height < 1 || height > 1024 ||
        threshold < 0 || threshold > 255 || colors < 0 || colors > 256 ||
        (opaque != 0 && opaque != 1) || (additive != 0 && additive != 1))
        return 2;
    size_t size = (size_t)width * height * 4;
    uint8_t *rgba = malloc(size), *encoded = NULL;
    if (!rgba) return 1;
    FILE *input = fopen(argv[1], "rb");
    int ok = input && fread(rgba, 1, size, input) == size && fgetc(input) == EOF;
    if (input) fclose(input);
    size_t length = 0;
    if (ok) {
        dtex_options options = {(dtex_format)format, (uint8_t)threshold, (uint16_t)colors,
                                (uint8_t)opaque, (uint8_t)additive};
        ok = dtex_encode(rgba, (uint16_t)width, (uint16_t)height, options, &encoded, &length);
    }
    FILE *output = ok ? fopen(argv[5], "wb") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(encoded, 1, length, output) == length;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[5]);
    }
    free(rgba);
    free(encoded);
    if (!ok) fprintf(stderr, "failed to encode DTEX\n");
    return ok ? 0 : 1;
}
