#include "worldplan.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int worldplan_parse_dat(const char *source, size_t length,
                        worldplan_file **files, size_t *count) {
    if (!source || !files || !count || length == SIZE_MAX) return 0;
    *files = NULL;
    *count = 0;
    char *text = malloc(length + 1);
    if (!text) return 0;
    memcpy(text, source, length);
    text[length] = 0;
    worldplan_file *results = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = text; line < text + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        while (isspace((unsigned char)*line)) ++line;
        char *stop = line + strlen(line);
        while (stop > line && isspace((unsigned char)stop[-1])) *--stop = 0;
        char *space = strchr(line, ' ');
        if (space) {
            *space++ = 0;
            while (isspace((unsigned char)*space)) ++space;
            stop = space + strlen(space);
            while (stop > space && isspace((unsigned char)stop[-1])) *--stop = 0;
            int ide = strlen(line) == 3 && toupper((unsigned char)line[0]) == 'I' &&
                      toupper((unsigned char)line[1]) == 'D' &&
                      toupper((unsigned char)line[2]) == 'E';
            int ipl = strlen(line) == 3 && toupper((unsigned char)line[0]) == 'I' &&
                      toupper((unsigned char)line[1]) == 'P' &&
                      toupper((unsigned char)line[2]) == 'L';
            size_t n = strlen(space);
            int ipl_file = ipl && n >= 4 && space[n - 4] == '.' &&
                           tolower((unsigned char)space[n - 3]) == 'i' &&
                           tolower((unsigned char)space[n - 2]) == 'p' &&
                           tolower((unsigned char)space[n - 1]) == 'l';
            if ((ide || ipl_file) &&
                n < sizeof results[0].path) {
                if (used == capacity) {
                    size_t next_capacity = capacity ? capacity * 2 : 64;
                    if (next_capacity > SIZE_MAX / sizeof *results) {
                        ok = 0;
                        break;
                    }
                    worldplan_file *next = realloc(results, next_capacity * sizeof *next);
                    if (!next) {
                        ok = 0;
                        break;
                    }
                    results = next;
                    capacity = next_capacity;
                }
                memset(&results[used], 0, sizeof results[used]);
                memcpy(results[used].path, space, n);
                results[used].kind = (uint8_t)(ipl ? 1 : 0);
                ++used;
            }
        }
        line = end ? end + 1 : text + length;
    }
    free(text);
    if (ok) {
        *files = results;
        *count = used;
    } else free(results);
    return ok;
}

