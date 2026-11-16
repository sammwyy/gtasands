#include "texture.h"
#include "txd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t target_size(uint16_t source, double scale, unsigned maximum) {
    unsigned n = (unsigned)(source * scale);
    unsigned power = 1;
    while (power <= n / 2 && power < 1024) power *= 2;
    if (power > maximum) power = maximum;
    if (power < 8) power = 8;
    return (uint16_t)power;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "usage: texture_cli INPUT.txd OUTPUT.dtxd [SCALE [MAX_SIZE [FORMAT]]]\n");
        return 2;
    }
    double scale = argc > 3 ? strtod(argv[3], NULL) : 0.5;
    unsigned maximum = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 128;
    const char *formats[] = {"auto", "a3i5", "pal4", "pal16", "pal256", NULL, "a5i3", "direct"};
    int format = 0;
    if (argc > 5)
        while (format < 8 && (!formats[format] || strcmp(formats[format], argv[5])))
            ++format;
    if (scale <= 0 || scale > 16 || maximum < 8 || maximum > 1024 || format == 8)
        return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END) || ftell(input) < 0) {
        if (input) fclose(input);
        return 1;
    }
    size_t size = (size_t)ftell(input);
    uint8_t *data = malloc(size ? size : 1);
    if (!data) {
        fclose(input);
        return 1;
    }
    rewind(input);
    int ok = fread(data, 1, size, input) == size;
    fclose(input);
    size_t count = 0;
    txd_texture texture;
    while (ok && txd_get(data, size, count, &texture)) ++count;
    texture_target *targets = count ? calloc(count, sizeof *targets) : NULL;
    if (count && !targets) ok = 0;
    for (size_t i = 0; i < count && ok; ++i) {
        txd_get(data, size, i, &texture);
        targets[i] = (texture_target){
            target_size(texture.width, scale, maximum),
            target_size(texture.height, scale, maximum),
            {format, 24, 16, 0, 0}
        };
    }
    uint8_t *converted = NULL;
    size_t length = 0;
    if (ok) ok = texture_convert_txd(data, size, targets, count, &converted, &length);
    FILE *output = ok ? fopen(argv[2], "wb") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(converted, 1, length, output) == length;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(data);
    free(targets);
    free(converted);
    if (!ok) fprintf(stderr, "failed to convert TXD\n");
    return ok ? 0 : 1;
}
