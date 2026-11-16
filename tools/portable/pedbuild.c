#include "pedbuild.h"

#include "pedpack.h"
#include "pedparse.h"
#include "pedpath.h"
#include "pedscm.h"
#include "pedtables.h"
#include "vehparse.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pedtable_model *models;
    size_t model_count;
    pedtable_stat *stats;
    size_t stat_count;
    pedtable_anim *anims;
    size_t anim_count;
    pedtable_group *groups, *cars;
    size_t car_count;
    pedtable_zone *zones;
    size_t zone_count;
    int32_t *population;
    size_t pop_count;
    uint8_t *tables;
    size_t tables_length;
    size_t table_offsets[9];
    uint8_t *path_nodes, *path_links, *path_cells;
    uint8_t *road_nodes, *road_links, *road_cells;
    size_t path_nodes_length, path_links_length, path_cells_length;
    size_t road_nodes_length, road_links_length, road_cells_length;
    uint32_t path_link_count, road_link_count;
    int16_t gx0, gy0;
    uint16_t gw, gh;
    int32_t script_start, script_end;
    uint32_t scripted;
} ped_state;

static void release(ped_state *state) {
    free(state->models);
    free(state->stats);
    free(state->anims);
    if (state->groups)
        for (size_t i = 0; i < 57; ++i) free((void *)state->groups[i].members);
    if (state->cars)
        for (size_t i = 0; i < state->car_count; ++i)
            free((void *)state->cars[i].members);
    free(state->groups);
    free(state->cars);
    free(state->zones);
    free(state->population);
    free(state->tables);
    free(state->path_nodes);
    free(state->path_links);
    free(state->path_cells);
    free(state->road_nodes);
    free(state->road_links);
    free(state->road_cells);
}

static int load(const pedbuild_options *options, const char *path,
                uint8_t **data, size_t *length) {
    *data = NULL;
    *length = 0;
    return options->file(options->context, path, data, length);
}

static int get_peds(const pedbuild_options *options, pedparse_ide_entry **out,
                    size_t *count) {
    uint8_t *data;
    size_t size;
    if (!load(options, "data/peds.ide", &data, &size)) return 0;
    int ok = pedparse_ide((const char *)data, size, out, count);
    free(data);
    return ok;
}

static int get_stats(const pedbuild_options *options, ped_state *state,
                     pedparse_stat_entry **names) {
    uint8_t *data;
    size_t size;
    if (!load(options, "data/pedstats.dat", &data, &size)) return 0;
    int ok = pedparse_stats((const char *)data, size, names,
                            &state->stat_count);
    free(data);
    if (!ok) return 0;
    state->stats = state->stat_count ?
        calloc(state->stat_count, sizeof *state->stats) : NULL;
    if (state->stat_count && !state->stats) return 0;
    for (size_t i = 0; i < state->stat_count; ++i)
        memcpy(state->stats[i].value, (*names)[i].values,
               sizeof state->stats[i].value);
    return 1;
}

typedef struct {
    char name[128];
    char clips[6][24];
} anim_record;

static int compare_anim(const void *a, const void *b) {
    return strcmp(((const anim_record *)a)->name,
                  ((const anim_record *)b)->name);
}

static void copy_name(char *output, size_t length, const char *input) {
    size_t size = 0;
    while (size + 1 < length && input[size]) ++size;
    memcpy(output, input, size);
}

