#include "movie_palette.h"

#include <stddef.h>
#include <string.h>

enum { COLORS = 32768, PALETTE = 256, FRAME_WIDTH = 256, FRAME_HEIGHT = 192 };

typedef struct {
    size_t begin, end;
    uint64_t weight;
    uint8_t min[3], max[3], axis;
} palette_box;

static uint8_t component(uint16_t index, int channel) {
    return (uint8_t)((index >> (channel * 5)) & 31);
}

static void measure(palette_box *box, const uint16_t *indices,
                    const uint32_t *histogram) {
    box->weight = 0;
    for (int c = 0; c < 3; ++c) {
        box->min[c] = 31;
        box->max[c] = 0;
    }
    for (size_t i = box->begin; i < box->end; ++i) {
        uint16_t index = indices[i];
        box->weight += histogram[index];
        for (int c = 0; c < 3; ++c) {
            uint8_t value = component(index, c);
            if (value < box->min[c]) box->min[c] = value;
            if (value > box->max[c]) box->max[c] = value;
        }
    }
    box->axis = 0;
    for (int c = 1; c < 3; ++c)
        if (box->max[c] - box->min[c] >
            box->max[box->axis] - box->min[box->axis]) box->axis = (uint8_t)c;
}

static int split(palette_box *box, palette_box *next,
                 uint16_t *indices, uint16_t *scratch,
                 const uint32_t *histogram) {
    int axis = box->axis;
    if (box->end - box->begin < 2 || box->max[axis] == box->min[axis])
        return 0;
    size_t counts[32] = {0}, offsets[32];
    uint64_t weights[32] = {0}, accumulated = 0;
    for (size_t i = box->begin; i < box->end; ++i) {
        uint16_t index = indices[i];
        uint8_t value = component(index, axis);
        ++counts[value];
        weights[value] += histogram[index];
    }
    int boundary = box->min[axis] + 1;
    for (int c = box->min[axis]; c < box->max[axis]; ++c) {
        accumulated += weights[c];
        if (accumulated >= box->weight / 2) {
            boundary = c + 1;
            break;
        }
    }
    size_t cursor = box->begin;
    for (int c = 0; c < 32; ++c) {
        offsets[c] = cursor;
        cursor += counts[c];
    }
    for (size_t i = box->begin; i < box->end; ++i) {
        uint16_t index = indices[i];
        scratch[offsets[component(index, axis)]++] = index;
    }
    size_t divide = box->begin;
    for (int c = 0; c < boundary; ++c) divide += counts[c];
    if (divide <= box->begin || divide >= box->end) return 0;
    memcpy(indices + box->begin, scratch + box->begin,
           (box->end - box->begin) * sizeof *indices);
    *next = (palette_box){.begin = divide, .end = box->end};
    box->end = divide;
    measure(box, indices, histogram);
    measure(next, indices, histogram);
    return 1;
}

void movie_palette_build(const uint32_t histogram[32768],
                         uint8_t palette[768], uint8_t lookup[32768]) {
    uint16_t indices[COLORS], scratch[COLORS];
    palette_box boxes[PALETTE - 1];
    size_t color_count = 0, box_count = 0;
    memset(palette, 0, PALETTE * 3);
    for (uint32_t i = 0; i < COLORS; ++i)
        if (histogram[i]) indices[color_count++] = (uint16_t)i;
    if (color_count) {
        boxes[0] = (palette_box){.begin = 0, .end = color_count};
        measure(&boxes[0], indices, histogram);
        box_count = 1;
    }
    while (box_count < PALETTE - 1) {
        size_t selected = box_count;
        uint64_t best = 0;
        for (size_t i = 0; i < box_count; ++i) {
            palette_box *box = &boxes[i];
            int spread = box->max[box->axis] - box->min[box->axis];
            uint64_t score = box->weight * (uint64_t)spread;
            if (spread && score > best) {
                best = score;
                selected = i;
            }
        }
        if (selected == box_count ||
            !split(&boxes[selected], &boxes[box_count], indices, scratch,
                   histogram)) break;
        ++box_count;
    }
    for (size_t i = 0; i < box_count; ++i) {
        uint64_t totals[3] = {0};
        palette_box *box = &boxes[i];
        for (size_t j = box->begin; j < box->end; ++j) {
            uint16_t index = indices[j];
            for (int c = 0; c < 3; ++c)
                totals[c] += (uint64_t)(component(index, c) * 8 + 4) *
                             histogram[index];
        }
        for (int c = 0; c < 3; ++c)
            palette[(i + 1) * 3 + c] = (uint8_t)(totals[c] / box->weight);
    }
    for (uint32_t i = 0; i < COLORS; ++i) {
        int r = component((uint16_t)i, 0) * 8 + 4;
        int g = component((uint16_t)i, 1) * 8 + 4;
        int b = component((uint16_t)i, 2) * 8 + 4;
        uint32_t best = UINT32_MAX;
        uint8_t selected = 0;
        for (size_t p = 0; p <= box_count; ++p) {
            int dr = r - palette[p * 3];
            int dg = g - palette[p * 3 + 1];
            int db = b - palette[p * 3 + 2];
            uint32_t distance = (uint32_t)(30 * dr * dr + 59 * dg * dg +
                                            11 * db * db);
            if (distance < best) {
                best = distance;
                selected = (uint8_t)p;
            }
        }
        lookup[i] = selected;
    }
}

void movie_palette_frame(const uint8_t *rgb, uint8_t *pixels,
                         const uint8_t lookup[32768]) {
    static const int8_t bayer[64] = {
        -32, 16, -20, 28, -29, 19, -17, 31,
          0,-16, 12, -4,   3,-13, 15, -1,
        -24, 24, -28, 20, -21, 27, -25, 23,
          8, -8,  4,-12,  11, -5,  7, -9,
        -30, 18, -18, 30, -31, 17, -19, 29,
          2,-14, 14, -2,   1,-15, 13, -3,
        -22, 26, -26, 22, -23, 25, -27, 21,
         10, -6,  6,-10,   9, -7,  5,-11
    };
    memset(pixels, 0, FRAME_WIDTH * 256);
    for (int y = 0; y < FRAME_HEIGHT; ++y)
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            int dither = bayer[(y & 7) * 8 + (x & 7)] / 6;
            int color[3];
            for (int c = 0; c < 3; ++c) {
                int value = rgb[((size_t)y * FRAME_WIDTH + x) * 3 + c] +
                            dither;
                if (value < 0) value = 0;
                if (value > 255) value = 255;
                color[c] = value >> 3;
            }
            pixels[(y + 32) * FRAME_WIDTH + x] =
                lookup[color[0] | color[1] << 5 | color[2] << 10];
        }
}
