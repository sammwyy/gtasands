#include "ipl.h"

#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static double read_float(const uint8_t *p) {
    uint32_t bits = read32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

int ipl_parse_binary(const uint8_t *source, size_t length,
                     ipl_instance **instances, size_t *count) {
    if (!source || !instances || !count) return 0;
    *instances = NULL;
    *count = 0;
    if (length < 4 || memcmp(source, "bnry", 4)) return 1;
    if (length < 32) return 0;
    size_t n = read32(source + 4);
    size_t start = read32(source + 28);
    if (start > length || n > (length - start) / 40) return 0;
    ipl_instance *result = n ? calloc(n, sizeof *result) : NULL;
    if (n && !result) return 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *p = source + start + i * 40;
        for (size_t j = 0; j < 7; ++j)
            result[i].transform[j] = read_float(p + j * 4);
        result[i].model = (int32_t)read32(p + 28);
        result[i].area = (int32_t)read32(p + 32);
        result[i].lod = (int32_t)read32(p + 36);
    }
    *instances = result;
    *count = n;
    return 1;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
    return text;
}

static int number(char *text, int32_t *value) {
    char *end;
    long parsed = strtol(text, &end, 10);
    if (end == text || *trim(end) || parsed < INT32_MIN || parsed > INT32_MAX) return 0;
    *value = (int32_t)parsed;
    return 1;
}

static int real(char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*trim(end);
}

int ipl_parse_text(const char *source, size_t length,
                   ipl_instance **instances, size_t *count) {
    if (!source || !instances || !count || length == SIZE_MAX) return 0;
    *instances = NULL;
    *count = 0;
    char *text = malloc(length + 1);
    if (!text) return 0;
    memcpy(text, source, length);
    text[length] = 0;
    ipl_instance *entries = NULL;
    size_t used = 0, capacity = 0;
    int in_inst = 0, ok = 1;
    for (char *line = text; line < text + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *start = trim(line);
        if (*start) {
            if (!strchr(start, ',')) {
                for (char *p = start; *p; ++p) *p = (char)tolower((unsigned char)*p);
                in_inst = strcmp(start, "inst") == 0;
            } else if (in_inst) {
                char *fields[11];
                size_t n = 0;
                fields[n++] = start;
                for (char *p = start; *p; ++p)
                    if (*p == ',') {
                        *p = 0;
                        if (n < 11) fields[n++] = p + 1;
                    }
                if (n == 11) {
                    ipl_instance item = {0};
                    ok = number(trim(fields[0]), &item.model) &&
                         number(trim(fields[2]), &item.area) &&
                         number(trim(fields[10]), &item.lod);
                    for (unsigned j = 0; j < 7 && ok; ++j)
                        ok = real(trim(fields[j + 3]), &item.transform[j]);
                    if (ok) {
                        if (used == capacity) {
                            size_t next = capacity ? capacity * 2 : 128;
                            if (next <= capacity || next > SIZE_MAX / sizeof *entries)
                                ok = 0;
                            else {
                                ipl_instance *resized = realloc(entries, next * sizeof *entries);
                                if (!resized) ok = 0;
                                else {
                                    entries = resized;
                                    capacity = next;
                                }
                            }
                        }
                        if (ok) entries[used++] = item;
                    } else ok = 1;
                }
            }
        }
        line = end ? end + 1 : text + length;
    }
    free(text);
    if (!ok) {
        free(entries);
        return 0;
    }
    *instances = entries;
    *count = used;
    return 1;
}

void ipl_rotation(double x, double y, double z, double w, double out[9]) {
    x = -x; y = -y; z = -z;
    double n = sqrt(x * x + y * y + z * z + w * w);
    if (!n) n = 1;
    x /= n; y /= n; z /= n; w /= n;
    double x2 = x + x, y2 = y + y, z2 = z + z;
    double xx = x2 * x, yx = y2 * x, zx = z2 * x;
    double yy = y2 * y, zy = z2 * y, zz = z2 * z;
    double xr = x2 * w, yr = y2 * w, zr = z2 * w;
    out[0] = 1 - (zz + yy); out[1] = zr + yx; out[2] = zx - yr;
    out[3] = yx - zr; out[4] = 1 - (zz + xx); out[5] = xr + zy;
    out[6] = yr + zx; out[7] = zy - xr; out[8] = 1 - (yy + xx);
}
