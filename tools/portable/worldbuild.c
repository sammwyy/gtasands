#include "worldbuild.h"

#include "dff.h"
#include "ide.h"
#include "img.h"
#include "ipl.h"
#include "worldcol.h"
#include "worldpack.h"
#include "worldplan.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    worldplan_object value;
    size_t order;
} object_record;

typedef struct {
    worldplan_parent value;
    size_t order;
} parent_record;

typedef struct {
    object_record *objects;
    size_t object_count;
    parent_record *parents;
    size_t parent_count;
    worldplan_file *files;
    size_t file_count;
    img_entry *main, *interior, *main_order, *interior_order;
    size_t main_count, interior_count;
    worldplan_instance *instances;
    size_t instance_count, instance_capacity;
    int32_t *ids;
    size_t id_count;
    worldplan_model_entry *models;
    worldplan_txd_entry *txds;
    size_t txd_count;
    uint16_t *cells;
    size_t *order;
    int16_t ox, oy;
    uint16_t width, height;
    worldpack_model *packed_models;
    worldpack_txd *packed_txds;
    worldpack_instance *packed_instances;
    worldpack_collision *collisions;
    uint8_t *output;
    size_t length;
    worldbuild_stats stats;
} build_state;

static void cleanup(build_state *state) {
    free(state->objects);
    free(state->parents);
    free(state->files);
    free(state->main);
    free(state->interior);
    free(state->main_order);
    free(state->interior_order);
    free(state->instances);
    free(state->ids);
    free(state->models);
    free(state->txds);
    free(state->cells);
    free(state->order);
    free(state->packed_models);
    free(state->packed_txds);
    free(state->packed_instances);
    if (state->collisions)
        for (size_t i = 0; i < state->id_count; ++i)
            free((void *)state->collisions[i].record);
    free(state->collisions);
    free(state->output);
}

static int compare_img(const void *a, const void *b) {
    return strcmp(((const img_entry *)a)->name,
                  ((const img_entry *)b)->name);
}

