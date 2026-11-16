#include "texture.h"
#include "txd.h"

#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *out, uint32_t value) {
    put16(out, (uint16_t)value);
    put16(out + 2, (uint16_t)(value >> 16));
}

uint8_t *texture_resize_rgba(const uint8_t *src, unsigned sw, unsigned sh,
                             unsigned tw, unsigned th) {
    size_t count = (size_t)tw * th * 4;
    uint8_t *dst = calloc(count, 1);
    if (!dst) return NULL;
    if (sw == tw && sh == th) {
        memcpy(dst, src, count);
    } else if (sw % tw == 0 && sh % th == 0) {
        unsigned fx = sw / tw, fy = sh / th;
        for (unsigned y = 0; y < th; ++y)
            for (unsigned x = 0; x < tw; ++x) {
                uint64_t r = 0, g = 0, b = 0, a = 0;
                for (unsigned yy = y * fy; yy < y * fy + fy; ++yy)
                    for (unsigned xx = x * fx; xx < x * fx + fx; ++xx) {
                        const uint8_t *pixel = src + ((size_t)yy * sw + xx) * 4;
                        unsigned alpha = pixel[3];
                        r += pixel[0] * alpha;
                        g += pixel[1] * alpha;
                        b += pixel[2] * alpha;
                        a += alpha;
                    }
                uint8_t *pixel = dst + ((size_t)y * tw + x) * 4;
                if (a) {
                    pixel[0] = (uint8_t)(r / a);
                    pixel[1] = (uint8_t)(g / a);
                    pixel[2] = (uint8_t)(b / a);
                    pixel[3] = (uint8_t)(a / (fx * fy));
                }
            }
    } else {
        for (unsigned y = 0; y < th; ++y)
            for (unsigned x = 0; x < tw; ++x) {
                unsigned sx = x * sw / tw, sy = y * sh / th;
                if (sx >= sw) sx = sw - 1;
                if (sy >= sh) sy = sh - 1;
                memcpy(dst + ((size_t)y * tw + x) * 4,
                       src + ((size_t)sy * sw + sx) * 4, 4);
            }
    }
    return dst;
}

int texture_inspect(const uint8_t *input, size_t size,
                    texture_info **textures, size_t *count) {
    if (!input || !textures || !count) return 0;
    *textures = NULL;
    *count = 0;
    texture_info *items = NULL;
    size_t capacity = 0;
    txd_texture tx;
    for (size_t i = 0; txd_get(input, size, i, &tx); ++i) {
        if (i == UINT16_MAX) {
            free(items);
            return 0;
        }
        if (i == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            texture_info *more = realloc(items, next * sizeof *items);
            if (!more) {
                free(items);
                return 0;
            }
            items = more;
            capacity = next;
        }
        memset(&items[i], 0, sizeof items[i]);
        memcpy(items[i].name, tx.name, sizeof items[i].name);
        items[i].width = tx.width;
        items[i].height = tx.height;
        *count = i + 1;
    }
    *textures = items;
    return 1;
}

int texture_convert_txd(const uint8_t *input, size_t size, const texture_target *targets,
                        size_t count, uint8_t **output, size_t *length) {
    if (!input || !output || !length || count > UINT16_MAX || (count && !targets))
        return 0;
    uint8_t **blobs = count ? calloc(count, sizeof *blobs) : NULL;
    size_t *sizes = count ? calloc(count, sizeof *sizes) : NULL;
    char (*names)[32] = count ? calloc(count, sizeof *names) : NULL;
    if (count && (!blobs || !sizes || !names)) {
        free(blobs);
        free(sizes);
        free(names);
        return 0;
    }
    uint64_t total = 8 + count * 40;
    int ok = 1;
    for (size_t i = 0; i < count && ok; ++i) {
        txd_texture tx;
        if (!txd_get(input, size, i, &tx) || !targets[i].width || !targets[i].height) {
            ok = 0;
            break;
        }
        size_t name_len = 0;
        while (name_len < 31 && tx.name[name_len]) ++name_len;
        memcpy(names[i], tx.name, name_len);
        size_t level = 0;
        while (level + 1 < tx.level_count) {
            unsigned next_w = level < 15 ? tx.width >> (level + 1) : 0;
            unsigned next_h = level < 15 ? tx.height >> (level + 1) : 0;
            if (!next_w) next_w = 1;
            if (!next_h) next_h = 1;
            if (next_w < targets[i].width || next_h < targets[i].height)
                break;
            ++level;
        }
        uint8_t *rgba, *resized;
        uint16_t sw, sh;
        if (!txd_decode(&tx, level, &rgba, &sw, &sh)) {
            ok = 0;
            break;
        }
        resized = texture_resize_rgba(rgba, sw, sh, targets[i].width, targets[i].height);
        free(rgba);
        if (!resized) {
            ok = 0;
            break;
        }
        ok = dtex_encode(resized, targets[i].width, targets[i].height,
                         targets[i].options, &blobs[i], &sizes[i]);
        free(resized);
        total += (sizes[i] + 3) & ~(uint64_t)3;
        if (total > UINT32_MAX) ok = 0;
    }
    if (ok && txd_get(input, size, count, &(txd_texture){0})) ok = 0;
    uint8_t *out = ok ? calloc((size_t)total, 1) : NULL;
    if (!out) ok = 0;
    if (ok) {
        memcpy(out, "DTXD", 4);
        put16(out + 4, 1);
        put16(out + 6, (uint16_t)count);
        uint32_t off = (uint32_t)(8 + count * 40);
        for (size_t i = 0; i < count; ++i) {
            uint8_t *record = out + 8 + i * 40;
            memcpy(record, names[i], 32);
            put32(record + 32, off);
            put32(record + 36, (uint32_t)sizes[i]);
            memcpy(out + off, blobs[i], sizes[i]);
            off += (uint32_t)((sizes[i] + 3) & ~(size_t)3);
        }
        *output = out;
        *length = (size_t)total;
    }
    for (size_t i = 0; i < count; ++i) free(blobs[i]);
    free(blobs);
    free(sizes);
    free(names);
    return ok;
}
