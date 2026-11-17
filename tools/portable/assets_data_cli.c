#include "enexbuild.h"
#include "gamefiles.h"
#include "pedbuild.h"
#include "points.h"
#include "vehbuild.h"
#include "worldbuild.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const char *name;
    const char *path;
    uint8_t *bytes;
    size_t length;
    int selected;
} asset;

static int number(const char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*end;
}

static int write_new(const char *directory, const asset *entry) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof path, "%s/%s", directory, entry->path);
    if (length < 0 || (size_t)length >= sizeof path) return 0;
    FILE *out = fopen(path, "wbx");
    if (!out) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return 0;
    }
    int ok = fwrite(entry->bytes, 1, entry->length, out) == entry->length;
    if (fclose(out)) ok = 0;
    if (!ok) {
        remove(path);
        fprintf(stderr, "cannot write %s\n", path);
    } else printf("%s: %zu bytes\n", entry->path, entry->length);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: assets_data_cli GTA_FOLDER OUTPUT_DIR [--bounds X0 Y0 X1 Y1] [--sector N] [--no-lods] [world|peds|vehicles|entry_exits|map_points ...]\n");
        return 2;
    }
    asset assets[] = {
        {"world", "world.dwld", NULL, 0, 0},
        {"peds", "peds.dped", NULL, 0, 0},
        {"vehicles", "vehicles.dveh", NULL, 0, 0},
        {"entry_exits", "entry_exits.denx", NULL, 0, 0},
        {"map_points", "map_points.dpoi", NULL, 0, 0}
    };
    const size_t count = sizeof assets / sizeof *assets;
    gamefiles files;
    if (!gamefiles_init(&files, argv[1])) return 2;
    worldbuild_options world = {
        .context = &files, .file = gamefiles_read_file,
        .range = gamefiles_read_range, .sector = 50, .keep_lods = 1
    };
    pedbuild_options peds = {
        .context = &files, .file = gamefiles_read_file,
        .bounds = {-3000, -3000, 3000, 3000}
    };
    int selected = 0;
    for (int i = 3; i < argc;) {
        if (!strcmp(argv[i], "--bounds") && i + 4 < argc) {
            world.use_bounds = 1;
            for (int j = 0; j < 4; ++j)
                if (!number(argv[i + j + 1], &world.bounds[j])) return 2;
            memcpy(peds.bounds, world.bounds, sizeof peds.bounds);
            i += 5;
        } else if (!strcmp(argv[i], "--sector") && i + 1 < argc) {
            char *end;
            unsigned long sector = strtoul(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end || sector < 1 || sector > UINT16_MAX) return 2;
            world.sector = (uint16_t)sector;
            i += 2;
        } else if (!strcmp(argv[i], "--no-lods")) {
            world.keep_lods = 0;
            ++i;
        } else {
            size_t j = 0;
            while (j < count && strcmp(argv[i], assets[j].name)) ++j;
            if (j == count || assets[j].selected) {
                fprintf(stderr, "unknown or duplicate section: %s\n", argv[i]);
                return 2;
            }
            assets[j].selected = 1;
            ++selected;
            ++i;
        }
    }
    if (!selected)
        for (size_t i = 0; i < count; ++i) assets[i].selected = 1;
    struct stat output;
    if (stat(argv[2], &output) || !S_ISDIR(output.st_mode)) {
        fprintf(stderr, "output directory does not exist: %s\n", argv[2]);
        return 1;
    }
    int ok = 1;
    if (assets[0].selected) {
        worldbuild_stats stats = {0};
        ok = gamefiles_init_archives(&files) &&
             worldbuild_convert(&world, &assets[0].bytes, &assets[0].length, &stats);
    }
    if (ok && assets[1].selected) {
        pedbuild_stats stats = {0};
        ok = pedbuild_convert(&peds, &assets[1].bytes, &assets[1].length, &stats);
    }
    if (ok && assets[2].selected) {
        vehbuild_options options = {&files, gamefiles_read_file};
        vehbuild_stats stats = {0};
        ok = vehbuild_convert(&options, &assets[2].bytes, &assets[2].length, &stats);
    }
    if (ok && (assets[3].selected || assets[4].selected)) {
        enexbuild_options options = {&files, gamefiles_read_file};
        enexbuild_stats stats = {0};
        ok = enexbuild_convert(&options, &assets[3].bytes, &assets[3].length, &stats);
        if (ok && assets[4].selected)
            ok = points_from_denx(assets[3].bytes, assets[3].length,
                                  &assets[4].bytes, &assets[4].length);
    }
    if (!ok) fprintf(stderr, "failed to convert data assets\n");
    if (ok) {
        char dir[PATH_MAX];
        int n = snprintf(dir, sizeof dir, "%s/data", argv[2]);
        if (n < 0 || (size_t)n >= sizeof dir) ok = 0;
        else if (mkdir(dir, 0777) && errno != EEXIST) ok = 0;
        else if (stat(dir, &output) || !S_ISDIR(output.st_mode)) ok = 0;
        if (!ok) fprintf(stderr, "cannot create data directory\n");
        for (size_t i = 0; ok && i < count; ++i) {
            if (!assets[i].selected) continue;
            char path[PATH_MAX];
            n = snprintf(path, sizeof path, "%s/%s", dir, assets[i].path);
            if (n < 0 || (size_t)n >= sizeof path) ok = 0;
            else if (!stat(path, &output)) {
                fprintf(stderr, "output already exists: %s\n", path);
                ok = 0;
            } else if (errno != ENOENT) ok = 0;
        }
        for (size_t i = 0; ok && i < count; ++i)
            if (assets[i].selected)
                ok = write_new(dir, &assets[i]);
    }
    for (size_t i = 0; i < count; ++i) free(assets[i].bytes);
    return ok ? 0 : 1;
}