static int get_anims(const pedbuild_options *options, ped_state *state,
                     const pedparse_ide_entry *peds) {
    uint8_t *data;
    size_t size;
    if (!load(options, "data/animgrp.dat", &data, &size)) return 0;
    pedparse_anim_event *events = NULL;
    size_t count = 0;
    int ok = pedparse_animgrp((const char *)data, size, &events, &count);
    free(data);
    if (!ok) return 0;
    anim_record *records = count ? calloc(count, sizeof *records) : NULL;
    size_t used = 0, active = SIZE_MAX;
    if (count && !records) ok = 0;
    for (size_t i = 0; i < count && ok; ++i) {
        if (events[i].kind == 0) {
            size_t j = 0;
            for (; j < used && strcmp(records[j].name, events[i].name); ++j) {}
            if (j == used) ++used;
            memset(&records[j], 0, sizeof records[j]);
            memcpy(records[j].name, events[i].name,
                   sizeof records[j].name);
            active = j;
            continue;
        }
        if (events[i].kind == 2) active = SIZE_MAX;
        if (events[i].kind != 1 || active == SIZE_MAX) continue;
        anim_record *current = &records[active];
        for (size_t j = 0; j < 6; ++j)
            if (!current->clips[j][0]) {
                copy_name(current->clips[j], sizeof current->clips[j],
                          events[i].name);
                break;
            }
    }
    free(events);
    if (ok) {
        qsort(records, used, sizeof *records, compare_anim);
        state->anims = calloc(state->model_count + 1, sizeof *state->anims);
        ok = state->anims != NULL;
    }
    if (ok) {
        memcpy(state->anims[0].name, "man", 3);
        for (size_t i = 0; i < used; ++i)
            if (!strcmp(records[i].name, "man"))
                memcpy(state->anims[0].clips, records[i].clips,
                       sizeof records[i].clips);
        state->anim_count = 1;
        for (size_t i = 0; i < used; ++i) {
            if (!strcmp(records[i].name, "man")) continue;
            int wanted = 0;
            for (size_t j = 0; j < state->model_count; ++j)
                if (!strcmp(peds[j].anim, records[i].name)) {
                    wanted = 1;
                    break;
                }
            if (!wanted) continue;
            if (state->anim_count > state->model_count) {
                ok = 0;
                break;
            }
            pedtable_anim *item = &state->anims[state->anim_count++];
            copy_name(item->name, sizeof item->name, records[i].name);
            memcpy(item->clips, records[i].clips, sizeof item->clips);
        }
    }
    free(records);
    return ok;
}

static const char *ped_types[] = {
    "PLAYER1", "PLAYER2", "PLAYER_NETWORK", "PLAYER_UNUSED",
    "CIVMALE", "CIVFEMALE", "COP", "GANG1", "GANG2", "GANG3",
    "GANG4", "GANG5", "GANG6", "GANG7", "GANG8", "GANG9",
    "GANG10", "DEALER", "MEDIC", "FIREMAN", "CRIMINAL",
    "BUM", "PROSTITUTE", "SPECIAL"
};

static int build_models(ped_state *state, const pedparse_ide_entry *peds,
                        const pedparse_stat_entry *stats) {
    state->models = state->model_count ?
        calloc(state->model_count, sizeof *state->models) : NULL;
    if (state->model_count && !state->models) return 0;
    for (size_t i = 0; i < state->model_count; ++i) {
        const pedparse_ide_entry *source = &peds[i];
        pedtable_model *item = &state->models[i];
        copy_name(item->model, sizeof item->model, source->model);
        copy_name(item->txd, sizeof item->txd, source->txd);
        item->id = (uint16_t)source->id;
        item->cars = (uint16_t)source->cars;
        item->flags = (uint8_t)source->flags;
        item->type = 4;
        for (size_t j = 0; j < sizeof ped_types / sizeof *ped_types; ++j)
            if (!strcmp(source->type, ped_types[j])) {
                item->type = (uint8_t)j;
                break;
            }
        for (size_t j = 0; j < state->stat_count; ++j)
            if (!strcmp(source->stat, stats[j].name))
                item->stat = (uint8_t)j;
        for (size_t j = 0; j < state->anim_count; ++j)
            if (!strcmp(source->anim, state->anims[j].name))
                item->anim = (uint8_t)j;
        for (size_t j = 0; j < 2 && source->model[j]; ++j) {
            int c = toupper((unsigned char)source->model[j]);
            if (c == 'B') item->race = 1;
            else if (c == 'W') item->race = 2;
            else if (c == 'O' || c == 'I') item->race = 3;
            else if (c == 'H') item->race = 4;
            if (item->race) break;
        }
    }
    return 1;
}

