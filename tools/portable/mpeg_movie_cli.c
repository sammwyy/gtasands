#include "mpeg_movie.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int rate(const char *text, unsigned long maximum, uint32_t *value) {
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || !parsed || parsed > maximum) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: mpeg_movie_cli INPUT.MPG OUTPUT.DVID [FPS [RATE]]\n");
        return 2;
    }
    uint32_t fps = 10, sample_rate = 22050;
    if ((argc >= 4 && !rate(argv[3], UINT16_MAX, &fps)) ||
        (argc >= 5 && !rate(argv[4], UINT16_MAX, &sample_rate))) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) {
        if (input) fclose(input);
        return 1;
    }
    long size = ftell(input);
    if (size <= 0) {
        fclose(input);
        return 1;
    }
    rewind(input);
    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(input);
        return 1;
    }
    int ok = fread(data, 1, (size_t)size, input) == (size_t)size;
    fclose(input);
    uint8_t *video = NULL;
    size_t length = 0;
    uint32_t frames = 0;
    if (ok) ok = mpeg_movie_convert(data, (size_t)size, (uint16_t)fps,
                                     sample_rate, &video, &length, &frames);
    free(data);
    FILE *output = ok ? fopen(argv[2], "wbx") : NULL;
    if (!output) ok = 0;
    else {
        ok = fwrite(video, 1, length, output) == length;
        if (fclose(output)) ok = 0;
        if (!ok) remove(argv[2]);
    }
    free(video);
    if (ok) printf("%u frames\n", frames);
    else fprintf(stderr, "failed to convert MPEG movie\n");
    return ok ? 0 : 1;
}
