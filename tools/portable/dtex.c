#include "dtex.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t color;
    uint32_t count;
} hist_color;

typedef struct {
    size_t *ids;
    size_t len;
    uint64_t score;
    uint32_t total;
    unsigned axis;
} box;

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *out, uint32_t value) {
    put16(out, (uint16_t)value);
    put16(out + 2, (uint16_t)(value >> 16));
}

static unsigned channel(uint16_t color, unsigned axis) {
    return (color >> (axis * 5)) & 31u;
}

static void evaluate(box *b, const hist_color *hist) {
    uint32_t total = 0;
    unsigned min[3] = {31, 31, 31}, max[3] = {0, 0, 0};
    for (size_t i = 0; i < b->len; ++i) {
        const hist_color *c = &hist[b->ids[i]];
        total += c->count;
        for (unsigned axis = 0; axis < 3; ++axis) {
            unsigned value = channel(c->color, axis);
            if (value < min[axis]) min[axis] = value;
            if (value > max[axis]) max[axis] = value;
        }
    }
    b->total = total;
    b->axis = 0;
    b->score = 0;
    if (b->len < 2) return;
    for (unsigned axis = 0; axis < 3; ++axis) {
        uint64_t score = (uint64_t)(max[axis] - min[axis]) * total;
        if (score > b->score) {
            b->score = score;
            b->axis = axis;
        }
    }
}

static uint32_t nearest(uint16_t color, const uint16_t *palette, size_t count) {
    uint32_t best = UINT32_MAX;
    size_t index = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t distance = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            int diff = (int)channel(color, axis) - (int)channel(palette[i], axis);
            distance += (uint32_t)(diff * diff);
        }
        if (distance < best) {
            best = distance;
            index = i;
        }
    }
    return (uint32_t)index;
}

static uint16_t rounded(uint64_t value, uint32_t divisor) {
    uint64_t quotient = value / divisor, remainder = value % divisor;
    return (uint16_t)(quotient + (remainder * 2 > divisor ||
                                  (remainder * 2 == divisor && (quotient & 1u))));
}

static int palette(const uint16_t *colors, const uint8_t *alpha, size_t count,
                   uint8_t threshold, int transparent, size_t limit,
                   uint16_t *result, size_t *length) {
    int32_t *lookup = malloc(32768 * sizeof *lookup);
    hist_color *hist = malloc(32768 * sizeof *hist);
    if (!lookup || !hist) {
        free(lookup);
        free(hist);
        return 0;
    }
    for (size_t i = 0; i < 32768; ++i) lookup[i] = -1;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (transparent && alpha[i] <= threshold) continue;
        uint16_t c = colors[i];
        if (lookup[c] < 0) {
            lookup[c] = (int32_t)used;
            hist[used++] = (hist_color){c, 0};
        }
        ++hist[lookup[c]].count;
    }
    if (!used) hist[used++] = (hist_color){0, 1};
    box boxes[256] = {{0}};
    boxes[0].ids = malloc(used * sizeof(size_t));
    if (!boxes[0].ids) {
        free(lookup);
        free(hist);
        return 0;
    }
    boxes[0].len = used;
    for (size_t i = 0; i < used; ++i) boxes[0].ids[i] = i;
    evaluate(&boxes[0], hist);
    size_t nboxes = 1;
    int ok = 1;
    while (nboxes < limit) {
        size_t selected = 0;
        for (size_t i = 1; i < nboxes; ++i)
            if (boxes[i].score > boxes[selected].score) selected = i;
        box *current = &boxes[selected];
        if (!current->score) break;
        size_t counts[32] = {0}, positions[32];
        for (size_t i = 0; i < current->len; ++i)
            ++counts[channel(hist[current->ids[i]].color, current->axis)];
        size_t at = 0;
        for (size_t i = 0; i < 32; ++i) {
            positions[i] = at;
            at += counts[i];
        }
        size_t *sorted = malloc(current->len * sizeof(size_t));
        if (!sorted) {
            ok = 0;
            break;
        }
        for (size_t i = 0; i < current->len; ++i) {
            size_t id = current->ids[i];
            sorted[positions[channel(hist[id].color, current->axis)]++] = id;
        }
        size_t cut = 1;
        uint32_t cumulative = 0;
        for (size_t i = 0; i < current->len; ++i) {
            cumulative += hist[sorted[i]].count;
            if ((uint64_t)cumulative * 2 >= current->total) {
                cut = i + 1 < current->len ? i + 1 : current->len - 1;
                break;
            }
        }
        size_t left_count = cut, right_count = current->len - cut;
        size_t *left = malloc(left_count * sizeof(size_t));
        size_t *right = malloc(right_count * sizeof(size_t));
        if (!left || !right) {
            free(sorted);
            free(left);
            free(right);
            ok = 0;
            break;
        }
        memcpy(left, sorted, left_count * sizeof(size_t));
        memcpy(right, sorted + cut, right_count * sizeof(size_t));
        free(sorted);
        free(current->ids);
        memmove(boxes + selected, boxes + selected + 1,
                (nboxes - selected - 1) * sizeof(box));
        boxes[nboxes - 1] = (box){left, left_count, 0, 0, 0};
        boxes[nboxes] = (box){right, right_count, 0, 0, 0};
        evaluate(&boxes[nboxes - 1], hist);
        evaluate(&boxes[nboxes], hist);
        ++nboxes;
    }
    if (ok) {
        for (size_t i = 0; i < nboxes; ++i) {
            uint16_t rgb[3];
            for (unsigned axis = 0; axis < 3; ++axis) {
                uint64_t sum = 0;
                for (size_t j = 0; j < boxes[i].len; ++j) {
                    const hist_color *c = &hist[boxes[i].ids[j]];
                    sum += (uint64_t)channel(c->color, axis) * c->count;
                }
                rgb[axis] = rounded(sum, boxes[i].total ? boxes[i].total : 1);
            }
            result[i] = rgb[0] | (rgb[1] << 5) | (rgb[2] << 10);
        }
        *length = nboxes;
    }
    for (size_t i = 0; i < nboxes; ++i) free(boxes[i].ids);
    free(lookup);
    free(hist);
    return ok;
}

