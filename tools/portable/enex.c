#include "enex.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
    return text;
}

static int equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return !*a && !*b;
}

static int number(const char *s, int32_t *v) {
    char *end;
    long n = strtol(s, &end, 10);
    if (end == s || *end || n < INT32_MIN || n > INT32_MAX) return 0;
    *v = (int32_t)n;
    return 1;
}

static int real(const char *s, double *v) {
    char *end;
    *v = strtod(s, &end);
    return end != s && !*end;
}

int enex_parse(const char *text, size_t length, const ipl_instance *placed,
                size_t placed_count, enex_entry **entries, size_t *count) {
    if (!text || !entries || !count || (placed_count && !placed) ||
        length == SIZE_MAX) return 0;
    *entries = NULL; *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    enex_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int active = 0, ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = trim(line);
        if (equal(start, "enex")) active = 1;
        else if (equal(start, "end")) active = 0;
        else if (active && *start) {
            char *fields[18];
            size_t n = 0;
            fields[n++] = start;
            for (char *p = start; *p; ++p) {
                if (*p == ',') {
                    *p = 0;
                    if (n < 18) fields[n++] = p + 1;
                    else ++n;
                }
            }
            if (n == 18) {
                enex_entry entry = {0};
                int valid = 1;
                for (size_t i = 0; i < 18; ++i)
                    fields[i] = trim(fields[i]);
                for (size_t i = 0; i < 11 && valid; ++i)
                    valid = real(fields[i], &entry.values[i]);
                if (valid) {
                    valid = number(fields[11], &entry.area) &&
                            number(fields[12], &entry.flags) &&
                            number(fields[14], &entry.sky) &&
                            number(fields[15], &entry.peds) &&
                            number(fields[16], &entry.on) &&
                            number(fields[17], &entry.off);
                }
                if (valid) {
                    char *name = fields[13];
                    size_t len = strlen(name);
                    while (len && name[len - 1] == '"') name[--len] = 0;
                    while (*name == '"') ++name;
                    if (strlen(name) >= sizeof entry.name) valid = 0;
                    else {
                        for (size_t i = 0; name[i]; ++i)
                            entry.name[i] = (char)tolower((unsigned char)name[i]);
                    }
                }
                if (valid) {
                    double distance = 0;
                    int best = 0, found = 0;
                    for (size_t i = 0; i < placed_count; ++i) {
                        int area = placed[i].area & 0xff;
                        if (area == 0 || area == 13) continue;
                        double dx = placed[i].transform[0] - entry.values[0];
                        double dy = placed[i].transform[1] - entry.values[1];
                        double dz = placed[i].transform[2] - entry.values[2];
                        double d = dx * dx + dy * dy + dz * dz;
                        if (!found || d < distance || (d == distance && area < best)) {
                            distance = d;
                            best = area;
                            found = 1;
                        }
                    }
                    entry.source_area = (uint16_t)(found && distance < 144 ? best :
                        entry.values[2] > 900 && entry.area != 0 && entry.area != 13 ?
                        entry.area : 0);
                    if (used == capacity) {
                        size_t next_capacity = capacity ? capacity * 2 : 128;
                        if (next_capacity <= capacity ||
                            next_capacity > SIZE_MAX / sizeof *result) {
                            ok = 0;
                            break;
                        }
                        enex_entry *next = realloc(result, next_capacity * sizeof *next);
                        if (!next) {
                            ok = 0;
                            break;
                        }
                        result = next;
                        capacity = next_capacity;
                    }
                    result[used++] = entry;
                }
            }
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *entries = result;
        *count = used;
    } else free(result);
    return ok;
}

static void put16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static void put32(uint8_t *p, uint32_t n) {
    put16(p, (uint16_t)n);
    put16(p + 2, (uint16_t)(n >> 16));
}

int enex_pack(enex_entry *entries, size_t count, uint8_t **output,
               size_t *length, uint32_t *exterior, uint32_t *linked) {
    if ((count && !entries) || !output || !length || !exterior || !linked ||
        count > UINT16_MAX) return 0;
    for (size_t i = 0; i < count; ++i) {
        enex_entry *entry = &entries[i];
        if (entry->source_area || entry->values[2] < 900 || !entry->name[0])
            continue;
        for (size_t j = 0; j < count; ++j) {
            const enex_entry *candidate = &entries[j];
            if (candidate->source_area == 0 &&
                !strcmp(entry->name, candidate->name) &&
                candidate->area != 0 && candidate->area != 13) {
                entry->source_area = (uint16_t)candidate->area;
                break;
            }
        }
    }
    uint8_t *result = malloc(8 + count * 64);
    if (!result) return 0;
    memcpy(result, "DENX", 4);
    put16(result + 4, 3);
    put16(result + 6, (uint16_t)count);
    *exterior = *linked = 0;
    for (size_t i = 0; i < count; ++i) {
        const enex_entry *e = &entries[i];
        uint8_t *out = result + 8 + i * 64;
        for (size_t j = 0; j < 11; ++j) {
            float v = (float)e->values[j];
            uint32_t bits;
            memcpy(&bits, &v, sizeof bits);
            put32(out + j * 4, bits);
        }
        put16(out + 44, (uint16_t)e->flags);
        put16(out + 46, (uint16_t)e->area);
        memset(out + 48, 0, 8);
        for (size_t j = 0; j < 8 && e->name[j]; ++j) {
            unsigned char ch = (unsigned char)e->name[j];
            out[48 + j] = ch <= 127 ? ch : '?';
        }
        out[56] = (uint8_t)e->sky;
        out[57] = (uint8_t)e->peds;
        out[58] = (uint8_t)e->on;
        out[59] = (uint8_t)e->off;
        put16(out + 60, e->source_area);
        uint16_t link = UINT16_MAX;
        if (e->name[0]) {
            for (size_t j = 0; j < count; ++j) {
                const enex_entry *other = &entries[j];
                if (j != i && !strcmp(other->name, e->name) &&
                    other->source_area != e->source_area &&
                    ((e->flags ^ other->flags) & 4)) {
                    link = (uint16_t)j;
                    break;
                }
            }
        }
        put16(out + 62, link);
        *exterior += e->source_area == 0;
        *linked += link != UINT16_MAX;
    }
    *output = result;
    *length = 8 + count * 64;
    return 1;
}
