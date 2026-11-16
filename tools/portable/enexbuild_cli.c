#include "enexbuild.h"
#include "gamefiles.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: enexbuild_cli GTA_FOLDER OUTPUT.DENX\n");
        return 2;
    }
    gamefiles source;
    if (!gamefiles_init(&source, argv[1])) {
        fprintf(stderr, "invalid GTA folder\n");
        return 1;
    }
    enexbuild_options options = {&source, gamefiles_read_file};
    enexbuild_stats stats;
    uint8_t *data = NULL;
    size_t size = 0;
    if (!enexbuild_convert(&options, &data, &size, &stats)) {
        fprintf(stderr, "failed to convert entrances\n");
        return 1;
    }
    FILE *out = fopen(argv[2], "wbx");
    int ok = out != NULL;
    if (ok) {
        ok = fwrite(data, 1, size, out) == size;
        if (fclose(out)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(data);
    if (!ok) {
        fprintf(stderr, "failed to write %s\n", argv[2]);
        return 1;
    }
    printf("%zu bytes, %u entrances, %u exterior, %u linked\n",
           size, stats.entries, stats.exterior, stats.linked);
    return 0;
}
