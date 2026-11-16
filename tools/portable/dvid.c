#include "dvid.h"

#include <stdlib.h>
#include <string.h>

enum { WIDTH = 256, HEIGHT = 256, PIXELS = WIDTH * HEIGHT, PALETTE = 768 };

static void put16(uint8_t *target, uint16_t value) {
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *target, uint32_t value) {
    put16(target, (uint16_t)value);
    put16(target + 2, (uint16_t)(value >> 16));
}

int dvid_pack(const uint8_t *palette, size_t palette_size,
              const uint8_t *frames, size_t frame_size,
              const uint8_t *audio, size_t audio_size,
              uint16_t fps, uint32_t rate,
              uint8_t **output, size_t *length, uint32_t *frame_count) {
    if (!palette || palette_size < PALETTE || (!frames && frame_size) ||
        (!audio && audio_size) || !fps || !rate || !output || !length ||
        !frame_count) return 0;
    size_t stride = frame_size % PIXELS && !(frame_size % (PIXELS + 1024)) ?
                    PIXELS + 1024 : PIXELS;
    size_t count = frame_size / stride;
    if (count > UINT32_MAX || audio_size > SIZE_MAX - 544 ||
        count > (SIZE_MAX - 544 - audio_size) / (PIXELS + 5)) return 0;
    size_t capacity = 544 + count * (PIXELS + 5) + audio_size;
    uint8_t *result = malloc(capacity);
    if (!result) return 0;
    memcpy(result, "DVID", 4);
    put16(result + 4, 1);
    put16(result + 6, WIDTH);
    put16(result + 8, HEIGHT);
    put16(result + 10, fps);
    memset(result + 12, 0, 4);
    put32(result + 16, (uint32_t)count);
    put32(result + 20, rate);
    for (size_t i = 0; i < 256; ++i)
        put16(result + 24 + i * 2, (uint16_t)
              ((palette[i * 3] >> 3) |
               ((palette[i * 3 + 1] >> 3) << 5) |
               ((palette[i * 3 + 2] >> 3) << 10)));
    size_t pos = 536, consumed = 0;
    double per = rate * 2.0 / fps;
    for (size_t i = 0; i < count; ++i) {
        double target = (i + 1) * per;
        size_t next = target >= (double)audio_size ? audio_size : (size_t)target;
        size_t chunk = next - consumed;
        if (chunk > UINT32_MAX - (chunk & 1)) {
            free(result);
            return 0;
        }
        put32(result + pos, (uint32_t)(chunk + (chunk & 1)));
        pos += 4;
        if (chunk) memcpy(result + pos, audio + consumed, chunk);
        pos += chunk;
        if (chunk & 1) result[pos++] = 0;
        memcpy(result + pos, frames + i * stride, PIXELS);
        pos += PIXELS;
        consumed = next;
    }
    if (consumed < audio_size) {
        if (audio_size - consumed > UINT32_MAX) {
            free(result);
            return 0;
        }
        put32(result + pos, (uint32_t)(audio_size - consumed));
        pos += 4;
        memcpy(result + pos, audio + consumed, audio_size - consumed);
        pos += audio_size - consumed;
    }
    put32(result + pos, 0);
    *length = pos + 4;
    *frame_count = (uint32_t)count;
    *output = result;
    return 1;
}
