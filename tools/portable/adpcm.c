#include "adpcm.h"

#include <stdlib.h>

static const uint16_t steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int index_delta[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static void store16(uint8_t *dst, int value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

int adpcm_encode(const int16_t *pcm, size_t count, int with_checkpoints,
                 uint8_t **output, size_t *length, uint8_t **checkpoints,
                 size_t *checkpoint_length) {
    if ((!pcm && count) || !output || !length || !checkpoints || !checkpoint_length ||
        count > (SIZE_MAX - 4) / 2)
        return 0;
    size_t nbytes = 4 + (count + 1) / 2;
    size_t ncheck = with_checkpoints ? (count + 8191) / 8192 * 4 : 0;
    uint8_t *out = malloc(nbytes), *checks = ncheck ? malloc(ncheck) : NULL;
    if (!out || (ncheck && !checks)) {
        free(out);
        free(checks);
        return 0;
    }
    int pred = count ? pcm[0] : 0;
    int index = 0;
    store16(out, pred);
    out[2] = out[3] = 0;
    size_t cursor = 4, checkpoint_cursor = 0;
    int low = -1;
    for (size_t i = 0; i < count; ++i) {
        if (with_checkpoints && i % 8192 == 0) {
            store16(checks + checkpoint_cursor, pred);
            checks[checkpoint_cursor + 2] = (uint8_t)index;
            checks[checkpoint_cursor + 3] = 0;
            checkpoint_cursor += 4;
        }
        int step = steps[index], difference = (int)pcm[i] - pred;
        int code = 0;
        if (difference < 0) {
            difference = -difference;
            code = 8;
        }
        int change = step >> 3;
        if (difference >= step) {
            code |= 4;
            difference -= step;
            change += step;
        }
        step >>= 1;
        if (difference >= step) {
            code |= 2;
            difference -= step;
            change += step;
        }
        step >>= 1;
        if (difference >= step) {
            code |= 1;
            change += step;
        }
        pred += (code & 8) ? -change : change;
        if (pred < -32767) pred = -32767;
        if (pred > 32767) pred = 32767;
        index += index_delta[code & 7];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        if (low < 0) low = code;
        else {
            out[cursor++] = (uint8_t)(low | code << 4);
            low = -1;
        }
    }
    if (low >= 0) out[cursor++] = (uint8_t)low;
    *output = out;
    *length = cursor;
    *checkpoints = checks;
    *checkpoint_length = checkpoint_cursor;
    return 1;
}
