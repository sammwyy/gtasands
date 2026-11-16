#ifndef GTASADS_GAMEFILES_H
#define GTASADS_GAMEFILES_H

#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char root[PATH_MAX];
    char archives[2][PATH_MAX];
} gamefiles;

int gamefiles_init(gamefiles *files, const char *root);
int gamefiles_init_archives(gamefiles *files);
int gamefiles_read_file(void *context, const char *path,
                        uint8_t **data, size_t *length);
int gamefiles_read_range(void *context, uint32_t pack, uint32_t offset_low,
                         uint32_t offset_high, uint32_t length,
                         uint8_t *destination);

#endif
