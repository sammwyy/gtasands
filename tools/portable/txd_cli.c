#include "txd.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: txd_cli INPUT INDEX LEVEL OUTPUT\n");
        return 2;
    }
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
    txd_texture texture;
    uint8_t *rgba = NULL;
    uint16_t w, h;
    if (ok) ok = txd_get(data, size, (size_t)strtoul(argv[2], NULL, 10), &texture);
    if (ok) ok = txd_decode(&texture, (size_t)strtoul(argv[3], NULL, 10), &rgba, &w, &h);
    FILE *output = ok ? fopen(argv[4], "wb") : NULL;
    if (!output) ok = 0;
    else {
        uint8_t header[4] = {(uint8_t)w, (uint8_t)(w >> 8),
                             (uint8_t)h, (uint8_t)(h >> 8)};
        ok = fwrite(header, 1, 4, output) == 4 &&
             fwrite(rgba, 4, (size_t)w * h, output) == (size_t)w * h;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[4]);
    }
    free(data);
    free(rgba);
    if (!ok) fprintf(stderr, "failed to decode TXD\n");
    return ok ? 0 : 1;
}
