#include "mpeg_movie.h"

#include "dvid.h"
#include "mpeg_audio.h"
#include "mpeg_video.h"
#include "movie_palette.h"
#include "resample.h"

#include <stdlib.h>
#include <string.h>

enum { WIDTH = 256, HEIGHT = 192, PIXELS = 256 * 256, COLORS = 32768 };

static uint8_t clamp(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void scale_frame(const uint8_t *yuv, uint32_t width, uint32_t height,
                        uint8_t *rgb) {
    size_t cw = (width + 1) / 2;
    size_t ch = (height + 1) / 2;
    const uint8_t *u = yuv + (size_t)width * height;
    const uint8_t *v = u + cw * ch;
    for (int y = 0; y < HEIGHT; ++y) {
        int64_t fy = (((int64_t)(2 * y + 1) * height) << 15) / HEIGHT - 32768;
        if (fy < 0) fy = 0;
        if (fy > (int64_t)(height - 1) * 65536)
            fy = (int64_t)(height - 1) * 65536;
        size_t y0 = (size_t)(fy >> 16);
        size_t y1 = y0 + 1 < height ? y0 + 1 : y0;
        int wy = (int)(fy & 65535);
        for (int x = 0; x < WIDTH; ++x) {
            int64_t fx = (((int64_t)(2 * x + 1) * width) << 15) / WIDTH - 32768;
            if (fx < 0) fx = 0;
            if (fx > (int64_t)(width - 1) * 65536)
                fx = (int64_t)(width - 1) * 65536;
            size_t x0 = (size_t)(fx >> 16);
            size_t x1 = x0 + 1 < width ? x0 + 1 : x0;
            int wx = (int)(fx & 65535);
            int top = (int)((yuv[y0 * width + x0] * (int64_t)(65536 - wx) +
                             yuv[y0 * width + x1] * (int64_t)wx + 32768) >> 16);
            int bottom = (int)((yuv[y1 * width + x0] * (int64_t)(65536 - wx) +
                                yuv[y1 * width + x1] * (int64_t)wx + 32768) >> 16);
            int luminance = (int)((top * (int64_t)(65536 - wy) +
                                   bottom * (int64_t)wy + 32768) >> 16) - 16;
            int cb = u[y0 / 2 * cw + x0 / 2] - 128;
            int cr = v[y0 / 2 * cw + x0 / 2] - 128;
            size_t index = ((size_t)y * WIDTH + x) * 3;
            rgb[index] = clamp((298 * luminance + 409 * cr + 128) >> 8);
            rgb[index + 1] = clamp((298 * luminance - 100 * cb -
                                    208 * cr + 128) >> 8);
            rgb[index + 2] = clamp((298 * luminance + 516 * cb + 128) >> 8);
        }
    }
}

static int video_pass(const uint8_t *source, size_t length, uint16_t fps,
                      uint32_t *histogram, const uint8_t *lookup,
                      uint8_t *frames, size_t maximum, size_t *count) {
    mpeg_video *video = mpeg_video_open(source, length);
    uint8_t *rgb = malloc(WIDTH * HEIGHT * 3);
    if (!video || !rgb) {
        mpeg_video_close(video);
        free(rgb);
        return 0;
    }
    size_t selected = 0;
    int ok = 1;
    for (;;) {
        uint8_t *yuv = NULL;
        size_t bytes = 0;
        uint32_t width = 0, height = 0;
        double time = 0;
        if (!mpeg_video_next(video, &yuv, &bytes, &width, &height, &time))
            break;
        if (time * fps + 0.000001 >= (double)selected) {
            if (selected >= maximum) ok = 0;
            if (ok) {
                scale_frame(yuv, width, height, rgb);
                if (histogram)
                    for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
                        uint8_t *pixel = rgb + i * 3;
                        uint32_t color = pixel[0] >> 3 |
                                         (uint32_t)(pixel[1] >> 3) << 5 |
                                         (uint32_t)(pixel[2] >> 3) << 10;
                        if (histogram[color] != UINT32_MAX)
                            ++histogram[color];
                    }
                else movie_palette_frame(rgb, frames + selected * PIXELS,
                                         lookup);
                ++selected;
            }
        }
        free(yuv);
        if (!ok) break;
    }
    mpeg_video_close(video);
    free(rgb);
    *count = selected;
    return ok && selected;
}