static int compare_named(const void *a, const void *b) {
    const pedparse_named_model *x = a, *y = b;
    int order = strcmp(x->name, y->name);
    if (order) return order;
    return x->index < y->index ? 1 : x->index > y->index ? -1 : 0;
}

static int get_group_data(const pedbuild_options *options,
                          const char *path,
                          const pedparse_named_model *names, size_t name_count,
                          uint16_t max_groups, int skip_slashes,
                          pedtable_group **groups, size_t *group_count) {
    uint8_t *data;
    size_t length;
    if (!load(options, path, &data, &length)) return 0;
    pedparse_group_member *members = NULL;
    size_t member_count = 0;
    uint16_t used = 0;
    int ok = pedparse_groups((const char *)data, length, names, name_count,
                             max_groups, skip_slashes, &members,
                             &member_count, &used);
    free(data);
    if (ok) {
        *groups = calloc(max_groups, sizeof **groups);
        ok = *groups != NULL;
    }
    *group_count = ok ? max_groups : 0;
    for (size_t i = 0; i < member_count && ok; ++i) {
        if (members[i].group >= max_groups) {
            ok = 0;
            break;
        }
        ++(*groups)[members[i].group].count;
    }
    for (size_t i = 0; i < max_groups && ok; ++i) {
        size_t count = (*groups)[i].count;
        uint16_t *list = count ? malloc(count * sizeof *list) : NULL;
        if (count && !list) ok = 0;
        (*groups)[i].members = list;
        (*groups)[i].count = 0;
    }
    for (size_t i = 0; i < member_count && ok; ++i) {
        pedtable_group *group = &(*groups)[members[i].group];
        ((uint16_t *)group->members)[group->count++] = members[i].index;
    }
    free(members);
    if (!skip_slashes) return ok;
    return ok && used == max_groups;
}

static int get_groups(const pedbuild_options *options, ped_state *state,
                      const pedparse_ide_entry *peds) {
    pedparse_named_model *names = state->model_count ?
        calloc(state->model_count, sizeof *names) : NULL;
    if (state->model_count && !names) return 0;
    for (size_t i = 0; i < state->model_count; ++i) {
        memcpy(names[i].name, peds[i].model, sizeof names[i].name);
        names[i].index = (uint16_t)i;
    }
    qsort(names, state->model_count, sizeof *names, compare_named);
    int ok = get_group_data(options, "data/pedgrp.dat", names,
                             state->model_count, 57, 0,
                             &state->groups, &(size_t){0});
    free(names);
    if (!ok) return 0;
    uint8_t *data;
    size_t length;
    if (!load(options, "data/vehicles.ide", &data, &length)) return 0;
    vehparse_ide_entry *vehicles = NULL;
    size_t count = 0;
    ok = vehparse_ide((const char *)data, length, &vehicles, &count);
    free(data);
    if (!ok) return 0;
    size_t named_count = 0;
    for (size_t i = 0; i < count; ++i)
        if (vehicles[i].type <= 1) ++named_count;
    names = named_count ? calloc(named_count, sizeof *names) : NULL;
    ok = !named_count || names != NULL;
    for (size_t i = 0, j = 0; i < count && ok; ++i) {
        if (vehicles[i].type > 1) continue;
        memcpy(names[j].name, vehicles[i].model, sizeof names[j].name);
        names[j++].index = (uint16_t)i;
    }
    free(vehicles);
    if (ok) {
        qsort(names, named_count, sizeof *names, compare_named);
        ok = get_group_data(options, "data/cargrp.dat", names, named_count,
                            29, 1, &state->cars, &state->car_count);
    }
    free(names);
    return ok;
}

static int get_population(const pedbuild_options *options, ped_state *state) {
    uint8_t *data;
    size_t size;
    if (!load(options, "data/popcycle.dat", &data, &size)) return 0;
    int ok = pedpop_parse((const char *)data, size,
                          &state->population, &state->pop_count);
    free(data);
    return ok;
}

