#include "audio_pack.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint8_t *data = malloc(length ? (size_t)length : 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    int ok = fread(data, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) {
        free(data);
        return NULL;
    }
    *size = (size_t)length;
    return data;
}

static int compare(const void *left, const void *right) {
    const dsfx_entry *a = left, *b = right;
    return a->bank < b->bank ? -1 : a->bank > b->bank;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc % 2 != 0) {
        fprintf(stderr, "usage: audio_pack_cli OUTPUT.DSFX [BANK_ID INPUT.DSBK]...\n");
        return 2;
    }
    size_t count = (size_t)(argc - 2) / 2;
    dsfx_entry *entries = count ? calloc(count, sizeof *entries) : NULL;
    int ok = !count || entries != NULL;
    for (size_t i = 0; i < count && ok; ++i) {
        char *end;
        unsigned long bank = strtoul(argv[i * 2 + 2], &end, 10);
        if (end == argv[i * 2 + 2] || *end || bank > UINT16_MAX) {
            ok = 0;
            break;
        }
        entries[i].bank = (uint16_t)bank;
        entries[i].data = read_file(argv[i * 2 + 3], &entries[i].size);
        ok = entries[i].data != NULL;
    }
    if (ok && count) qsort(entries, count, sizeof *entries, compare);
    uint8_t *output = NULL;
    size_t length = 0;
    if (ok) ok = dsfx_pack(entries, count, &output, &length);
    FILE *file = ok ? fopen(argv[1], "wbx") : NULL;
    if (!file) ok = 0;
    else {
        ok = fwrite(output, 1, length, file) == length;
        if (fclose(file)) ok = 0;
        if (!ok) remove(argv[1]);
    }
    for (size_t i = 0; i < count; ++i) free((void *)entries[i].data);
    free(entries);
    free(output);
    if (!ok) fprintf(stderr, "failed to pack sound banks\n");
    return ok ? 0 : 1;
}