static int compare_objects(const void *a, const void *b) {
    const object_record *x = a, *y = b;
    if (x->value.id != y->value.id)
        return x->value.id < y->value.id ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static int compare_parents(const void *a, const void *b) {
    const parent_record *x = a, *y = b;
    int cmp = strcmp(x->value.name, y->value.name);
    if (cmp) return cmp;
    return x->order < y->order ? -1 : x->order > y->order;
}

static int compare_id(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y;
}

static int read_archive(const worldbuild_options *options, uint32_t pack,
                        uint64_t offset, uint32_t length,
                        uint8_t *destination) {
    return options->range(options->context, pack, (uint32_t)offset,
                          (uint32_t)(offset >> 32), length, destination);
}

static int read_index(const worldbuild_options *options, uint32_t pack,
                      img_entry **sorted, img_entry **original, size_t *count) {
    uint8_t header[8];
    size_t bytes = 0;
    if (!read_archive(options, pack, 0, sizeof header, header) ||
        !img_index_size(header, sizeof header, &bytes) ||
        bytes > UINT32_MAX) return 0;
    uint8_t *index = malloc(bytes);
    if (!index) return 0;
    int ok = read_archive(options, pack, 0, (uint32_t)bytes, index) &&
             img_parse_index(index, bytes, sorted, count);
    free(index);
    if (!ok) return 0;
    if (*count > SIZE_MAX / sizeof **sorted) return 0;
    *original = *count ? malloc(*count * sizeof **original) : NULL;
    if (*count && !*original) return 0;
    if (*count) memcpy(*original, *sorted, *count * sizeof **original);
    for (size_t i = 0; i < *count; ++i)
        for (size_t j = 0; j < 25 && (*sorted)[i].name[j]; ++j)
            (*sorted)[i].name[j] =
                (char)tolower((unsigned char)(*sorted)[i].name[j]);
    qsort(*sorted, *count, sizeof **sorted, compare_img);
    return 1;
}

static int load_file(const worldbuild_options *options, const char *path,
                     uint8_t **data, size_t *length) {
    *data = NULL;
    *length = 0;
    return options->file(options->context, path, data, length);
}

static int append_ide(build_state *state, const uint8_t *data, size_t size) {
    ide_object *models = NULL;
    ide_parent *parents = NULL;
    size_t model_count = 0, parent_count = 0;
    if (!ide_parse((const char *)data, size, &models, &model_count,
                   &parents, &parent_count)) return 0;
    int ok = model_count <= SIZE_MAX / sizeof *state->objects -
                            state->object_count &&
             parent_count <= SIZE_MAX / sizeof *state->parents -
                             state->parent_count;
    if (ok && model_count) {
        size_t total = state->object_count + model_count;
        object_record *more = realloc(state->objects, total * sizeof *more);
        if (more) state->objects = more;
        else ok = 0;
    }
    if (ok && parent_count) {
        size_t total = state->parent_count + parent_count;
        parent_record *more = realloc(state->parents, total * sizeof *more);
        if (more) state->parents = more;
        else ok = 0;
    }
    if (ok) {
        for (size_t i = 0; i < model_count; ++i) {
            size_t position = state->object_count + i;
            object_record *item = &state->objects[position];
            item->value = (worldplan_object){
                .id = models[i].id,
                .draw_distance = models[i].draw_distance,
                .flags = models[i].flags,
                .on = (uint8_t)models[i].on,
                .off = (uint8_t)models[i].off
            };
            memcpy(item->value.name, models[i].name, sizeof item->value.name);
            memcpy(item->value.txd, models[i].txd, sizeof item->value.txd);
            item->order = position;
        }
        for (size_t i = 0; i < parent_count; ++i) {
            size_t position = state->parent_count + i;
            state->parents[position].value = (worldplan_parent){0};
            memcpy(state->parents[position].value.name, parents[i].name,
                   sizeof parents[i].name);
            memcpy(state->parents[position].value.parent, parents[i].parent,
                   sizeof parents[i].parent);
            state->parents[position].order = position;
        }
        state->object_count += model_count;
        state->parent_count += parent_count;
    }
    free(models);
    free(parents);
    return ok;
}

static int load_ides(build_state *state, const worldbuild_options *options) {
    for (size_t i = 0; i <= state->file_count; ++i) {
        if (i && state->files[i - 1].kind != 0) continue;
        const char *name = i ? state->files[i - 1].path : "data/maps/txd.ide";
        uint8_t *data = NULL;
        size_t size = 0;
        if (!load_file(options, name, &data, &size)) continue;
        int ok = data && append_ide(state, data, size);
        free(data);
        if (!ok) return 0;
    }
    qsort(state->objects, state->object_count,
          sizeof *state->objects, compare_objects);
    qsort(state->parents, state->parent_count,
          sizeof *state->parents, compare_parents);
    size_t used = 0;
    for (size_t i = 0; i < state->object_count;) {
        size_t last = i;
        while (last + 1 < state->object_count &&
               state->objects[last + 1].value.id ==
               state->objects[i].value.id) ++last;
        state->objects[used++] = state->objects[last];
        i = last + 1;
    }
    state->object_count = used;
    used = 0;
    for (size_t i = 0; i < state->parent_count;) {
        size_t last = i;
        while (last + 1 < state->parent_count &&
               !strcmp(state->parents[last + 1].value.name,
                       state->parents[i].value.name)) ++last;
        state->parents[used++] = state->parents[last];
        i = last + 1;
    }
    state->parent_count = used;
    return 1;
}

static int append_instances(build_state *state, worldplan_instance *more,
                            size_t count) {
    if (count > SIZE_MAX / sizeof *more - state->instance_count) return 0;
    size_t needed = state->instance_count + count;
    if (needed > state->instance_capacity) {
        size_t maximum = SIZE_MAX / sizeof *more;
        size_t next = state->instance_capacity > maximum / 2 ?
                      needed : state->instance_capacity * 2;
        if (next < needed) next = needed;
        worldplan_instance *new_items = realloc(state->instances,
                                                next * sizeof *new_items);
        if (!new_items) return 0;
        state->instances = new_items;
        state->instance_capacity = next;
    }
    if (count) memcpy(state->instances + state->instance_count, more,
                      count * sizeof *more);
    state->instance_count = needed;
    return 1;
}

static int read_streams(build_state *state, const worldbuild_options *options,
                        const char *name, ipl_instance **output,
                        size_t *count) {
    const char *base = strrchr(name, '/');
    const char *backslash = strrchr(name, '\\');
    if (backslash && (!base || backslash > base)) base = backslash;
    base = base ? base + 1 : name;
    const char *ext = strrchr(base, '.');
    size_t length = ext ? (size_t)(ext - base) : strlen(base);
    if (length > 110) return 0;
    *output = NULL;
    *count = 0;
    for (size_t i = 0; i < UINT16_MAX; ++i) {
        char filename[133];
        int written = snprintf(filename, sizeof filename, "%.*s_stream%zu.ipl",
                               (int)length, base, i);
        if (written < 0 || (size_t)written >= sizeof filename) return 0;
        for (char *c = filename; *c; ++c)
            *c = (char)tolower((unsigned char)*c);
        img_entry entry;
        int pack;
        if (!worldplan_resolve(filename, state->main, state->main_count,
                               state->interior, state->interior_count,
                               &entry, &pack)) break;
        uint8_t *data = malloc(entry.size);
        if (!data) return 0;
        int ok = read_archive(options, (uint32_t)pack, entry.offset,
                              entry.size, data);
        ipl_instance *records = NULL;
        size_t n = 0;
        if (ok) ok = ipl_parse_binary(data, entry.size, &records, &n);
        free(data);
        if (ok && n > SIZE_MAX / sizeof *records - *count) ok = 0;
        if (ok && n) {
            ipl_instance *more = realloc(*output, (*count + n) * sizeof *more);
            if (more) *output = more;
            else ok = 0;
        }
        if (ok && n) {
            memcpy(*output + *count, records, n * sizeof *records);
            *count += n;
        }
        free(records);
        if (!ok) return 0;
    }
    return 1;
}

static int load_ipls(build_state *state, const worldbuild_options *options) {
    worldplan_model *models = state->object_count ?
        malloc(state->object_count * sizeof *models) : NULL;
    if (state->object_count && !models) return 0;
    for (size_t i = 0; i < state->object_count; ++i) {
        worldplan_object *entry = &state->objects[i].value;
        char name[133];
        img_entry found;
        int pack;
        if (snprintf(name, sizeof name, "%s.dff", entry->name) >=
            (int)sizeof name) {
            free(models);
            return 0;
        }
        models[i] = (worldplan_model){.id = entry->id,
            .available = worldplan_resolve(name, state->main, state->main_count,
                                           state->interior, state->interior_count,
                                           &found, &pack)};
        memcpy(models[i].name, entry->name, sizeof models[i].name);
    }
    int ok = 1;
    for (size_t i = 0; i < state->file_count && ok; ++i) {
        if (state->files[i].kind != 1) continue;
        uint8_t *data = NULL;
        size_t size = 0;
        if (!load_file(options, state->files[i].path, &data, &size)) continue;
        ipl_instance *text = NULL, *streams = NULL;
        size_t text_count = 0, stream_count = 0;
        worldplan_instance *planned = NULL;
        size_t count = 0;
        uint32_t skipped[3];
        ok = data && ipl_parse_text((const char *)data, size, &text,
                                     &text_count);
        free(data);
        if (ok) ok = read_streams(state, options, state->files[i].path,
                                  &streams, &stream_count);
        if (ok) ok = worldplan_instances(text, text_count, streams,
                                          stream_count, models, state->object_count,
                                          options->use_bounds ? options->bounds : NULL,
                                          options->keep_lods, &planned, &count,
                                          skipped);
        if (ok) {
            state->stats.skip_models += skipped[0];
            state->stats.skip_lods += skipped[1];
            state->stats.skip_bounds += skipped[2];
            ok = append_instances(state, planned, count);
        }
        free(text);
        free(streams);
        free(planned);
    }
    free(models);
    return ok;
}

static size_t model_index(const build_state *state, int32_t id) {
    size_t low = 0, high = state->id_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (state->ids[mid] < id) low = mid + 1;
        else high = mid;
    }
    return low < state->id_count && state->ids[low] == id ?
           low : state->id_count;
}

