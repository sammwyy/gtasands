#include "pedtables.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static int quant(double value, int scale, uint8_t *out) {
    value = trunc(value * scale);
    if (!isfinite(value) || value < 0) return 0;
    *out = value > 255 ? 255 : (uint8_t)value;
    return 1;
}

static void groups(uint8_t *out, const pedtable_group *groups, size_t count) {
    size_t members = 0;
    for (size_t i = 0; i < count; ++i) {
        put16(out + i * 4, (uint16_t)members);
        put16(out + i * 4 + 2, (uint16_t)groups[i].count);
        for (size_t j = 0; j < groups[i].count; ++j)
            put16(out + count * 4 + (members + j) * 2, groups[i].members[j]);
        members += groups[i].count;
    }
}

int pedtables_encode(const pedtable_input *p, uint8_t **output, size_t *length,
                     size_t offsets[9]) {
    if (!p || !output || !length || !offsets ||
        (p->model_count && !p->models) || (p->stat_count && !p->stats) ||
        (p->anim_count && !p->anims) || (p->group_count && !p->groups) ||
        (p->car_count && !p->cars) || (p->pop_count && !p->popcycle) ||
        (p->zone_count && !p->zones)) return 0;
    size_t groups_count = 0, cars_count = 0;
    for (size_t i = 0; i < p->group_count; ++i) {
        if (p->groups[i].count && !p->groups[i].members) return 0;
        if (p->groups[i].count > UINT16_MAX - groups_count) return 0;
        groups_count += p->groups[i].count;
    }
    for (size_t i = 0; i < p->car_count; ++i) {
        if (p->cars[i].count && !p->cars[i].members) return 0;
        if (p->cars[i].count > UINT16_MAX - cars_count) return 0;
        cars_count += p->cars[i].count;
    }
    uint64_t sizes[8] = {p->model_count * 12u, p->model_count * 48u,
                         p->stat_count * 12u, p->anim_count * 160u,
                         p->group_count * 4u + groups_count * 2u,
                         p->pop_count * 24u, p->zone_count * 24u,
                         p->car_count * 4u + cars_count * 2u};
    sizes[4] = (sizes[4] + 3) & ~(uint64_t)3;
    uint64_t total = 0;
    for (size_t i = 0; i < 8; ++i) {
        offsets[i] = (size_t)total;
        total += sizes[i];
        if (total > SIZE_MAX) return 0;
    }
    offsets[8] = (size_t)total;
    uint8_t *out = calloc((size_t)total ? (size_t)total : 1, 1);
    if (!out) return 0;
    int ok = 1;
    for (size_t i = 0; i < p->model_count; ++i) {
        const pedtable_model *m = &p->models[i];
        uint8_t *rec = out + offsets[0] + i * 12;
        put16(rec, m->id);
        rec[2] = m->type;
        rec[3] = m->stat;
        rec[4] = m->anim;
        rec[5] = m->race;
        rec[6] = m->flags;
        put16(rec + 8, m->cars);
        memcpy(out + offsets[1] + i * 48, m->model, 24);
        memcpy(out + offsets[1] + i * 48 + 24, m->txd, 24);
    }
    for (size_t i = 0; i < p->stat_count && ok; ++i) {
        uint8_t *rec = out + offsets[2] + i * 12;
        const double *v = p->stats[i].value;
        ok = quant(v[0], 1, rec) && quant(v[1], 10, rec + 1);
        for (size_t j = 2; j < 6 && ok; ++j)
            ok = quant(v[j], 1, rec + j);
        if (ok) ok = quant(v[6], 20, rec + 6) &&
                     quant(v[7], 20, rec + 7) &&
                     quant(v[8], 1, rec + 8) &&
                     quant(v[9], 1, rec + 9);
    }
    for (size_t i = 0; i < p->anim_count; ++i) {
        memcpy(out + offsets[3] + i * 160, p->anims[i].name, 16);
        memcpy(out + offsets[3] + i * 160 + 16, p->anims[i].clips, 144);
    }
    groups(out + offsets[4], p->groups, p->group_count);
    for (size_t i = 0; i < p->pop_count * 24; ++i) {
        int32_t value = p->popcycle[i];
        out[offsets[5] + i] = value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
    }
    for (size_t i = 0; i < p->zone_count; ++i) {
        const pedtable_zone *z = &p->zones[i];
        uint8_t *rec = out + offsets[6] + i * 24;
        for (size_t j = 0; j < 4; ++j)
            put16(rec + j * 2, (uint16_t)z->bounds[j]);
        rec[8] = z->population;
        rec[9] = z->races;
        rec[10] = z->dealer;
        memcpy(rec + 12, z->gang, 10);
    }
    groups(out + offsets[7], p->cars, p->car_count);
    if (ok) {
        *output = out;
        *length = (size_t)total;
    } else free(out);
    return ok;
}
