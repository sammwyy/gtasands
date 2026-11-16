#include "enex.h"

#include <stdio.h>
#include <stdlib.h>

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *bytes = malloc((size_t)size + 1);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    int ok = fread(bytes, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!ok) {
        free(bytes);
        return NULL;
    }
    *length = (size_t)size;
    return bytes;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: enex_cli OUTPUT.DENX IPL...\n");
        return 2;
    }
    enex_entry *entries = NULL;
    size_t used = 0;
    int ok = 1;
    for (int i = 2; i < argc && ok; ++i) {
        size_t length = 0, count = 0, placed_count = 0;
        char *source = read_file(argv[i], &length);
        ipl_instance *placed = NULL;
        enex_entry *found = NULL;
        if (source)
            ok = ipl_parse_text(source, length, &placed, &placed_count) &&
                 enex_parse(source, length, placed, placed_count, &found, &count);
        else ok = 0;
        if (ok && count) {
            if (used > SIZE_MAX / sizeof *entries - count) ok = 0;
            else {
                enex_entry *next = realloc(entries, (used + count) * sizeof *next);
                if (!next) ok = 0;
                else {
                    entries = next;
                    for (size_t j = 0; j < count; ++j)
                        entries[used + j] = found[j];
                    used += count;
                }
            }
        }
        free(source);
        free(placed);
        free(found);
    }
    uint8_t *output = NULL;
    size_t length = 0;
    uint32_t exterior, linked;
    if (ok) ok = enex_pack(entries, used, &output, &length, &exterior, &linked);
    if (ok) {
        FILE *file = fopen(argv[1], "wbx");
        ok = file != NULL;
        if (ok) {
            ok = fwrite(output, 1, length, file) == length;
            if (fclose(file)) ok = 0;
            if (!ok) remove(argv[1]);
        }
    }
    free(output);
    free(entries);
    if (!ok) fprintf(stderr, "failed to convert IPL entrances\n");
    return ok ? 0 : 1;
}
