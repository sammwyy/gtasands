#include "mpeg_video.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "vendor/pl_mpeg.h"

struct mpeg_video {
    plm_t *decoder;
    uint8_t *source;
};

mpeg_video *mpeg_video_open(const uint8_t *source, size_t length) {
    if (!source || !length) return NULL;
    mpeg_video *video = calloc(1, sizeof *video);
    if (!video) return NULL;
    video->source = malloc(length);
    if (video->source) {
        memcpy(video->source, source, length);
        video->decoder = plm_create_with_memory(video->source, length, FALSE);
    }
    if (!video->decoder) {
        mpeg_video_close(video);
        return NULL;
    }
    plm_set_audio_enabled(video->decoder, FALSE);
    return video;
}

int mpeg_video_next(mpeg_video *video, uint8_t **pixels, size_t *length,
                    uint32_t *width, uint32_t *height, double *time) {
    if (!video || !pixels || !length || !width || !height || !time) return 0;
    *pixels = NULL;
    *length = 0;
    plm_frame_t *frame = plm_decode_video(video->decoder);
    if (!frame || !frame->width || !frame->height) return 0;
    size_t w = frame->width, h = frame->height;
    size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (w > SIZE_MAX / h || cw > SIZE_MAX / ch) return 0;
    size_t luminance = w * h, chroma = cw * ch;
    if (chroma > (SIZE_MAX - luminance) / 2 ||
        frame->y.width < w || frame->y.height < h ||
        frame->cb.width < cw || frame->cb.height < ch ||
        frame->cr.width < cw || frame->cr.height < ch) return 0;
    uint8_t *output = malloc(luminance + 2 * chroma);
    if (!output) return 0;
    for (size_t row = 0; row < h; ++row)
        memcpy(output + row * w, frame->y.data + row * frame->y.width, w);
    for (size_t row = 0; row < ch; ++row) {
        memcpy(output + luminance + row * cw,
               frame->cb.data + row * frame->cb.width, cw);
        memcpy(output + luminance + chroma + row * cw,
               frame->cr.data + row * frame->cr.width, cw);
    }
    *pixels = output;
    *length = luminance + 2 * chroma;
    *width = frame->width;
    *height = frame->height;
    *time = frame->time;
    return 1;
}

void mpeg_video_close(mpeg_video *video) {
    if (!video) return;
    if (video->decoder) plm_destroy(video->decoder);
    free(video->source);
    free(video);
}