static int plan_tables(build_state *state, const worldbuild_options *options) {
    if (state->instance_count > SIZE_MAX / sizeof *state->ids) return 0;
    state->ids = state->instance_count ?
        malloc(state->instance_count * sizeof *state->ids) : NULL;
    if (state->instance_count && !state->ids) return 0;
    for (size_t i = 0; i < state->instance_count; ++i)
        state->ids[i] = state->instances[i].model;
    if (state->instance_count > 1)
        qsort(state->ids, state->instance_count,
              sizeof *state->ids, compare_id);
    for (size_t i = 0; i < state->instance_count; ++i)
        if (!state->id_count ||
            state->ids[i] != state->ids[state->id_count - 1])
            state->ids[state->id_count++] = state->ids[i];
    if (state->id_count > UINT16_MAX) return 0;
    worldplan_object *objects = state->object_count ?
        malloc(state->object_count * sizeof *objects) : NULL;
    worldplan_parent *parents = state->parent_count ?
        malloc(state->parent_count * sizeof *parents) : NULL;
    if ((state->object_count && !objects) ||
        (state->parent_count && !parents)) {
        free(objects);
        free(parents);
        return 0;
    }
    for (size_t i = 0; i < state->object_count; ++i)
        objects[i] = state->objects[i].value;
    for (size_t i = 0; i < state->parent_count; ++i)
        parents[i] = state->parents[i].value;
    int ok = worldplan_tables(state->ids, state->id_count, objects,
                              state->object_count, parents,
                              state->parent_count, state->main,
                              state->main_count, state->interior,
                              state->interior_count, &state->models,
                              &state->txds, &state->txd_count);
    free(objects);
    free(parents);
    if (!ok) return 0;
    state->packed_models = state->id_count ?
        calloc(state->id_count, sizeof *state->packed_models) : NULL;
    state->collisions = state->id_count ?
        calloc(state->id_count, sizeof *state->collisions) : NULL;
    state->packed_txds = state->txd_count ?
        calloc(state->txd_count, sizeof *state->packed_txds) : NULL;
    if ((state->id_count && (!state->packed_models || !state->collisions)) ||
        (state->txd_count && !state->packed_txds)) return 0;
    for (size_t i = 0; i < state->id_count; ++i) {
        worldplan_model_entry *model = &state->models[i];
        worldpack_model *packed = &state->packed_models[i];
        memcpy(packed->name, model->name, sizeof packed->name);
        packed->txd = model->txd;
        packed->draw_distance = model->draw_distance <= 0 ? 0 :
            model->draw_distance >= UINT16_MAX ? UINT16_MAX :
            (uint16_t)model->draw_distance;
        packed->time_on = model->on;
        packed->time_off = model->off;
        packed->flags = (uint32_t)model->flags;
        uint8_t *data = model->size ? malloc(model->size) : NULL;
        uint16_t radius = 64;
        if (data && read_archive(options, model->pack, model->offset,
                                 model->size, data))
            if (!dff_radius(data, model->size, &radius)) radius = 64;
        free(data);
        packed->radius = radius;
    }
    for (size_t i = 0; i < state->txd_count; ++i) {
        memcpy(state->packed_txds[i].name, state->txds[i].name,
               sizeof state->packed_txds[i].name);
        state->packed_txds[i].parent = state->txds[i].parent;
        state->packed_txds[i].pack = state->txds[i].pack;
    }
    state->stats.models = (uint32_t)state->id_count;
    state->stats.txds = (uint32_t)state->txd_count;
    state->stats.instances = (uint32_t)state->instance_count;
    return 1;
}

