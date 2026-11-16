#include "enexbuild.h"
#include "enex.h"
#include "worldplan.h"

#include <stdlib.h>
#include <string.h>

int enexbuild_convert(const enexbuild_options *options, uint8_t **output,
                      size_t *length, enexbuild_stats *stats) {
    if (!options || !options->file || !output || !length || !stats) return 0;
    *output = NULL;
    *length = 0;
    memset(stats, 0, sizeof *stats);
    uint8_t *dat = NULL;
    size_t dat_size = 0;
    worldplan_file *paths = NULL;
    size_t path_count = 0;
    int ok = options->file(options->context, "data/gta.dat", &dat, &dat_size) &&
             dat && worldplan_parse_dat((const char *)dat, dat_size,
                                         &paths, &path_count);
    free(dat);
    enex_entry *entries = NULL;
    size_t count = 0;
    for (size_t i = 0; i < path_count && ok; ++i) {
        if (paths[i].kind != 1) continue;
        uint8_t *text = NULL;
        size_t size = 0;
        if (!options->file(options->context, paths[i].path, &text, &size))
            continue;
        ipl_instance *placed = NULL;
        enex_entry *found = NULL;
        size_t nplaced = 0, nfound = 0;
        ok = ipl_parse_text((const char *)text, size, &placed, &nplaced) &&
             enex_parse((const char *)text, size, placed, nplaced,
                        &found, &nfound);
        if (ok && nfound) {
            if (count > SIZE_MAX / sizeof *entries - nfound) ok = 0;
            else {
                enex_entry *next = realloc(entries, (count + nfound) * sizeof *next);
                if (!next) ok = 0;
                else {
                    entries = next;
                    memcpy(entries + count, found, nfound * sizeof *found);
                    count += nfound;
                }
            }
        }
        free(text);
        free(placed);
        free(found);
    }
    if (ok) ok = enex_pack(entries, count, output, length,
                           &stats->exterior, &stats->linked);
    if (ok) stats->entries = (uint32_t)count;
    free(paths);
    free(entries);
    return ok;
}
