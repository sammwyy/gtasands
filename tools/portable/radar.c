#include "radar.h"
#include "texture.h"
#include "txd.h"

#include <stdlib.h>

static void store_pixels(uint8_t *out, const uint8_t *rgba, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unsigned r = rgba[i * 4] >> 3, g = rgba[i * 4 + 1] >> 3, b = rgba[i * 4 + 2] >> 3;
        unsigned value = r | g << 5 | b << 10 | 0x8000u;
        out[i * 2] = (uint8_t)value;
        out[i * 2 + 1] = (uint8_t)(value >> 8);
    }
}

int radar_convert(const uint8_t *txd, size_t size, uint8_t **output, size_t *length) {
    txd_texture texture;
    if (!txd_get(txd, size, 0, &texture) || !output || !length) return 0;
    size_t level = 0;
    while (level + 1 < texture.level_count) {
        unsigned w = level < 15 ? texture.width >> (level + 1) : 0;
        unsigned h = level < 15 ? texture.height >> (level + 1) : 0;
        if (w < 64 || h < 64) break;
        ++level;
    }
    uint8_t *pixels;
    uint16_t w, h;
    if (!txd_decode(&texture, level, &pixels, &w, &h)) return 0;
    for (size_t i = 0; i < (size_t)w * h; ++i) pixels[i * 4 + 3] = 255;
    while (w > 64 && h > 64 && w % 2 == 0 && h % 2 == 0) {
        uint8_t *half = texture_resize_rgba(pixels, w, h, w / 2, h / 2);
        free(pixels);
        if (!half) return 0;
        pixels = half;
        w /= 2;
        h /= 2;
    }
    uint8_t *full = texture_resize_rgba(pixels, w, h, 64, 64);
    free(pixels);
    if (!full) return 0;
    uint8_t *mip = texture_resize_rgba(full, 64, 64, 32, 32);
    if (!mip) {
        free(full);
        return 0;
    }
    uint8_t *out = malloc(10240);
    if (out) {
        store_pixels(out, full, 64 * 64);
        store_pixels(out + 64 * 64 * 2, mip, 32 * 32);
        *output = out;
        *length = 10240;
    }
    free(full);
    free(mip);
    return out != NULL;
}