static int plan_sectors(build_state *state, const worldbuild_options *options) {
    if (!worldplan_sectors(state->instances, state->instance_count,
                           options->sector, &state->ox, &state->oy,
                           &state->width, &state->height, &state->cells,
                           &state->order)) return 0;
    state->stats.sectors = (uint32_t)state->width * state->height;
    state->packed_instances = state->instance_count ?
        calloc(state->instance_count, sizeof *state->packed_instances) : NULL;
    if (state->instance_count && !state->packed_instances) return 0;
    for (size_t i = 0; i < state->instance_count; ++i) {
        worldplan_instance *instance = &state->instances[state->order[i]];
        size_t mid = model_index(state, instance->model);
        size_t linked = model_index(state, instance->linked);
        if (mid >= state->id_count) return 0;
        worldpack_instance *record = &state->packed_instances[i];
        memcpy(record->pos, instance->position, sizeof record->pos);
        memcpy(record->rot, instance->rotation, sizeof record->rot);
        record->model = (uint16_t)mid;
        record->switch_flags = (linked < state->id_count ? 100 : 0) |
                               (instance->is_lod ? 0x8000 : 0);
        record->linked = linked < state->id_count ?
                         (uint16_t)linked : UINT16_MAX;
        record->area = instance->area;
    }
    return 1;
}

