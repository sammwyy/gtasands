#include "dsnd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int format(const char *name) {
    if (!strcmp(name, "pcm8")) return 0;
    if (!strcmp(name, "pcm16")) return 1;
    if (!strcmp(name, "adpcm")) return 2;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 10 ||
        (strcmp(argv[1], "sound") && strcmp(argv[1], "bank") &&
         strcmp(argv[1], "decrypt"))) {
        fprintf(stderr, "usage: audio_cli sound|bank|decrypt INPUT OUTPUT [INPUT_RATE [SAMPLE_RATE [FORMAT [VOLUME [MAX_SECONDS [LOOP]]]]]]\n");
        return 2;
    }
    FILE *source = fopen(argv[2], "rb");
    if (!source || fseek(source, 0, SEEK_END) || ftell(source) < 0) {
        if (source) fclose(source);
        return 1;
    }
    size_t length = (size_t)ftell(source);
    uint8_t *raw = malloc(length ? length : 1);
    if (!raw) {
        fclose(source);
        return 1;
    }
    rewind(source);
    int ok = fread(raw, 1, length, source) == length;
    fclose(source);
    dsnd_options opts = {0};
    opts.rate = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 22050;
    opts.sample_rate = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 11025;
    int codec = argc > 6 ? format(argv[6]) : 2;
    opts.format = (uint8_t)codec;
    opts.volume = argc > 7 ? strtod(argv[7], NULL) : 1;
    opts.max_seconds = argc > 8 ? strtod(argv[8], NULL) : 0;
    opts.loop = argc > 9 ? (int32_t)strtol(argv[9], NULL, 10) : -1;
    uint64_t decrypt_offset = argc > 4 ? strtoull(argv[4], NULL, 10) : 0;
    uint8_t *output = NULL, *saved = NULL;
    size_t converted = 0, saved_length = 0;
    uint16_t sounds = 0;
    if (!strcmp(argv[1], "decrypt")) {
        if (ok) {
            output = malloc(length ? length : 1);
            ok = output != NULL;
            if (ok) {
                audio_decrypt(raw, length, decrypt_offset, output);
                converted = length;
            }
        }
    } else if (ok && codec >= 0 && !strcmp(argv[1], "bank"))
        ok = dsbk_convert(raw, length, &opts, &output, &converted, &sounds);
    else if (ok && codec >= 0) {
        size_t count = length / 2;
        int16_t *pcm = count ? malloc(count * sizeof *pcm) : NULL;
        if (count && !pcm) ok = 0;
        if (ok) {
            for (size_t i = 0; i < count; ++i)
                pcm[i] = (int16_t)((uint16_t)raw[i * 2] | (uint16_t)raw[i * 2 + 1] << 8);
            ok = dsnd_encode(pcm, count, &opts, &output, &converted,
                             &saved, &saved_length);
        }
        free(pcm);
    } else ok = 0;
    FILE *destination = ok ? fopen(argv[3], "wbx") : NULL;
    if (!destination) ok = 0;
    else {
        ok = fwrite(output, 1, converted, destination) == converted;
        if (fclose(destination)) ok = 0;
        if (!ok) remove(argv[3]);
    }
    free(raw);
    free(output);
    free(saved);
    if (!ok) fprintf(stderr, "failed to convert sound\n");
    return ok ? 0 : 1;
}
