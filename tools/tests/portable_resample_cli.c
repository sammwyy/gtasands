#include "../portable/resample.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    char *end;
    unsigned long source_rate = strtoul(argv[3], &end, 10);
    if (*end || !source_rate || source_rate > UINT32_MAX) return 2;
    unsigned long target_rate = strtoul(argv[4], &end, 10);
    if (*end || !target_rate || target_rate > UINT32_MAX) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 1;
    if (fseek(input, 0, SEEK_END)) return 1;
    long size = ftell(input);
    if (size < 0 || size % 2 || fseek(input, 0, SEEK_SET)) return 1;
    int16_t *samples = malloc(size ? (size_t)size : 1);
    if (!samples) return 1;
    if (fread(samples, 1, (size_t)size, input) != (size_t)size ||
        fclose(input)) return 1;
    int16_t *output = NULL;
    size_t count = 0;
    if (!resample_pcm16(samples, (size_t)size / 2,
                        (uint32_t)source_rate, (uint32_t)target_rate,
                        &output, &count)) return 1;
    FILE *file = fopen(argv[2], "wbx");
    if (!file) return 1;
    int ok = fwrite(output, sizeof *output, count, file) == count;
    if (fclose(file)) ok = 0;
    free(output);
    free(samples);
    return ok ? 0 : 1;
}
