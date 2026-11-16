#include "dvid.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint8_t *data = malloc(length ? (size_t)length : 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    int ok = fread(data, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) {
        free(data);
        return NULL;
    }
    *size = (size_t)length;
    return data;
}

static int rate(const char *text, unsigned long maximum, uint32_t *value) {
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || !parsed || parsed > maximum) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 5 || argc > 7) {
        fprintf(stderr, "usage: dvid_cli PALETTE.RGB FRAMES.PAL AUDIO.PCM OUTPUT.DVID [FPS [RATE]]\n");
        return 2;
    }
    uint32_t fps = 10, sample_rate = 22050;
    if ((argc >= 6 && !rate(argv[5], UINT16_MAX, &fps)) ||
        (argc >= 7 && !rate(argv[6], UINT32_MAX, &sample_rate))) return 2;
    size_t palette_size = 0, frame_size = 0, audio_size = 0, length = 0;
    uint8_t *palette = read_file(argv[1], &palette_size);
    uint8_t *frames = read_file(argv[2], &frame_size);
    uint8_t *audio = read_file(argv[3], &audio_size);
    uint8_t *output = NULL;
    uint32_t count = 0;
    int ok = palette && frames && audio &&
             dvid_pack(palette, palette_size, frames, frame_size,
                       audio, audio_size, (uint16_t)fps, sample_rate,
                       &output, &length, &count);
    FILE *destination = ok ? fopen(argv[4], "wbx") : NULL;
    if (!destination) ok = 0;
    else {
        ok = fwrite(output, 1, length, destination) == length;
        if (fclose(destination)) ok = 0;
        if (!ok) remove(argv[4]);
    }
    free(palette);
    free(frames);
    free(audio);
    free(output);
    if (ok) printf("%u frames\n", count);
    else fprintf(stderr, "failed to pack DVID\n");
    return ok ? 0 : 1;
}
