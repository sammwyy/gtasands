#include "points.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t kind;
} point;

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int rounded(float value) {
    int integer = (int)value;
    float fraction = value - integer;
    if (fraction > 0.5f || (fraction == 0.5f && integer % 2)) ++integer;
    if (fraction < -0.5f || (fraction == -0.5f && integer % 2)) --integer;
    return integer;
}

static int compare_points(const void *a, const void *b) {
    const point *x = a, *y = b;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return x->kind < y->kind ? -1 : x->kind > y->kind;
}

static int starts(const uint8_t *label, const char *prefix) {
    for (size_t i = 0; prefix[i]; ++i) {
        unsigned c = label[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (unsigned char)prefix[i]) return 0;
    }
    return 1;
}

int points_from_denx(const uint8_t *input, size_t size, uint8_t **output, size_t *length) {
    if (!input || !output || !length || size < 8 || memcmp(input, "DENX", 4) ||
        u16(input + 4) != 3)
        return 0;
    size_t count = u16(input + 6);
    if (count > (size - 8) / 64) return 0;
    point *points = count ? malloc(count * sizeof *points) : NULL;
    if (count && !points) return 0;
    static const char *prefixes[] = {
        "AMMUN", "BARBER", "FDBURG", "FDCHICK", "FDPIZA", "FDREST",
        "DINER", "GYM", "CLOTH", "POLICE", "TATTO", "SV", "CARMOD"
    };
    static const uint8_t types[] = {0, 1, 2, 3, 4, 5, 5, 6, 7, 8, 9, 10, 11};
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *record = input + 8 + i * 64;
        if (u16(record + 60)) continue;
        float x, y;
        memcpy(&x, record, sizeof x);
        memcpy(&y, record + 4, sizeof y);
        if (!(x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767))
            continue;
        int px = rounded(x), py = rounded(y);
        if (px < -3000 || px >= 3000 || py < -3000 || py >= 3000) continue;
        uint8_t kind = 12;
        for (size_t j = 0; j < sizeof types; ++j)
            if (starts(record + 48, prefixes[j])) {
                kind = types[j];
                break;
            }
        points[used++] = (point){(int16_t)px, (int16_t)py, kind};
    }
    qsort(points, used, sizeof *points, compare_points);
    size_t unique = 0;
    for (size_t i = 0; i < used; ++i)
        if (!unique || compare_points(&points[i], &points[unique - 1]))
            points[unique++] = points[i];
    uint8_t *out = malloc(8 + unique * 6);
    if (out) {
        memcpy(out, "DPOI", 4);
        put16(out + 4, 2);
        put16(out + 6, (uint16_t)unique);
        for (size_t i = 0; i < unique; ++i) {
            uint8_t *dst = out + 8 + i * 6;
            put16(dst, (uint16_t)points[i].x);
            put16(dst + 2, (uint16_t)points[i].y);
            dst[4] = points[i].kind;
            dst[5] = 0;
        }
        *output = out;
        *length = 8 + unique * 6;
    }
    free(points);
    return out != NULL;
}