static int16_t zone_min(double value) {
    if (value < INT16_MIN) return INT16_MIN;
    if (value > INT16_MAX) return INT16_MAX;
    return (int16_t)value;
}

static int get_zones(const pedbuild_options *options, ped_state *state) {
    uint8_t *data;
    size_t length;
    if (!load(options, "data/info.zon", &data, &length)) return 0;
    pedparse_zone_entry *zones = NULL;
    size_t count = 0;
    int ok = pedparse_zones((const char *)data, length, &zones, &count);
    free(data);
    if (!ok) return 0;
    state->zones = count ? calloc(count, sizeof *state->zones) : NULL;
    if (count && !state->zones) {
        free(zones);
        return 0;
    }
    pedscm_zone *script = NULL;
    size_t script_count = 0;
    if (load(options, "data/script/main.scm", &data, &length)) {
        ok = pedscm_scan(data, length, &script, &script_count,
                          &state->script_start, &state->script_end);
        free(data);
    }
    for (size_t i = 0; i < count && ok; ++i) {
        const double *b = zones[i].bounds, *w = options->bounds;
        if (b[2] < w[0] - 60 || b[0] > w[2] + 60 ||
            b[3] < w[1] - 60 || b[1] > w[3] + 60) continue;
        pedtable_zone *zone = &state->zones[state->zone_count++];
        for (size_t j = 0; j < 4; ++j)
            zone->bounds[j] = zone_min(b[j]);
        zone->population = 5;
        zone->races = 15;
        for (size_t j = 0; j < script_count; ++j) {
            if (strcmp(zones[i].name, script[j].label)) continue;
            ++state->scripted;
            if (script[j].has_population && script[j].population >= 0 &&
                script[j].population < 20)
                zone->population = (uint8_t)script[j].population;
            if (script[j].races >= 0)
                zone->races = (uint8_t)script[j].races;
            zone->dealer = script[j].dealer;
            memcpy(zone->gang, script[j].gang, sizeof zone->gang);
            break;
        }
    }
    free(zones);
    free(script);
    return ok;
}

typedef struct {
    uint16_t area;
    pedpath_node node;
    uint32_t cell;
} path_item;

typedef struct {
    uint32_t key;
    uint16_t index;
} path_lookup;

static int compare_path(const void *a, const void *b) {
    const path_item *x = a, *y = b;
    if (x->cell != y->cell) return x->cell < y->cell ? -1 : 1;
    if (x->area != y->area) return x->area < y->area ? -1 : 1;
    return x->node.index < y->node.index ? -1 :
           x->node.index > y->node.index;
}

static int compare_lookup(const void *a, const void *b) {
    uint32_t x = ((const path_lookup *)a)->key;
    uint32_t y = ((const path_lookup *)b)->key;
    return x < y ? -1 : x > y;
}

static int find_link(const path_lookup *items, size_t count,
                     uint32_t key, uint16_t *index) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (items[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo == count || items[lo].key != key) return 0;
    *index = items[lo].index;
    return 1;
}

