#include "dff.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: dff_radius_cli INPUT.dff\n");
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        if (file) fclose(file);
        return 1;
    }
    size_t size = (size_t)ftell(file);
    uint8_t *data = malloc(size ? size : 1);
    if (!data) {
        fclose(file);
        return 1;
    }
    rewind(file);
    int ok = fread(data, 1, size, file) == size;
    fclose(file);
    uint16_t radius = 0;
    if (ok) ok = dff_radius(data, size, &radius);
    if (ok) printf("%u\n", radius);
    else fprintf(stderr, "failed to read DFF\n");
    free(data);
    return ok ? 0 : 1;
}