static int decode_audio(const uint8_t *source, size_t length, uint32_t target,
                        uint8_t **output, size_t *output_length) {
    mpeg_audio *decoder = mpeg_audio_open(source, length);
    if (!decoder) return 0;
    int16_t *samples = NULL;
    size_t count = 0, capacity = 0;
    uint32_t source_rate = 0;
    int ok = 1;
    for (;;) {
        int16_t *frame = NULL;
        size_t n = 0;
        uint32_t rate = 0;
        double time = 0;
        if (!mpeg_audio_next(decoder, &frame, &n, &rate, &time)) break;
        if (source_rate && source_rate != rate) ok = 0;
        source_rate = rate;
        if (n > SIZE_MAX / sizeof *samples - count) ok = 0;
        if (ok && count + n > capacity) {
            size_t needed = count + n;
            size_t next = capacity > (SIZE_MAX / sizeof *samples) / 2 ?
                          needed : capacity * 2;
            if (next < needed) next = needed;
            int16_t *more = realloc(samples, next * sizeof *samples);
            if (more) {
                samples = more;
                capacity = next;
            } else ok = 0;
        }
        if (ok) {
            memcpy(samples + count, frame, n * sizeof *frame);
            count += n;
        }
        free(frame);
        if (!ok) break;
    }
    mpeg_audio_close(decoder);
    if (!count || !source_rate || source_rate < target) ok = 0;
    if (ok && target < source_rate) {
        int16_t *converted = NULL;
        size_t converted_count = 0;
        ok = resample_pcm16(samples, count, source_rate, target,
                            &converted, &converted_count);
        if (ok) {
            free(samples);
            samples = converted;
            count = converted_count;
        }
    }
    if (count > SIZE_MAX / 2) ok = 0;
    uint8_t *pcm = ok ? malloc(count * 2) : NULL;
    if (ok && !pcm) ok = 0;
    for (size_t i = 0; i < count && ok; ++i) {
        pcm[i * 2] = (uint8_t)samples[i];
        pcm[i * 2 + 1] = (uint8_t)((uint16_t)samples[i] >> 8);
    }
    free(samples);
    if (!ok) {
        free(pcm);
        return 0;
    }
    *output = pcm;
    *output_length = count * 2;
    return 1;
}

int mpeg_movie_convert(const uint8_t *source, size_t length,
                       uint16_t fps, uint32_t audio_rate,
                       uint8_t **output, size_t *output_length,
                       uint32_t *frame_count) {
    if (!source || !length || !fps || !audio_rate ||
        audio_rate > UINT16_MAX || !output || !output_length ||
        !frame_count) return 0;
    *output = NULL;
    *output_length = 0;
    uint32_t *histogram = calloc(COLORS, sizeof *histogram);
    uint8_t *lookup = malloc(COLORS);
    size_t count = 0, produced = 0, pcm_size = 0;
    uint8_t *frames = NULL, *pcm = NULL;
    uint8_t palette[768];
    int ok = histogram && lookup &&
             video_pass(source, length, fps, histogram, NULL, NULL,
                        UINT32_MAX, &count) &&
             count <= SIZE_MAX / PIXELS && count <= UINT32_MAX;
    if (ok) {
        movie_palette_build(histogram, palette, lookup);
        frames = malloc(count * PIXELS);
        ok = frames && video_pass(source, length, fps, NULL, lookup, frames,
                                  count, &produced) && produced == count;
    }
    if (ok) ok = decode_audio(source, length, audio_rate, &pcm, &pcm_size);
    if (ok) ok = dvid_pack(palette, sizeof palette, frames, count * PIXELS,
                           pcm, pcm_size, fps, audio_rate,
                           output, output_length, frame_count);
    free(histogram);
    free(lookup);
    free(frames);
    free(pcm);
    return ok;
}
