#include "dspk.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int read_file(void *context, void *source, uint32_t offset, void *data, size_t size) {
    (void)context;
    FILE *file = source;
    return fseek(file, (long)offset, SEEK_SET) == 0 && fread(data, 1, size, file) == size;
}

static int write_file(void *context, const void *data, size_t size) {
    return fwrite(data, 1, size, context) == size;
}

int main(int argc, char **argv) {
    if (argc < 2 || (argc - 2) % 2) {
        fprintf(stderr, "usage: dspk_cli OUTPUT [NAME FILE]...\n");
        return 2;
    }
    size_t count = (size_t)(argc - 2) / 2;
    dspk_entry *entries = calloc(count ? count : 1, sizeof *entries);
    if (!entries)
        return 1;
    int ok = 1;
    for (size_t i = 0; i < count; ++i) {
        FILE *file = fopen(argv[3 + i * 2], "rb");
        if (!file || fseek(file, 0, SEEK_END) || ftell(file) < 0 ||
            (uint64_t)ftell(file) > UINT32_MAX) {
            if (file)
                fclose(file);
            ok = 0;
            break;
        }
        entries[i] = (dspk_entry){argv[2 + i * 2], (uint32_t)ftell(file), file};
    }
    FILE *out = ok ? fopen(argv[1], "wb") : NULL;
    if (!out)
        ok = 0;
    else {
        ok = dspk_write(entries, count, read_file, write_file, out);
        if (fclose(out))
            ok = 0;
        if (!ok)
            remove(argv[1]);
    }
    for (size_t i = 0; i < count; ++i)
        if (entries[i].source)
            fclose(entries[i].source);
    free(entries);
    if (!ok)
        fprintf(stderr, "failed to build DSPK\n");
    return ok ? 0 : 1;
}
