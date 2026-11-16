#include "mpeg_video.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: mpeg_video_cli INPUT.MPG OUTPUT.YUV [FRAMES]\n");
        return 2;
    }
    unsigned long limit = 0;
    if (argc == 4) {
        char *end;
        errno = 0;
        limit = strtoul(argv[3], &end, 10);
        if (errno || end == argv[3] || *end || !limit) return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) {
        if (input) fclose(input);
        return 1;
    }
    long length = ftell(input);
    if (length <= 0) {
        fclose(input);
        return 1;
    }
    rewind(input);
    uint8_t *data = malloc((size_t)length);
    if (!data) {
        fclose(input);
        return 1;
    }
    int ok = fread(data, 1, (size_t)length, input) == (size_t)length;
    fclose(input);
    mpeg_video *video = ok ? mpeg_video_open(data, (size_t)length) : NULL;
    free(data);
    FILE *output = video ? fopen(argv[2], "wbx") : NULL;
    size_t count = 0;
    if (output) {
        while (!limit || count < limit) {
            uint8_t *frame = NULL;
            size_t frame_length = 0;
            uint32_t width = 0, height = 0;
            double time = 0;
            if (!mpeg_video_next(video, &frame, &frame_length,
                                 &width, &height, &time)) break;
            ok = fwrite(frame, 1, frame_length, output) == frame_length;
            free(frame);
            if (!ok) break;
            ++count;
        }
        if (fclose(output)) ok = 0;
        if (!ok || !count) remove(argv[2]);
    }
    mpeg_video_close(video);
    if (!output || !ok || !count) {
        fprintf(stderr, "failed to decode MPEG-1 video\n");
        return 1;
    }
    printf("%zu frames\n", count);
    return 0;
}