static int make_graph(const pedbuild_options *options, ped_state *state,
                      int road) {
    size_t count = 0, capacity = 0;
    path_item *items = NULL;
    double bounds[4] = {
        options->bounds[0] - 60, options->bounds[1] - 60,
        options->bounds[2] + 60, options->bounds[3] + 60
    };
    int ok = 1;
    for (unsigned area = 0; area < 64 && ok; ++area) {
        char path[64];
        snprintf(path, sizeof path, "data/paths/nodes%u.dat", area);
        uint8_t *data;
        size_t size;
        if (!load(options, path, &data, &size)) continue;
        pedpath_node *nodes = NULL;
        size_t n = 0;
        ok = pedpath_parse(data, size, bounds, road, &nodes, &n);
        free(data);
        if (ok && n > SIZE_MAX / sizeof *items - count) ok = 0;
        if (ok && count + n > capacity) {
            size_t next = capacity ? capacity * 2 : 512;
            if (next < count + n) next = count + n;
            path_item *more = realloc(items, next * sizeof *more);
            if (more) {
                items = more;
                capacity = next;
            } else ok = 0;
        }
        for (size_t i = 0; i < n && ok; ++i) {
            path_item *item = &items[count++];
            item->area = (uint16_t)area;
            item->node = nodes[i];
            int cx = (int)floor(((double)nodes[i].x / 8 - state->gx0) / 50);
            int cy = (int)floor(((double)nodes[i].y / 8 - state->gy0) / 50);
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx >= state->gw) cx = state->gw - 1;
            if (cy >= state->gh) cy = state->gh - 1;
            item->cell = (uint32_t)cy * state->gw + (uint32_t)cx;
        }
        free(nodes);
    }
    if (road && count > INT16_MAX) ok = 0;
    if (count > UINT16_MAX) ok = 0;
    if (ok && count) qsort(items, count, sizeof *items, compare_path);
    path_lookup *lookup = ok && count ? malloc(count * sizeof *lookup) : NULL;
    pedpack_path_node *records = ok && count ?
        calloc(count, sizeof *records) : NULL;
    uint16_t *links = ok && count ? calloc(count * 15, sizeof *links) : NULL;
    if (count && (!lookup || !records || !links)) ok = 0;
    for (size_t i = 0; i < count && ok; ++i) {
        lookup[i].key = (uint32_t)items[i].area << 16 |
                        (uint16_t)items[i].node.index;
        lookup[i].index = (uint16_t)i;
    }
    if (ok && count) qsort(lookup, count, sizeof *lookup, compare_lookup);
    for (size_t i = 0; i < count && ok; ++i) {
        const path_item *source = &items[i];
        pedpack_path_node *item = &records[i];
        item->x = source->node.x;
        item->y = source->node.y;
        item->z = source->node.z;
        item->width = source->node.width;
        item->flags = source->node.flags;
        item->cell = source->cell;
        item->links = links + i * 15;
        uint32_t self = (uint32_t)source->area << 16 |
                         (uint16_t)source->node.index;
        for (size_t j = 0; j < source->node.link_count; ++j) {
            uint32_t key = (uint32_t)source->node.links[j].area << 16 |
                            source->node.links[j].node;
            uint16_t target;
            if ((!road || key != self) &&
                find_link(lookup, count, key, &target))
                links[i * 15 + item->link_count++] = target;
        }
    }
    uint8_t **nodes_out = road ? &state->road_nodes : &state->path_nodes;
    uint8_t **links_out = road ? &state->road_links : &state->path_links;
    uint8_t **cells_out = road ? &state->road_cells : &state->path_cells;
    size_t *nodes_size = road ? &state->road_nodes_length : &state->path_nodes_length;
    size_t *links_size = road ? &state->road_links_length : &state->path_links_length;
    size_t *cells_size = road ? &state->road_cells_length : &state->path_cells_length;
    uint32_t *links_count = road ? &state->road_link_count : &state->path_link_count;
    if (ok)
        ok = pedpack_paths(records, count, (size_t)state->gw * state->gh,
                           road, nodes_out, nodes_size, links_out, links_size,
                           cells_out, cells_size, links_count);
    free(items);
    free(lookup);
    free(records);
    free(links);
    return ok;
}

