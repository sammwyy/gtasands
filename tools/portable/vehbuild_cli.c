#include "gamefiles.h"
#include "vehbuild.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: vehbuild_cli GTA_FOLDER OUTPUT.DVEH\n");
        return 2;
    }
    gamefiles source;
    if (!gamefiles_init(&source, argv[1])) {
        fprintf(stderr, "invalid GTA folder\n");
        return 1;
    }
    vehbuild_options options = {&source, gamefiles_read_file};
    vehbuild_stats stats;
    uint8_t *data = NULL;
    size_t size = 0;
    if (!vehbuild_convert(&options, &data, &size, &stats)) {
        fprintf(stderr, "failed to convert vehicles\n");
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
    printf("%zu bytes, %u vehicles, %u colours, %u surfaces, %u water, %u track nodes\n",
           size, stats.vehicles, stats.colours, stats.surfaces, stats.water,
           stats.track_nodes);
    return 0;
}
