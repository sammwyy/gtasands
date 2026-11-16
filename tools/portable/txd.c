#include "txd.h"

#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *data) {
    return (uint16_t)(data[0] | data[1] << 8);
}

static uint32_t u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t u48(const uint8_t *data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 6; ++i) value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static int chunk(const uint8_t *data, size_t limit, size_t *position,
                 uint32_t *type, size_t *start, size_t *end) {
    size_t o = *position;
    if (o > limit || limit - o < 12) return 0;
    *type = u32(data + o);
    *start = o + 12;
    size_t length = u32(data + o + 4);
    if (length > limit - *start) return 0;
    *end = *start + length;
    *position = *end;
    return 1;
}

static int native(const uint8_t *data, size_t start, size_t end, txd_texture *texture) {
    size_t pos = start, s, e;
    uint32_t type;
    while (chunk(data, end, &pos, &type, &s, &e)) {
        if (type != 1) continue;
        if (e - s < 88) return 0;
        const uint8_t *p = data + s;
        uint32_t platform = u32(p), format = u32(p + 76);
        if (platform != 8 && platform != 9) return 0;
        memset(texture, 0, sizeof *texture);
        memcpy(texture->name, p + 8, 32);
        texture->name[32] = 0;
        texture->raster = u32(p + 72);
        texture->width = u16(p + 80);
        texture->height = u16(p + 82);
        texture->level_count = p[85];
        texture->has_alpha = platform == 8 ? !!format : !!(p[87] & 1u);
        texture->dxt = platform == 8 ? p[87] :
                       p[76] == 'D' && p[77] == 'X' && p[78] == 'T' &&
                       p[79] >= '0' && p[79] <= '9' ? (uint8_t)(p[79] - '0') : 0;
        size_t off = s + 88;
        size_t palette_size = texture->raster & 0x2000u ? 1024 :
                              texture->raster & 0x4000u ? 128 : 0;
        if (palette_size > e - off) return 0;
        texture->palette = palette_size ? data + off : NULL;
        off += palette_size;
        texture->level_data = data + off;
        texture->level_size = e - off;
        return texture->width && texture->height && texture->level_count;
    }
    return 0;
}

int txd_get(const uint8_t *data, size_t size, size_t index, txd_texture *texture) {
    if (!data || !texture) return 0;
    size_t pos = 0, s, e;
    uint32_t type;
    while (chunk(data, size, &pos, &type, &s, &e)) {
        if (type != 0x16) continue;
        size_t inner = s, ns, ne;
        uint32_t nt;
        while (chunk(data, e, &inner, &nt, &ns, &ne)) {
            if (nt == 0x15) {
                if (!index) return native(data, ns, ne, texture);
                --index;
            }
        }
    }
    return 0;
}

