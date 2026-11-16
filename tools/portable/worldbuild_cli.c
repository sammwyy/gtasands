#include "worldbuild.h"
#include "gamefiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*end;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: worldbuild_cli GTA_FOLDER OUTPUT.DWLD [--bounds X0 Y0 X1 Y1] [--no-lods] [--sector N]\n");
        return 2;
    }
    gamefiles files = {0};
    if (!gamefiles_init(&files, argv[1]) ||
        !gamefiles_init_archives(&files)) {
        fprintf(stderr, "missing GTA IMG archives\n");
        return 1;
    }
    worldbuild_options options = {
        .context = &files, .file = gamefiles_read_file,
        .range = gamefiles_read_range,
        .sector = 50, .keep_lods = 1
    };
    for (int i = 3; i < argc;) {
        if (!strcmp(argv[i], "--bounds") && i + 4 < argc) {
            options.use_bounds = 1;
            for (int j = 0; j < 4; ++j)
                if (!number(argv[i + 1 + j], &options.bounds[j])) return 2;
            i += 5;
        } else if (!strcmp(argv[i], "--no-lods")) {
            options.keep_lods = 0;
            ++i;
        } else if (!strcmp(argv[i], "--sector") && i + 1 < argc) {
            char *end;
            unsigned long n = strtoul(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end || !n || n > UINT16_MAX) return 2;
            options.sector = (uint16_t)n;
            i += 2;
        } else return 2;
    }
    uint8_t *output = NULL;
    size_t length = 0;
    worldbuild_stats stats = {0};
    int ok = worldbuild_convert(&options, &output, &length, &stats);
    FILE *destination = ok ? fopen(argv[2], "wbx") : NULL;
    if (destination) {
        ok = fwrite(output, 1, length, destination) == length;
        if (fclose(destination)) ok = 0;
        if (!ok) remove(argv[2]);
    } else ok = 0;
    free(output);
    if (!ok) {
        fprintf(stderr, "failed to build DWLD\n");
        return 1;
    }
    printf("%zu bytes, %u models, %u instances, %u collisions\n",
           length, stats.models, stats.instances, stats.col_models);
    return 0;
}