static int load_collisions(build_state *state,
                           const worldbuild_options *options) {
    uint8_t *seen = state->id_count ? calloc(state->id_count, 1) : NULL;
    if (state->id_count && !seen) return 0;
    int ok = 1;
    for (uint32_t pack = 0; pack < 2 && ok; ++pack) {
        img_entry *entries = pack ? state->interior_order : state->main_order;
        size_t count = pack ? state->interior_count : state->main_count;
        for (size_t i = 0; i < count && ok; ++i) {
            const char *name = entries[i].name;
            size_t size = strlen(name);
            if (size < 4 || name[size - 4] != '.' ||
                tolower((unsigned char)name[size - 3]) != 'c' ||
                tolower((unsigned char)name[size - 2]) != 'o' ||
                tolower((unsigned char)name[size - 1]) != 'l')
                continue;
            uint8_t *data = entries[i].size ? malloc(entries[i].size) : NULL;
            if (!data) {
                ok = 0;
                break;
            }
            if (!read_archive(options, pack, entries[i].offset,
                              entries[i].size, data)) {
                free(data);
                ok = 0;
                break;
            }
            worldcol_entry *models = NULL;
            size_t model_count = 0;
            int parsed = worldcol_parse_archive(data, entries[i].size,
                                                 &models, &model_count);
            free(data);
            if (!parsed) continue;
            for (size_t j = 0; j < model_count && ok; ++j) {
                for (char *p = models[j].name; *p; ++p)
                    *p = (char)tolower((unsigned char)*p);
                int wanted = 0, already = 1;
                for (size_t k = 0; k < state->id_count && ok; ++k) {
                    if (strcmp(models[j].name, state->models[k].name))
                        continue;
                    wanted = 1;
                    if (seen[k]) continue;
                    already = 0;
                    seen[k] = 1;
                    uint8_t *record = models[j].length ?
                        malloc(models[j].length) : NULL;
                    if (models[j].length && !record) {
                        ok = 0;
                        break;
                    }
                    if (record) memcpy(record, models[j].record,
                                       models[j].length);
                    state->collisions[k] = (worldpack_collision){
                        record, models[j].length, models[j].radius
                    };
                    if (record) {
                        ++state->stats.col_models;
                        state->stats.col_faces += models[j].faces;
                        state->stats.col_bytes += models[j].length;
                    }
                }
                if (wanted && already && !pack)
                    ++state->stats.col_dups;
            }
            worldcol_free_archive(models, model_count);
        }
    }
    free(seen);
    return ok;
}

int worldbuild_convert(const worldbuild_options *options,
                       uint8_t **output, size_t *length,
                       worldbuild_stats *stats) {
    if (!options || !options->file || !options->range ||
        !options->sector || !output || !length || !stats) return 0;
    *output = NULL;
    *length = 0;
    build_state state = {0};
    uint8_t *dat = NULL;
    size_t dat_length = 0;
    int ok = load_file(options, "data/gta.dat", &dat, &dat_length) &&
             dat && worldplan_parse_dat((const char *)dat, dat_length,
                                         &state.files, &state.file_count);
    free(dat);
    if (ok) ok = read_index(options, 0, &state.main, &state.main_order,
                             &state.main_count) &&
                 read_index(options, 1, &state.interior,
                             &state.interior_order, &state.interior_count);
    if (ok) ok = load_ides(&state, options) && load_ipls(&state, options) &&
                 plan_tables(&state, options) &&
                 plan_sectors(&state, options) &&
                 load_collisions(&state, options);
    if (ok) {
        worldpack_input input = {
            state.ox, state.oy, options->sector, state.width, state.height,
            state.packed_models, state.id_count,
            state.packed_txds, state.txd_count, state.cells,
            state.packed_instances, state.instance_count, state.collisions
        };
        ok = worldpack_encode(&input, &state.output, &state.length);
    }
    if (ok) {
        *output = state.output;
        *length = state.length;
        *stats = state.stats;
        state.output = NULL;
    }
    cleanup(&state);
    return ok;
}
