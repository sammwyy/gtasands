#include "worldpack.h"

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

static double nearest(double value) {
    double whole;
    double fraction = modf(value, &whole);
    if (fraction > 0.5 || (fraction == 0.5 && fmod(whole, 2))) return whole + 1;
    if (fraction < -0.5 || (fraction == -0.5 && fmod(whole, 2))) return whole - 1;
    return whole;
}

static int32_t fx(double value) {
    value *= 4096;
    if (!isfinite(value)) return 0;
    if (value <= INT32_MIN) return INT32_MIN;
    if (value >= INT32_MAX) return INT32_MAX;
    return (int32_t)nearest(value);
}

static int16_t fx16(double value) {
    value *= 4096;
    if (!isfinite(value)) return 0;
    if (value <= INT16_MIN) return INT16_MIN;
    if (value >= INT16_MAX) return INT16_MAX;
    return (int16_t)nearest(value);
}

int worldpack_encode(const worldpack_input *input, uint8_t **output, size_t *length) {
    if (!input || !output || !length || !input->sector ||
        !input->grid_width || !input->grid_height ||
        (input->model_count && (!input->models || !input->collisions)) ||
        (input->txd_count && !input->txds) ||
        !input->cell_counts || (input->instance_count && !input->instances) ||
        input->model_count > UINT16_MAX || input->txd_count > UINT16_MAX ||
        input->instance_count > UINT32_MAX) return 0;
    size_t cells = (size_t)input->grid_width * input->grid_height;
    size_t total_instances = 0, colbytes = 0;
    for (size_t i = 0; i < cells; ++i) total_instances += input->cell_counts[i];
    if (total_instances != input->instance_count) return 0;
    for (size_t i = 0; i < input->model_count; ++i) {
        size_t n = input->collisions[i].length;
        if (n && !input->collisions[i].record) return 0;
        if (n > UINT32_MAX || n > SIZE_MAX - colbytes) return 0;
        colbytes += n;
    }
    uint64_t model_offset = 40;
    uint64_t txd_offset = model_offset + input->model_count * 36u;
    uint64_t sectors_offset = txd_offset + input->txd_count * 28u;
    uint64_t instances_offset = sectors_offset + cells * 8u;
    uint64_t collision_offset = instances_offset + input->instance_count * 40u;
    uint64_t data_offset = collision_offset + input->model_count * 12u;
    uint64_t size = (data_offset + colbytes + 3u) & ~(uint64_t)3;
    if (size > UINT32_MAX) return 0;
    uint8_t *out = calloc((size_t)size, 1);
    if (!out) return 0;
    memcpy(out, "DWLD", 4);
    put16(out + 4, 6);
    put16(out + 6, input->sector);
    put16(out + 8, (uint16_t)input->ox);
    put16(out + 10, (uint16_t)input->oy);
    put16(out + 12, input->grid_width);
    put16(out + 14, input->grid_height);
    put16(out + 16, (uint16_t)input->model_count);
    put16(out + 18, (uint16_t)input->txd_count);
    put32(out + 20, (uint32_t)input->instance_count);
    put32(out + 24, (uint32_t)model_offset);
    put32(out + 28, (uint32_t)sectors_offset);
    put32(out + 32, (uint32_t)instances_offset);
    put32(out + 36, (uint32_t)collision_offset);
    for (size_t i = 0; i < input->model_count; ++i) {
        const worldpack_model *m = &input->models[i];
        uint8_t *p = out + model_offset + i * 36;
        memcpy(p, m->name, 24);
        put16(p + 24, m->txd);
        put16(p + 26, m->draw_distance);
        p[28] = m->time_on;
        p[29] = m->time_off;
        put16(p + 30, m->radius);
        put32(p + 32, m->flags);
    }
    for (size_t i = 0; i < input->txd_count; ++i) {
        const worldpack_txd *t = &input->txds[i];
        uint8_t *p = out + txd_offset + i * 28;
        memcpy(p, t->name, 24);
        put16(p + 24, (uint16_t)t->parent);
        put16(p + 26, t->pack);
    }
    uint32_t first = 0;
    for (size_t i = 0; i < cells; ++i) {
        uint8_t *p = out + sectors_offset + i * 8;
        put32(p, first);
        put16(p + 4, input->cell_counts[i]);
        first += input->cell_counts[i];
    }
    for (size_t i = 0; i < input->instance_count; ++i) {
        const worldpack_instance *v = &input->instances[i];
        uint8_t *p = out + instances_offset + i * 40;
        for (unsigned j = 0; j < 3; ++j)
            put32(p + j * 4, (uint32_t)fx(v->pos[j]));
        for (unsigned j = 0; j < 9; ++j)
            put16(p + 12 + j * 2, (uint16_t)fx16(v->rot[j]));
        put16(p + 30, v->model);
        put16(p + 32, v->switch_flags);
        put16(p + 34, v->linked);
        put16(p + 36, v->area);
    }
    uint64_t offset = data_offset;
    for (size_t i = 0; i < input->model_count; ++i) {
        const worldpack_collision *c = &input->collisions[i];
        uint8_t *p = out + collision_offset + i * 12;
        if (!c->length) continue;
        put32(p, (uint32_t)offset);
        put32(p + 4, (uint32_t)c->length);
        put16(p + 8, c->radius);
        memcpy(out + offset, c->record, c->length);
        offset += c->length;
    }
    *output = out;
    *length = (size_t)size;
    return 1;
}