static void rgb565(uint16_t value, uint8_t *out) {
    unsigned r = value >> 11, g = (value >> 5) & 63u, b = value & 31u;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static void decode_dxt(const uint8_t *data, size_t size, unsigned w, unsigned h,
                       unsigned kind, uint8_t *out) {
    size_t o = 0, block_size = kind == 1 ? 8 : 16;
    for (unsigned by = 0; by < (h + 3) / 4; ++by) {
        for (unsigned bx = 0; bx < (w + 3) / 4; ++bx) {
            if (size - o < block_size) return;
            const uint8_t *block = data + o;
            const uint8_t *color = block + (kind == 1 ? 0 : 8);
            uint16_t c0 = u16(color), c1 = u16(color + 2);
            uint32_t bits = u32(color + 4);
            uint8_t pal[4][4];
            rgb565(c0, pal[0]);
            rgb565(c1, pal[1]);
            pal[0][3] = pal[1][3] = 255;
            if (c0 > c1 || kind != 1) {
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
                    pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
                }
                pal[2][3] = pal[3][3] = 255;
            } else {
                for (int k = 0; k < 3; ++k) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
                pal[2][3] = 255;
                memset(pal[3], 0, 4);
            }
            uint8_t alpha[16], alpha_table[8];
            if (kind == 3) {
                uint64_t a64 = u48(block) | (uint64_t)u16(block + 6) << 48;
                for (unsigned i = 0; i < 16; ++i)
                    alpha[i] = (uint8_t)(((a64 >> (i * 4)) & 15u) * 17u);
            } else if (kind == 5) {
                unsigned a0 = block[0], a1 = block[1];
                alpha_table[0] = (uint8_t)a0;
                alpha_table[1] = (uint8_t)a1;
                if (a0 > a1) {
                    for (unsigned i = 1; i < 7; ++i)
                        alpha_table[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                } else {
                    for (unsigned i = 1; i < 5; ++i)
                        alpha_table[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
                    alpha_table[6] = 0;
                    alpha_table[7] = 255;
                }
                uint64_t a48 = u48(block + 2);
                for (unsigned i = 0; i < 16; ++i)
                    alpha[i] = alpha_table[(a48 >> (3 * i)) & 7u];
            }
            for (unsigned i = 0; i < 16; ++i) {
                unsigned x = bx * 4 + (i & 3u), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t *dst = out + ((size_t)y * w + x) * 4;
                memcpy(dst, pal[(bits >> (2 * i)) & 3u], 4);
                if (kind == 3 || kind == 5) dst[3] = alpha[i];
            }
            o += block_size;
        }
    }
}

int txd_decode(const txd_texture *texture, size_t level,
               uint8_t **rgba, uint16_t *width, uint16_t *height) {
    if (!texture || !rgba || !width || !height || level >= texture->level_count)
        return 0;
    unsigned w = level < 16 ? texture->width >> level : 0;
    unsigned h = level < 16 ? texture->height >> level : 0;
    if (!w) w = 1;
    if (!h) h = 1;
    size_t n = (size_t)w * h, remaining = texture->level_size;
    const uint8_t *data = texture->level_data;
    for (size_t i = 0; i <= level; ++i) {
        if (remaining < 4) return 0;
        uint32_t size = u32(data);
        data += 4;
        remaining -= 4;
        if (size > remaining) return 0;
        if (i == level) {
            remaining = size;
            break;
        }
        data += size;
        remaining -= size;
    }
    uint8_t *out = calloc(n, 4);
    if (!out) return 0;
    if (texture->dxt) {
        decode_dxt(data, remaining, w, h, texture->dxt, out);
    } else if (texture->palette) {
        if (remaining < (texture->raster & 0x4000u ? (n + 1) / 2 : n)) goto fail;
        for (size_t i = 0; i < n; ++i) {
            unsigned index = texture->raster & 0x4000u ?
                             (data[i / 2] >> ((i % 2) * 4)) & 15u : data[i];
            memcpy(out + i * 4, texture->palette + index * 4, 4);
        }
    } else {
        unsigned format = texture->raster & 0xf00u;
        if (format == 0x500u || format == 0x600u) {
            if (remaining < n * 4) goto fail;
            for (size_t i = 0; i < n; ++i) {
                out[i * 4] = data[i * 4 + 2];
                out[i * 4 + 1] = data[i * 4 + 1];
                out[i * 4 + 2] = data[i * 4];
                out[i * 4 + 3] = format == 0x500u && texture->has_alpha ? data[i * 4 + 3] : 255;
            }
        } else if (format == 0x400u) {
            if (remaining < n) goto fail;
            for (size_t i = 0; i < n; ++i) {
                out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = data[i];
                out[i * 4 + 3] = 255;
            }
        } else {
            if (remaining < n * 2) goto fail;
            for (size_t i = 0; i < n; ++i) {
                uint16_t p = u16(data + i * 2);
                if (format == 0x200u) {
                    out[i * 4] = (uint8_t)((p >> 11) << 3);
                    out[i * 4 + 1] = (uint8_t)(((p >> 5) & 63u) << 2);
                    out[i * 4 + 2] = (uint8_t)((p & 31u) << 3);
                    out[i * 4 + 3] = 255;
                } else if (format == 0x300u) {
                    out[i * 4] = (uint8_t)(((p >> 8) & 15u) * 17u);
                    out[i * 4 + 1] = (uint8_t)(((p >> 4) & 15u) * 17u);
                    out[i * 4 + 2] = (uint8_t)((p & 15u) * 17u);
                    out[i * 4 + 3] = (uint8_t)((p >> 12) * 17u);
                } else {
                    out[i * 4] = (uint8_t)(((p >> 10) & 31u) << 3);
                    out[i * 4 + 1] = (uint8_t)(((p >> 5) & 31u) << 3);
                    out[i * 4 + 2] = (uint8_t)((p & 31u) << 3);
                    out[i * 4 + 3] = format == 0xa00u || p & 0x8000u ? 255 : 0;
                }
            }
        }
    }
    *rgba = out;
    *width = (uint16_t)w;
    *height = (uint16_t)h;
    return 1;
fail:
    free(out);
    return 0;
}