int pedbuild_convert(const pedbuild_options *options, uint8_t **output,
                     size_t *length, pedbuild_stats *stats) {
    if (!options || !options->file || !output || !length || !stats) return 0;
    *output = NULL;
    *length = 0;
    ped_state state = {0};
    double x0 = floor(trunc(options->bounds[0] - 60) / 50) * 50;
    double y0 = floor(trunc(options->bounds[1] - 60) / 50) * 50;
    double width = floor((trunc(options->bounds[2] + 60) - x0) / 50) + 1;
    double height = floor((trunc(options->bounds[3] + 60) - y0) / 50) + 1;
    if (!isfinite(x0) || !isfinite(y0) || x0 < INT16_MIN || x0 > INT16_MAX ||
        y0 < INT16_MIN || y0 > INT16_MAX || width < 1 || width > UINT16_MAX ||
        height < 1 || height > UINT16_MAX) return 0;
    state.gx0 = (int16_t)x0;
    state.gy0 = (int16_t)y0;
    state.gw = (uint16_t)width;
    state.gh = (uint16_t)height;
    pedparse_ide_entry *peds = NULL;
    pedparse_stat_entry *names = NULL;
    int ok = get_peds(options, &peds, &state.model_count) &&
             get_stats(options, &state, &names) &&
             get_anims(options, &state, peds) &&
             build_models(&state, peds, names) &&
             get_groups(options, &state, peds) &&
             get_population(options, &state) &&
             get_zones(options, &state) &&
             make_graph(options, &state, 0) &&
             make_graph(options, &state, 1);
    if (ok) {
        pedtable_input tables = {
            state.models, state.model_count,
            state.stats, state.stat_count,
            state.anims, state.anim_count,
            state.groups, 57, state.cars, state.car_count,
            state.population, state.pop_count,
            state.zones, state.zone_count
        };
        ok = pedtables_encode(&tables, &state.tables,
                              &state.tables_length, state.table_offsets);
    }
    if (ok) {
        pedpack_input input = {0};
        input.models = (uint16_t)state.model_count;
        input.stats = (uint16_t)state.stat_count;
        input.anims = (uint16_t)state.anim_count;
        input.groups = 57;
        input.zones = (uint16_t)state.zone_count;
        input.nodes = (uint32_t)(state.path_nodes_length / 16);
        input.links = state.path_link_count;
        input.road_nodes = (uint32_t)(state.road_nodes_length / 16);
        input.road_links = state.road_link_count;
        input.gx0 = state.gx0;
        input.gy0 = state.gy0;
        input.gw = state.gw;
        input.gh = state.gh;
        input.cell = 50;
        for (size_t i = 0; i < 7; ++i) {
            input.parts[i] = state.tables + state.table_offsets[i];
            input.lengths[i] = state.table_offsets[i + 1] - state.table_offsets[i];
        }
        input.parts[7] = state.path_cells;
        input.lengths[7] = state.path_cells_length;
        input.parts[8] = state.path_nodes;
        input.lengths[8] = state.path_nodes_length;
        input.parts[9] = state.path_links;
        input.lengths[9] = state.path_links_length;
        input.parts[10] = state.road_nodes;
        input.lengths[10] = state.road_nodes_length;
        input.parts[11] = state.road_links;
        input.lengths[11] = state.road_links_length;
        input.parts[12] = state.tables + state.table_offsets[7];
        input.lengths[12] = state.table_offsets[8] - state.table_offsets[7];
        input.parts[13] = state.road_cells;
        input.lengths[13] = state.road_cells_length;
        ok = pedpack_encode(&input, output, length);
    }
    if (ok) {
        uint32_t nonempty = 0;
        for (size_t i = 0; i < 57; ++i)
            if (state.groups[i].count) ++nonempty;
        *stats = (pedbuild_stats){
            .models = (uint32_t)state.model_count,
            .stats = (uint32_t)state.stat_count,
            .anims = (uint32_t)state.anim_count,
            .groups = nonempty,
            .zones = (uint32_t)state.zone_count,
            .nodes = (uint32_t)(state.path_nodes_length / 16),
            .links = state.path_link_count,
            .road_nodes = (uint32_t)(state.road_nodes_length / 16),
            .road_links = state.road_link_count,
            .script_start = state.script_start,
            .script_end = state.script_end,
            .grid_width = state.gw,
            .grid_height = state.gh
        };
        stats->scripted = state.scripted;
    }
    free(peds);
    free(names);
    release(&state);
    return ok;
}
