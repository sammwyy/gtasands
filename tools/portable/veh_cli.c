#include "vehpack.h"
#include "vehparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint8_t *data = malloc(size ? (size_t)size : 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length = (size_t)size;
    return data;
}

static void name(char *out, size_t length, const char *value) {
    size_t size = strlen(value);
    if (size >= length) size = length - 1;
    memcpy(out, value, size);
}

static const vehparse_handling_entry *handling_for(
    const vehparse_handling_entry *items, size_t count, const char *name) {
    for (size_t i = count; i; --i)
        if (!strcmp(items[i - 1].name, name)) return &items[i - 1];
    return NULL;
}

static const vehparse_special_entry *special_for(
    const vehparse_special_entry *items, size_t count, const char *name, int kind) {
    for (size_t i = count; i; --i)
        if (!strcmp(items[i - 1].name, name) && items[i - 1].kind == kind)
            return &items[i - 1];
    return NULL;
}

static const vehparse_car_color *colors_for(
    const vehparse_car_color *items, size_t count, const char *name) {
    for (size_t i = count; i; --i)
        if (!strcmp(items[i - 1].name, name)) return &items[i - 1];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 12) {
        fprintf(stderr, "usage: veh_cli VEHICLES.IDE HANDLING.CFG CARCOLS.DAT SURFACE.DAT SURFINFO.DAT WATER.DAT TRACKS.DAT TRACKS2.DAT TRACKS3.DAT TRACKS4.DAT OUTPUT.DVEH\n");
        return 2;
    }
    uint8_t *sources[10] = {0};
    size_t sizes[10] = {0};
    for (size_t i = 0; i < 10; ++i) {
        if (i >= 5 && !strcmp(argv[i + 1], "-")) {
            sources[i] = calloc(1, 1);
            continue;
        }
        sources[i] = read_file(argv[i + 1], &sizes[i]);
    }
    int ok = 1;
    for (size_t i = 0; i < 10; ++i)
        if (!sources[i]) ok = 0;
    vehparse_ide_entry *ide = NULL;
    vehparse_handling_entry *handling = NULL;
    vehparse_special_entry *special = NULL;
    vehparse_color *colors = NULL;
    vehparse_car_color *cars = NULL;
    vehparse_surface *surfaces = NULL;
    vehparse_water_entry *water = NULL;
    vehpack_vec3 *track_nodes[4] = {0};
    size_t nide = 0, nhandling = 0, nspecial = 0, ncolors = 0, ncars = 0;
    size_t nsurfaces = 0, nwater = 0, ntracks[4] = {0};
    double adhesion[36];
    if (ok) ok = vehparse_ide((char *)sources[0], sizes[0], &ide, &nide) &&
                 vehparse_handling((char *)sources[1], sizes[1], &handling, &nhandling) &&
                 vehparse_special((char *)sources[1], sizes[1], &special, &nspecial) &&
                 vehparse_carcols((char *)sources[2], sizes[2],
                                   &colors, &ncolors, &cars, &ncars) &&
                 vehparse_surfaces((char *)sources[3], sizes[3],
                                    (char *)sources[4], sizes[4],
                                    adhesion, &surfaces, &nsurfaces) &&
                 vehparse_water((char *)sources[5], sizes[5], &water, &nwater);
    for (size_t i = 0; i < 4 && ok; ++i)
        ok = vehparse_track((char *)sources[6 + i], sizes[6 + i],
                            &track_nodes[i], &ntracks[i]);
    vehpack_model *models = ok && nide ? calloc(nide, sizeof *models) : NULL;
    vehpack_water *polygons = ok && nwater ? calloc(nwater, sizeof *polygons) : NULL;
    uint8_t *palette = ok && ncolors ? calloc(ncolors, 4) : NULL;
    uint8_t *surface_table = ok && nsurfaces ? calloc(nsurfaces, 4) : NULL;
    if ((nide && !models) || (nwater && !polygons) ||
        (ncolors && !palette) || (nsurfaces && !surface_table)) ok = 0;
    for (size_t i = 0; i < nide && ok; ++i) {
        const vehparse_ide_entry *v = &ide[i];
        const vehparse_handling_entry *h =
            handling_for(handling, nhandling, v->handling);
        if (!h) h = handling_for(handling, nhandling, "LANDSTAL");
        if (!h && nhandling) h = &handling[0];
        if (!h) {
            ok = 0;
            break;
        }
        vehpack_model *m = &models[i];
        name(m->model, 24, v->model);
        name(m->txd, 24, v->txd);
        name(m->gxt, 8, v->gxt);
        name(m->anims, 16, v->anims);
        m->id = (uint16_t)v->id;
        m->type = v->type;
        m->flags = v->flags;
        const vehparse_car_color *c = colors_for(cars, ncars, v->model);
        m->color_count = c ? c->count : 1;
        for (size_t j = 0; j < m->color_count; ++j) {
            m->palette[j * 2] = (uint8_t)(c ?
                (c->pairs[j][0] > 255 ? 255 : c->pairs[j][0]) : 1);
            m->palette[j * 2 + 1] = (uint8_t)(c ?
                (c->pairs[j][1] > 255 ? 255 : c->pairs[j][1]) : 1);
        }
        m->wheel_front = v->wheel_front;
        m->wheel_rear = v->wheel_rear;
        const double values[26] = {
            h->mass, h->turn_mass, h->drag, h->com[0], h->com[1], h->com[2],
            h->traction_mult, h->traction_loss, h->traction_bias,
            h->max_vel, h->max_flat, h->max_reverse, h->engine_accel,
            h->engine_inertia, h->brake_decel, h->brake_bias, h->steering_lock,
            h->suspension[0], h->suspension[1], h->suspension[2],
            h->suspension[3], h->suspension[4], h->suspension[5],
            h->suspension[6], h->seat_offset, h->damage_mult};
        memcpy(m->handling, values, sizeof values);
        m->gears = (uint8_t)h->gears;
        m->drive = h->drive;
        m->engine = h->engine;
        m->abs = (uint8_t)h->abs;
        m->model_flags = (uint32_t)h->model_flags;
        m->handling_flags = (uint32_t)h->handling_flags;
        m->value = h->value > 0 ? (uint32_t)h->value : 0;
        m->front_lights = (uint8_t)h->front_lights;
        m->rear_lights = (uint8_t)h->rear_lights;
        m->anim_group = (uint8_t)h->anim_group;
        m->submerged = (uint8_t)h->submerged;
        const vehparse_special_entry *ext = special_for(special, nspecial,
                                                        v->handling, 1);
        if (ext) m->special_kind = 1;
        else {
            ext = special_for(special, nspecial, v->handling, 2);
            if (ext) m->special_kind = 2;
        }
        if (ext) memcpy(m->special, ext->values,
                        (m->special_kind == 1 ? 15 : 14) * sizeof(double));
        const vehparse_special_entry *fly = special_for(special, nspecial,
                                                        v->handling, 3);
        if (fly) {
            m->has_fly = 1;
            memcpy(m->flying, fly->values, sizeof m->flying);
        }
        m->steer = v->steer;
    }
    for (size_t i = 0; i < ncolors && ok; ++i) {
        palette[i * 4] = (uint8_t)colors[i].r;
        palette[i * 4 + 1] = (uint8_t)colors[i].g;
        palette[i * 4 + 2] = (uint8_t)colors[i].b;
    }
    for (size_t i = 0; i < nsurfaces && ok; ++i) {
        surface_table[i * 4] = (uint8_t)surfaces[i].group;
        surface_table[i * 4 + 1] = (uint8_t)surfaces[i].wet;
        surface_table[i * 4 + 2] = (uint8_t)surfaces[i].skid;
    }
    for (size_t i = 0; i < nwater && ok; ++i) {
        memcpy(polygons[i].vertices, water[i].vertices, sizeof polygons[i].vertices);
        polygons[i].count = water[i].count;
        polygons[i].flags = (uint8_t)water[i].flags;
    }
    vehpack_track tracks[4];
    for (size_t i = 0; i < 4; ++i)
        tracks[i] = (vehpack_track){track_nodes[i], ntracks[i]};
    vehpack_input input = {models, nide, palette, ncolors, surface_table,
                           nsurfaces, adhesion, polygons, nwater, tracks, 4};
    uint8_t *output = NULL;
    size_t length = 0;
    if (ok) ok = vehpack_encode(&input, &output, &length);
    if (ok) {
        FILE *file = fopen(argv[11], "wbx");
        ok = file != NULL;
        if (ok) {
            ok = fwrite(output, 1, length, file) == length;
            if (fclose(file)) ok = 0;
            if (!ok) remove(argv[11]);
        }
    }
    free(output);
    free(models); free(polygons); free(palette); free(surface_table);
    free(ide); free(handling); free(special); free(colors); free(cars);
    free(surfaces); free(water);
    for (size_t i = 0; i < 4; ++i) free(track_nodes[i]);
    for (size_t i = 0; i < 10; ++i) free(sources[i]);
    if (!ok) fprintf(stderr, "failed to convert vehicles\n");
    return ok ? 0 : 1;
}
