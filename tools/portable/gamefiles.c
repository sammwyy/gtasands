#include "gamefiles.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same_name(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return !*a && !*b;
}

static int resolve(gamefiles *files, const char *relative, char *result) {
    if (!relative || !*relative || *relative == '/' || *relative == '\\' ||
        strlen(files->root) >= PATH_MAX) return 0;
    strcpy(result, files->root);
    const char *part = relative;
    while (*part) {
        while (*part == '/' || *part == '\\') ++part;
        if (!*part) break;
        const char *end = part;
        while (*end && *end != '/' && *end != '\\') ++end;
        char name[PATH_MAX];
        size_t length = (size_t)(end - part);
        if (!length || length >= sizeof name) return 0;
        memcpy(name, part, length);
        name[length] = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..") ||
            strchr(name, ':')) return 0;
        DIR *directory = opendir(result);
        if (!directory) return 0;
        struct dirent *entry;
        int matched = 0;
        while ((entry = readdir(directory))) {
            if (!same_name(entry->d_name, name)) continue;
            size_t base = strlen(result), next = strlen(entry->d_name);
            if (base + next + 2 < PATH_MAX) {
                result[base] = '/';
                memcpy(result + base + 1, entry->d_name, next + 1);
                matched = 1;
            }
            break;
        }
        closedir(directory);
        if (!matched) return 0;
        part = end;
    }
    return 1;
}

int gamefiles_init(gamefiles *files, const char *root) {
    if (!files || !root || strlen(root) >= sizeof files->root) return 0;
    memset(files, 0, sizeof *files);
    strcpy(files->root, root);
    return 1;
}

int gamefiles_init_archives(gamefiles *files) {
    return resolve(files, "models/gta3.img", files->archives[0]) &&
           resolve(files, "models/gta_int.img", files->archives[1]);
}

int gamefiles_read_file(void *context, const char *path, uint8_t **data,
                        size_t *length) {
    char name[PATH_MAX];
    if (!resolve(context, path, name)) return 0;
    FILE *input = fopen(name, "rb");
    if (!input) return 0;
    int ok = !fseek(input, 0, SEEK_END);
    long size = ok ? ftell(input) : -1;
    if (size < 0 || fseek(input, 0, SEEK_SET)) ok = 0;
    uint8_t *bytes = ok ? malloc(size ? (size_t)size : 1) : NULL;
    if (ok && !bytes) ok = 0;
    if (ok) ok = fread(bytes, 1, (size_t)size, input) == (size_t)size;
    fclose(input);
    if (!ok) {
        free(bytes);
        return 0;
    }
    *data = bytes;
    *length = (size_t)size;
    return 1;
}

int gamefiles_read_range(void *context, uint32_t pack, uint32_t offset_low,
                         uint32_t offset_high, uint32_t length,
                         uint8_t *destination) {
    gamefiles *files = context;
    uint64_t offset = (uint64_t)offset_high << 32 | offset_low;
    if (pack > 1 || offset > LONG_MAX || (length && !destination)) return 0;
    FILE *input = fopen(files->archives[pack], "rb");
    if (!input) return 0;
    int ok = !fseek(input, (long)offset, SEEK_SET) &&
             fread(destination, 1, length, input) == length;
    fclose(input);
    return ok;
}
