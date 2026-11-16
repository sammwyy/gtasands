#ifndef GTASADS_MPEG_VIDEO_H
#define GTASADS_MPEG_VIDEO_H

#include <stddef.h>
#include <stdint.h>

typedef struct mpeg_video mpeg_video;

mpeg_video *mpeg_video_open(const uint8_t *source, size_t length);
int mpeg_video_next(mpeg_video *video, uint8_t **pixels, size_t *length,
                    uint32_t *width, uint32_t *height, double *time);
void mpeg_video_close(mpeg_video *video);

#endif
