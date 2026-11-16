#include "geometry.h"

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

static double nearest_even(double value) {
    double whole;
    double part = modf(value, &whole);
    if (part > 0.5 || (part == 0.5 && fmod(whole, 2) != 0)) return whole + 1;
    if (part < -0.5 || (part == -0.5 && fmod(whole, 2) != 0)) return whole - 1;
    return whole;
}

static int16_t fixed16(double value) {
    if (isnan(value) || isinf(value)) return 0;
    if (value <= -32768) return -32768;
    if (value >= 32767) return 32767;
    return (int16_t)nearest_even(value);
}

static int32_t fixed(double value) {
    if (isnan(value) || isinf(value)) return 0;
    value *= 4096;
    if (value <= -2147483648.0) return INT32_MIN;
    if (value >= 2147483647.0) return INT32_MAX;
    return (int32_t)nearest_even(value);
}

static int exponent(double max) {
    int exponent = max > 0 ? (int)ceil(log2(fmax(max, 1e-6) * 4096 / 32767)) : -4;
    if (exponent < -4) exponent = -4;
    if (exponent > 12) exponent = 12;
    return exponent;
}

static size_t align4(size_t n) {
    return (n + 3) & ~(size_t)3;
}

int geometry_encode(const double *positions, const double *uvs, const double *normals,
                    const uint8_t *colors, const uint8_t *bones,
                    const uint16_t *indices, size_t vertex_count, size_t index_count,
                    const geometry_material *materials, size_t material_count,
                    uint8_t **output, size_t *length) {
    if (!output || !length || (vertex_count && !positions) ||
        (index_count && !indices) || (material_count && !materials) ||
        vertex_count > UINT16_MAX || material_count > UINT16_MAX || index_count > UINT32_MAX)
        return 0;
    uint64_t size = 24u + material_count * 44u + align4(vertex_count * 6u) +
                    (uvs ? vertex_count * 4u : 0) + (normals ? vertex_count * 4u : 0) +
                    (colors ? align4(vertex_count * 2u) : 0) +
                    (bones ? align4(vertex_count) : 0) + align4(index_count * 2u);
    if (size > UINT32_MAX) return 0;
    uint8_t *out = calloc((size_t)size, 1);
    if (!out) return 0;
    double lo[3] = {0}, hi[3] = {0}, center[3];
    if (vertex_count) {
        for (size_t j = 0; j < 3; ++j) lo[j] = hi[j] = positions[j];
        for (size_t i = 1; i < vertex_count; ++i)
            for (size_t j = 0; j < 3; ++j) {
                double v = positions[i * 3 + j];
                if (v < lo[j]) lo[j] = v;
                if (v > hi[j]) hi[j] = v;
            }
    }
    double magnitude = 0;
    for (size_t j = 0; j < 3; ++j)
        center[j] = (lo[j] + hi[j]) / 2;
    for (size_t i = 0; i < vertex_count; ++i)
        for (size_t j = 0; j < 3; ++j) {
            double value = fabs(positions[i * 3 + j] - center[j]);
            if (value > magnitude) magnitude = value;
        }
    int pexp = exponent(magnitude);
    int uexp = 0;
    if (uvs) {
        double maximum = 0;
        for (size_t i = 0; i < vertex_count * 2; ++i) {
            double value = fabs(uvs[i]);
            if (value > maximum) maximum = value;
        }
        uexp = exponent(maximum);
        if (uexp < 0) uexp = 0;
    }
    double ps = ldexp(4096.0, -pexp), us = ldexp(4096.0, -uexp);
    put16(out, (uint16_t)vertex_count);
    put16(out + 2, (uint16_t)material_count);
    put32(out + 4, (uint32_t)index_count);
    out[8] = (uint8_t)((uvs ? 1 : 0) | (normals ? 2 : 0) | (colors ? 4 : 0) | (bones ? 8 : 0));
    out[9] = (uint8_t)pexp;
    out[10] = (uint8_t)uexp;
    for (size_t j = 0; j < 3; ++j) put32(out + 12 + j * 4, (uint32_t)fixed(center[j]));
    size_t cursor = 24;
    for (size_t i = 0; i < material_count; ++i) {
        const geometry_material *m = &materials[i];
        memcpy(out + cursor, m->texture, 32);
        put16(out + cursor + 32, m->color);
        out[cursor + 34] = m->alpha;
        put32(out + cursor + 36, m->first);
        put32(out + cursor + 40, m->count);
        cursor += 44;
    }
    for (size_t i = 0; i < vertex_count; ++i)
        for (size_t j = 0; j < 3; ++j)
            put16(out + cursor + i * 6 + j * 2,
                  (uint16_t)fixed16(nearest_even((positions[i * 3 + j] - center[j]) * ps)));
    cursor += align4(vertex_count * 6);
    if (uvs) {
        for (size_t i = 0; i < vertex_count * 2; ++i)
            put16(out + cursor + i * 2, (uint16_t)fixed16(nearest_even(uvs[i] * us)));
        cursor += vertex_count * 4;
    }
    if (normals) {
        for (size_t i = 0; i < vertex_count; ++i) {
            uint32_t packed = 0;
            for (unsigned j = 0; j < 3; ++j) {
                double value = normals[i * 3 + j] * 512;
                if (value < -512) value = -512;
                if (value > 511) value = 511;
                packed |= ((uint32_t)(int)value & 0x3ffu) << (j * 10);
            }
            put32(out + cursor + i * 4, packed);
        }
        cursor += vertex_count * 4;
    }
    if (colors) {
        for (size_t i = 0; i < vertex_count; ++i) {
            const uint8_t *c = colors + i * 4;
            uint16_t rgb = (uint16_t)((c[0] >> 3) | (c[1] >> 3) << 5 | (c[2] >> 3) << 10);
            put16(out + cursor + i * 2, rgb);
        }
        cursor += align4(vertex_count * 2);
    }
    if (bones) {
        memcpy(out + cursor, bones, vertex_count);
        cursor += align4(vertex_count);
    }
    for (size_t i = 0; i < index_count; ++i)
        put16(out + cursor + i * 2, indices[i]);
    *output = out;
    *length = (size_t)size;
    return 1;
}