static const worldplan_model *lookup(const worldplan_model *models, size_t count,
                                     int32_t id) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (models[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && models[lo].id == id ? &models[lo] : NULL;
}

static const img_entry *asset(const img_entry *entries, size_t count, const char *name) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(entries[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && !strcmp(entries[lo].name, name) ? &entries[lo] : NULL;
}

int worldplan_resolve(const char *name, const img_entry *main, size_t main_count,
                      const img_entry *interior, size_t interior_count,
                      img_entry *found, int *pack) {
    if (!name || !found || !pack ||
        (main_count && !main) || (interior_count && !interior)) return 0;
    const img_entry *value = asset(main, main_count, name);
    if (value) *pack = 0;
    else {
        value = asset(interior, interior_count, name);
        *pack = 1;
    }
    if (!value) return 0;
    *found = *value;
    return 1;
}

int worldplan_instances(const ipl_instance *text, size_t text_count,
                        const ipl_instance *stream, size_t stream_count,
                        const worldplan_model *models, size_t model_count,
                        const double *bounds, int keep_lods,
                        worldplan_instance **output, size_t *count,
                        uint32_t skipped[3]) {
    if (!output || !count || !skipped || (text_count && !text) ||
        (stream_count && !stream) || (model_count && !models) ||
        stream_count > SIZE_MAX - text_count ||
        text_count > SIZE_MAX / sizeof(int32_t))
        return 0;
    *output = NULL;
    *count = 0;
    memset(skipped, 0, 3 * sizeof *skipped);
    size_t total = text_count + stream_count;
    if (total > SIZE_MAX / sizeof(worldplan_instance)) return 0;
    worldplan_instance *results = total ? malloc(total * sizeof *results) : NULL;
    int32_t *linked = text_count ? malloc(text_count * sizeof *linked) : NULL;
    if ((total && !results) || (text_count && !linked)) {
        free(results); free(linked);
        return 0;
    }
    for (size_t i = 0; i < text_count; ++i) linked[i] = INT32_MIN;
    for (size_t i = 0; i < total; ++i) {
        const ipl_instance *p = i < text_count ? &text[i] : &stream[i - text_count];
        if (p->lod >= 0 && (size_t)p->lod < text_count)
            linked[p->lod] = p->model;
    }
    size_t used = 0;
    for (size_t i = 0; i < total; ++i) {
        const ipl_instance *p = i < text_count ? &text[i] : &stream[i - text_count];
        const worldplan_model *m = lookup(models, model_count, p->model);
        if (!m || !m->available) {
            ++skipped[0];
            continue;
        }
        int is_lod = (i < text_count && linked[i] != INT32_MIN) ||
                     !strncmp(m->name, "lod", 3);
        if (is_lod && !keep_lods) {
            ++skipped[1];
            continue;
        }
        double x = p->transform[0], y = p->transform[1];
        if (bounds && !(bounds[0] <= x && x <= bounds[2] &&
                        bounds[1] <= y && y <= bounds[3])) {
            ++skipped[2];
            continue;
        }
        worldplan_instance *out = &results[used++];
        out->model = p->model;
        out->linked = is_lod && i < text_count ? linked[i] :
                      !is_lod && p->lod >= 0 && (size_t)p->lod < text_count ?
                      text[p->lod].model : INT32_MIN;
        for (unsigned j = 0; j < 3; ++j) out->position[j] = p->transform[j];
        ipl_rotation(p->transform[3], p->transform[4], p->transform[5],
                     p->transform[6], out->rotation);
        out->is_lod = (uint8_t)is_lod;
        out->area = (uint8_t)p->area;
    }
    free(linked);
    *output = results;
    *count = used;
    return 1;
}

int worldplan_sectors(const worldplan_instance *instances, size_t count, int sector,
                      int16_t *ox, int16_t *oy, uint16_t *width, uint16_t *height,
                      uint16_t **counts, size_t **order) {
    if ((count && !instances) || sector <= 0 || !ox || !oy || !width || !height ||
        !counts || !order || count > SIZE_MAX / sizeof(size_t)) return 0;
    double min_x = count ? instances[0].position[0] : 0;
    double max_x = min_x;
    double min_y = count ? instances[0].position[1] : 0;
    double max_y = min_y;
    for (size_t i = 1; i < count; ++i) {
        double x = instances[i].position[0], y = instances[i].position[1];
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    double x0 = floor(min_x / sector) * sector;
    double y0 = floor(min_y / sector) * sector;
    double gw = floor((max_x - x0) / sector) + 1;
    double gh = floor((max_y - y0) / sector) + 1;
    if (!isfinite(x0) || !isfinite(y0) || x0 < INT16_MIN || x0 > INT16_MAX ||
        y0 < INT16_MIN || y0 > INT16_MAX || gw < 1 || gw > UINT16_MAX ||
        gh < 1 || gh > UINT16_MAX ||
        (uint64_t)gw * (uint64_t)gh > SIZE_MAX / sizeof(uint16_t)) return 0;
    size_t cells = (size_t)gw * (size_t)gh;
    uint16_t *sizes = calloc(cells, sizeof *sizes);
    size_t *places = count ? malloc(count * sizeof *places) : NULL;
    size_t *starts = calloc(cells, sizeof *starts);
    size_t *permutation = count ? malloc(count * sizeof *permutation) : NULL;
    int ok = sizes && starts && (!count || (places && permutation));
    for (size_t i = 0; i < count && ok; ++i) {
        double cx = floor((instances[i].position[0] - x0) / sector);
        double cy = floor((instances[i].position[1] - y0) / sector);
        if (cx < 0 || cx >= gw || cy < 0 || cy >= gh) {
            ok = 0;
            break;
        }
        size_t cell = (size_t)cy * (size_t)gw + (size_t)cx;
        if (sizes[cell] == UINT16_MAX) {
            ok = 0;
            break;
        }
        ++sizes[cell];
        places[i] = cell;
    }
    if (ok) {
        size_t offset = 0;
        for (size_t i = 0; i < cells; ++i) {
            starts[i] = offset;
            offset += sizes[i];
        }
        for (size_t i = 0; i < count; ++i)
            permutation[starts[places[i]]++] = i;
        *ox = (int16_t)x0;
        *oy = (int16_t)y0;
        *width = (uint16_t)gw;
        *height = (uint16_t)gh;
        *counts = sizes;
        *order = permutation;
    } else {
        free(sizes);
        free(permutation);
    }
    free(places);
    free(starts);
    return ok;
}

typedef struct {
    const worldplan_parent *parents;
    size_t parent_count;
    const img_entry *main, *interior;
    size_t main_count, interior_count;
    worldplan_txd_entry *txds;
    size_t count, capacity;
    int error;
} table_builder;

static const worldplan_object *object_for(const worldplan_object *objects, size_t count,
                                          int32_t id) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (objects[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && objects[lo].id == id ? &objects[lo] : NULL;
}

static const char *parent_for(const table_builder *b, const char *name) {
    size_t lo = 0, hi = b->parent_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(b->parents[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < b->parent_count && !strcmp(b->parents[lo].name, name) ?
           b->parents[lo].parent : NULL;
}

static const img_entry *find_source(const table_builder *b, const char *name,
                                    const char *extension, int *pack) {
    size_t n = 0;
    while (n < 128 && name[n]) ++n;
    if (n == 128) return NULL;
    char filename[133];
    memcpy(filename, name, n);
    memcpy(filename + n, extension, 5);
    const img_entry *found = asset(b->main, b->main_count, filename);
    if (found) {
        *pack = 0;
        return found;
    }
    found = asset(b->interior, b->interior_count, filename);
    if (found) *pack = 1;
    return found;
}

static int txd_index(table_builder *b, const char *name) {
    for (size_t i = 0; i < b->count; ++i)
        if (!strcmp(b->txds[i].name, name)) return (int)i;
    if (b->count > INT16_MAX) {
        b->error = 1;
        return -1;
    }
    if (b->count == b->capacity) {
        size_t next_capacity = b->capacity ? b->capacity * 2 : 128;
        if (next_capacity > SIZE_MAX / sizeof *b->txds) {
            b->error = 1;
            return -1;
        }
        worldplan_txd_entry *next = realloc(b->txds, next_capacity * sizeof *next);
        if (!next) {
            b->error = 1;
            return -1;
        }
        b->txds = next;
        b->capacity = next_capacity;
    }
    size_t index = b->count++;
    worldplan_txd_entry *entry = &b->txds[index];
    memset(entry, 0, sizeof *entry);
    memcpy(entry->name, name, strlen(name) + 1);
    entry->parent = -1;
    int pack = 0;
    find_source(b, name, ".txd", &pack);
    entry->pack = (uint16_t)pack;
    const char *parent = parent_for(b, name);
    if (parent && *parent && strcmp(parent, name) &&
        find_source(b, parent, ".txd", &pack)) {
        int next = txd_index(b, parent);
        if (next < 0) return -1;
        b->txds[index].parent = (int16_t)next;
    }
    return (int)index;
}

int worldplan_tables(const int32_t *ids, size_t count,
                     const worldplan_object *objects, size_t object_count,
                     const worldplan_parent *parents, size_t parent_count,
                     const img_entry *main, size_t main_count,
                     const img_entry *interior, size_t interior_count,
                     worldplan_model_entry **models,
                     worldplan_txd_entry **txds, size_t *txd_count) {
    if (!models || !txds || !txd_count ||
        (count && !ids) || (object_count && !objects) ||
        (parent_count && !parents) || (main_count && !main) ||
        (interior_count && !interior) ||
        count > SIZE_MAX / sizeof(worldplan_model_entry)) return 0;
    *models = NULL;
    *txds = NULL;
    *txd_count = 0;
    worldplan_model_entry *result = count ? calloc(count, sizeof *result) : NULL;
    if (count && !result) return 0;
    table_builder b = {parents, parent_count, main, interior, main_count,
                       interior_count, NULL, 0, 0, 0};
    int ok = 1;
    for (size_t i = 0; i < count && ok; ++i) {
        const worldplan_object *obj = object_for(objects, object_count, ids[i]);
        if (!obj) {
            ok = 0;
            break;
        }
        int pack = 0;
        const img_entry *source = find_source(&b, obj->name, ".dff", &pack);
        if (!source) {
            ok = 0;
            break;
        }
        worldplan_model_entry *item = &result[i];
        memcpy(item->name, obj->name, sizeof item->name);
        item->draw_distance = obj->draw_distance;
        item->flags = obj->flags | ((int64_t)pack << 31);
        item->offset = source->offset;
        item->size = source->size;
        item->pack = (uint16_t)pack;
        item->on = obj->on;
        item->off = obj->off;
        item->txd = UINT16_MAX;
        if (find_source(&b, obj->txd, ".txd", &pack)) {
            int index = txd_index(&b, obj->txd);
            if (index < 0) ok = 0;
            else item->txd = (uint16_t)index;
        }
        if (b.error) ok = 0;
    }
    if (ok) {
        *models = result;
        *txds = b.txds;
        *txd_count = b.count;
    } else {
        free(result);
        free(b.txds);
    }
    return ok;
}
