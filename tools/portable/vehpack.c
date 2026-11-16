#include "vehpack.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static void put_float(uint8_t *p, double value) {
    float converted = (float)value;
    uint32_t bits;
    memcpy(&bits, &converted, sizeof bits);
    put32(p, bits);
}

static double round_even(double value) {
    double whole;
    double fraction = modf(value, &whole);
    if (fraction > 0.5 || (fraction == 0.5 && fmod(whole, 2))) return whole + 1;
    if (fraction < -0.5 || (fraction == -0.5 && fmod(whole, 2))) return whole - 1;
    return whole;
}

static int put_integer(uint8_t *p, double value) {
    double rounded = round_even(value);
    if (!isfinite(rounded) || rounded < INT16_MIN || rounded > INT16_MAX)
        return 0;
    put16(p, (uint16_t)(int16_t)rounded);
    return 1;
}

int vehpack_encode(const vehpack_input *input, uint8_t **output, size_t *length) {
    if (!input || !output || !length ||
        (input->model_count && !input->models) ||
        (input->color_count && !input->colors) ||
        (input->surface_count && !input->surfaces) ||
        (input->water_count && !input->water) ||
        (input->track_count && !input->tracks) ||
        !input->adhesion || input->model_count > UINT16_MAX ||
        input->color_count > UINT16_MAX || input->surface_count > UINT16_MAX ||
        input->water_count > UINT16_MAX || input->track_count > UINT16_MAX)
        return 0;
    uint64_t nodes = 0;
    for (size_t i = 0; i < input->track_count; ++i) {
        if (input->tracks[i].count > UINT16_MAX ||
            (input->tracks[i].count && !input->tracks[i].nodes) ||
            nodes + input->tracks[i].count > UINT16_MAX) return 0;
        nodes += input->tracks[i].count;
    }
    uint64_t vehicle_offset = 184;
    uint64_t color_offset = vehicle_offset + input->model_count * 380u;
    uint64_t surface_offset = color_offset + input->color_count * 4u;
    uint64_t water_offset = surface_offset + input->surface_count * 4u;
    uint64_t track_offset = water_offset + input->water_count * 28u;
    uint64_t total = track_offset + input->track_count * 8u + nodes * 8u;
    if (total > UINT32_MAX || total > SIZE_MAX) return 0;
    uint8_t *out = calloc((size_t)total, 1);
    if (!out) return 0;
    memcpy(out, "DVEH", 4);
    put16(out + 4, 2);
    put16(out + 6, (uint16_t)input->model_count);
    put16(out + 8, (uint16_t)input->color_count);
    put16(out + 10, (uint16_t)input->surface_count);
    put32(out + 12, (uint32_t)vehicle_offset);
    put32(out + 16, (uint32_t)color_offset);
    put32(out + 20, (uint32_t)surface_offset);
    for (size_t i = 0; i < 36; ++i)
        put_float(out + 24 + i * 4, input->adhesion[i]);
    put32(out + 168, (uint32_t)water_offset);
    put16(out + 172, (uint16_t)input->water_count);
    put16(out + 174, (uint16_t)input->track_count);
    put32(out + 176, (uint32_t)track_offset);
    put32(out + 180, (uint32_t)nodes);
    for (size_t i = 0; i < input->model_count; ++i) {
        const vehpack_model *m = &input->models[i];
        uint8_t *p = out + vehicle_offset + i * 380;
        memcpy(p, m->model, 24);
        memcpy(p + 24, m->txd, 24);
        memcpy(p + 48, m->gxt, 8);
        memcpy(p + 56, m->anims, 16);
        put16(p + 72, m->id);
        p[74] = m->type;
        p[75] = m->color_count;
        put32(p + 76, m->flags);
        memcpy(p + 80, m->palette, 16);
        put_float(p + 96, m->wheel_front);
        put_float(p + 100, m->wheel_rear);
        for (size_t j = 0; j < 14; ++j)
            put_float(p + 104 + j * 4, m->handling[j]);
        p[160] = m->gears;
        p[161] = m->drive;
        p[162] = m->engine;
        p[163] = m->abs;
        for (size_t j = 14; j < 26; ++j)
            put_float(p + 164 + (j - 14) * 4, m->handling[j]);
        put32(p + 212, m->model_flags);
        put32(p + 216, m->handling_flags);
        put32(p + 220, m->value);
        p[224] = m->front_lights;
        p[225] = m->rear_lights;
        p[226] = m->anim_group;
        p[227] = m->submerged;
        p[228] = m->special_kind;
        p[229] = m->has_fly;
        put_float(p + 232, m->steer);
        for (size_t j = 0; j < 15; ++j)
            put_float(p + 236 + j * 4, m->special[j]);
        for (size_t j = 0; j < 21; ++j)
            put_float(p + 296 + j * 4, m->flying[j]);
    }
    if (input->color_count)
        memcpy(out + color_offset, input->colors, input->color_count * 4);
    if (input->surface_count)
        memcpy(out + surface_offset, input->surfaces, input->surface_count * 4);
    int ok = 1;
    for (size_t i = 0; i < input->water_count && ok; ++i) {
        const vehpack_water *w = &input->water[i];
        if (w->count != 3 && w->count != 4) {
            ok = 0;
            break;
        }
        uint8_t *p = out + water_offset + i * 28;
        for (size_t j = 0; j < 4 && ok; ++j) {
            const vehpack_vec3 *v = &w->vertices[j < w->count ? j : (size_t)w->count - 1];
            ok = put_integer(p + j * 2, v->x) &&
                 put_integer(p + 8 + j * 2, v->y);
            double z = round_even(v->z * 100.0);
            if (z < INT16_MIN) z = INT16_MIN;
            if (z > INT16_MAX) z = INT16_MAX;
            if (ok) put16(p + 16 + j * 2, (uint16_t)(int16_t)z);
        }
        p[24] = w->count;
        p[25] = w->flags;
    }
    uint16_t first = 0;
    size_t at = (size_t)track_offset + input->track_count * 8;
    for (size_t i = 0; i < input->track_count && ok; ++i) {
        const vehpack_track *t = &input->tracks[i];
        uint8_t *header = out + track_offset + i * 8;
        put16(header, first);
        put16(header + 2, (uint16_t)t->count);
        double distance = 0;
        for (size_t j = 0; j < t->count && ok; ++j) {
            const vehpack_vec3 *v = &t->nodes[j];
            if (j) {
                const vehpack_vec3 *prev = &t->nodes[j - 1];
                double x = v->x - prev->x, y = v->y - prev->y, z = v->z - prev->z;
                distance += sqrt(x * x + y * y + z * z);
            }
            ok = put_integer(out + at, v->x * 8) &&
                 put_integer(out + at + 2, v->y * 8) &&
                 put_integer(out + at + 4, v->z * 8);
            double step = distance * 2;
            if (step < 0 || step > UINT16_MAX) step = UINT16_MAX;
            if (ok) put16(out + at + 6, (uint16_t)step);
            at += 8;
        }
        if (t->count) {
            const vehpack_vec3 *a = &t->nodes[0], *b = &t->nodes[t->count - 1];
            double x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
            distance += sqrt(x * x + y * y + z * z);
        }
        put_float(header + 4, distance);
        first = (uint16_t)(first + t->count);
    }
    if (ok) {
        *output = out;
        *length = (size_t)total;
    } else free(out);
    return ok;
}

int vehpack_convert_handling(vehpack_handling *h) {
    if (!h || !h->mass) return 0;
    h->engine_accel *= 1.0 / (50.0 * 50.0);
    h->max_vel *= 0.277778 / 50.0;
    h->brake_decel *= 1.0 / (50.0 * 50.0);
    h->damage_mult *= 2000.0 / h->mass;
    double limit = h->engine_accel / 6.0;
    double v = h->max_vel;
    while (v > 0.0) {
        v -= 0.01;
        if (h->drag >= 0.01) {
            if (v * v * (h->drag / 2.0) / 1000.0 <= limit) break;
            continue;
        }
        if ((1.0 / (v * v * h->drag + 1.0) - 1.0) * -v <= limit) break;
    }
    if (h->handling_flags & 0x01000000u) {
        h->max_flat = v / 1.2;
        h->max_reverse = fmin(-h->max_flat / 4.0, -0.2);
    } else {
        h->max_vel = v * 1.2;
        h->max_flat = v;
        h->max_reverse = fmin(-v * 0.3, -0.2);
    }
    h->engine_accel /= h->drive == '4' ? 4.0 : 2.0;
    return 1;
}