int dtex_encode(const uint8_t *rgba, uint16_t width, uint16_t height,
                dtex_options options, uint8_t **output, size_t *length) {
    if (!rgba || !output || !length || !width || !height ||
        (options.format != DTEX_AUTO && options.format != DTEX_A3I5 &&
         options.format != DTEX_PAL4 && options.format != DTEX_PAL16 &&
         options.format != DTEX_PAL256 && options.format != DTEX_A5I3 &&
         options.format != DTEX_DIRECT))
        return 0;
    size_t n = (size_t)width * height;
    uint16_t *colors = malloc(n * sizeof *colors);
    uint8_t *alpha = malloc(n);
    if (!colors || !alpha) {
        free(colors);
        free(alpha);
        return 0;
    }
    uint8_t seen[32768] = {0};
    size_t unique = 0;
    int has_alpha = 0, binary = 1;
    uint8_t th = options.alpha_threshold;
    for (size_t i = 0; i < n; ++i) {
        unsigned r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        unsigned a = rgba[i * 4 + 3];
        if (options.additive) {
            unsigned maximum = r > g ? r : g;
            if (b > maximum) maximum = b;
            if (maximum) {
                r = r * 255 / maximum;
                g = g * 255 / maximum;
                b = b * 255 / maximum;
                a = maximum * a / 255;
            } else {
                r = g = b = a = 0;
            }
        }
        colors[i] = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
        if (!seen[colors[i]]) {
            seen[colors[i]] = 1;
            ++unique;
        }
        alpha[i] = options.opaque ? 255 : (uint8_t)a;
        if (alpha[i] < 255 - th) has_alpha = 1;
        if (alpha[i] > th && alpha[i] < 255 - th) binary = 0;
    }
    dtex_format format = options.format;
    if (format == DTEX_AUTO) {
        if (has_alpha && !binary) format = DTEX_A3I5;
        else if (unique + (size_t)has_alpha <= 4) format = DTEX_PAL4;
        else format = options.auto_colors > 16 ? DTEX_PAL256 : DTEX_PAL16;
    }
    size_t texels = format == DTEX_DIRECT ? n * 2 :
                    format == DTEX_PAL4 ? (n + 3) / 4 :
                    format == DTEX_PAL16 ? (n + 1) / 2 : n;
    size_t data_size = (texels + 3) & ~(size_t)3;
    uint8_t *out = calloc(16 + data_size + 512, 1);
    uint16_t pal[256] = {0};
    size_t pal_count = 0;
    uint8_t flags = has_alpha ? 2 : 0;
    if (!out) {
        free(colors);
        free(alpha);
        return 0;
    }
    uint8_t *tex = out + 16;
    int ok = 1;
    if (format == DTEX_DIRECT) {
        for (size_t i = 0; i < n; ++i)
            put16(tex + i * 2, colors[i] | (alpha[i] > th ? 0x8000u : 0u));
    } else {
        size_t max_colors = format == DTEX_A3I5 ? 32 : format == DTEX_A5I3 ? 8 :
                            format == DTEX_PAL4 ? 4 : format == DTEX_PAL16 ? 16 : 256;
        int transparent = format != DTEX_A3I5 && format != DTEX_A5I3 && has_alpha;
        size_t offset = (size_t)transparent;
        ok = palette(colors, alpha, n, th, format == DTEX_A3I5 || format == DTEX_A5I3 ||
                     transparent, max_colors - offset, pal + offset, &pal_count);
        pal_count += offset;
        if (transparent) flags |= 1;
        if (ok) {
            for (size_t i = 0; i < n; ++i) {
                uint32_t index = (transparent && alpha[i] <= th) ? 0 :
                                 nearest(colors[i], pal + offset, pal_count - offset) + (uint32_t)offset;
                if (format == DTEX_A3I5 || format == DTEX_A5I3) {
                    unsigned bits = format == DTEX_A3I5 ? 3 : 5;
                    tex[i] = alpha[i] > th ? (uint8_t)(index | (alpha[i] >> (8 - bits)) << (8 - bits)) : 0;
                } else if (format == DTEX_PAL256) tex[i] = (uint8_t)index;
                else if (format == DTEX_PAL16) tex[i / 2] |= (uint8_t)(index << ((i % 2) * 4));
                else tex[i / 4] |= (uint8_t)(index << ((i % 4) * 2));
            }
        }
    }
    if (ok) {
        memcpy(out, "DTEX", 4);
        put16(out + 4, width);
        put16(out + 6, height);
        out[8] = (uint8_t)format;
        out[9] = flags;
        put16(out + 10, (uint16_t)pal_count);
        put32(out + 12, (uint32_t)texels);
        for (size_t i = 0; i < pal_count; ++i)
            put16(out + 16 + data_size + 2 * i, pal[i]);
        *length = 16 + data_size + pal_count * 2;
        *output = out;
    } else free(out);
    free(colors);
    free(alpha);
    return ok;
}

void dtex_free(void *output) {
    free(output);
}
