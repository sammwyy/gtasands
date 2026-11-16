#include "pedbuild.h"
#include "gamefiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 7) {
        fprintf(stderr, "usage: pedbuild_cli GTA_FOLDER OUTPUT.DPED [X0 Y0 X1 Y1]\n");
        return 2;
    }
    gamefiles files;
    if (!gamefiles_init(&files, argv[1])) return 2;
    pedbuild_options options = {
        .context = &files,
        .file = gamefiles_read_file,
        .bounds = {-3000, -3000, 3000, 3000}
    };
    if (argc == 7)
        for (unsigned i = 0; i < 4; ++i) {
            char *end;
            options.bounds[i] = strtod(argv[3 + i], &end);
            if (end == argv[3 + i] || *end) return 2;
        }
    uint8_t *output = NULL;
    size_t length = 0;
    pedbuild_stats stats = {0};
    int ok = pedbuild_convert(&options, &output, &length, &stats);
    FILE *destination = ok ? fopen(argv[2], "wbx") : NULL;
    if (destination) {
        ok = fwrite(output, 1, length, destination) == length;
        if (fclose(destination)) ok = 0;
        if (!ok) remove(argv[2]);
    } else ok = 0;
    free(output);
    if (!ok) {
        fprintf(stderr, "failed to build DPED\n");
        return 1;
    }
    printf("%zu bytes, %u models, %u path nodes, %u road nodes\n",
           length, stats.models, stats.nodes, stats.road_nodes);
    return 0;
}
