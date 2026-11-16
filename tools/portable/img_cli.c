#include "img.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: img_cli ARCHIVE.img\n");
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 1;
    uint8_t header[8];
    size_t required = 0;
    int ok = fread(header, 1, 8, file) == 8 &&
             img_index_size(header, sizeof header, &required);
    if (ok) {
        ok = fseek(file, 0, SEEK_END) == 0 && ftell(file) >= 0 &&
             required <= (size_t)ftell(file);
        rewind(file);
        if (ok) ok = fseek(file, 8, SEEK_SET) == 0;
    }
    uint8_t *index = ok ? malloc(required) : NULL;
    if (!index) ok = 0;
    if (ok) {
        for (size_t i = 0; i < 8; ++i) index[i] = header[i];
        ok = fread(index + 8, 1, required - 8, file) == required - 8;
    }
    fclose(file);
    img_entry *entries = NULL;
    size_t count = 0;
    if (ok) ok = img_parse_index(index, required, &entries, &count);
    if (ok)
        for (size_t i = 0; i < count; ++i)
            printf("%s\t%llu\t%u\n", entries[i].name,
                   (unsigned long long)entries[i].offset, entries[i].size);
    free(entries);
    free(index);
    if (!ok) fprintf(stderr, "invalid IMG v2 index\n");
    return ok ? 0 : 1;
}
