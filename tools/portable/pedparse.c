#include "pedparse.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static int equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return !*a && !*b;
}

static int label(char *out, size_t length, const char *text, int upper) {
    size_t n = strlen(text);
    if (n >= length) return 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = (char)(upper ? toupper((unsigned char)text[i]) :
                               tolower((unsigned char)text[i]));
    out[n] = 0;
    return 1;
}

static int number(const char *text, int base, int32_t *value) {
    char *end;
    long parsed = strtol(text, &end, base);
    if (end == text || *end || parsed < INT32_MIN || parsed > INT32_MAX) return 0;
    *value = (int32_t)parsed;
    return 1;
}

static int unsigned_number(const char *text, int base, uint32_t *value) {
    char *end;
    unsigned long long parsed = strtoull(text, &end, base);
    if (end == text || *end || parsed > UINT32_MAX) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int decimal(const char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*end;
}

static int grow(void **entries, size_t *capacity, size_t count, size_t element) {
    if (count < *capacity) return 1;
    if (*capacity > SIZE_MAX / 2) return 0;
    size_t next_capacity = *capacity ? *capacity * 2 : 128;
    if (next_capacity > SIZE_MAX / element) return 0;
    void *next = realloc(*entries, next_capacity * element);
    if (!next) return 0;
    *capacity = next_capacity;
    *entries = next;
    return 1;
}

static int open_text(const char *text, size_t length, char **copy) {
    if (!text || length == SIZE_MAX) return 0;
    *copy = malloc(length + 1);
    if (!*copy) return 0;
    memcpy(*copy, text, length);
    (*copy)[length] = 0;
    return 1;
}

int pedparse_ide(const char *text, size_t length,
                 pedparse_ide_entry **entries, size_t *count) {
    if (!entries || !count) return 0;
    *entries = NULL; *count = 0;
    char *copy;
    if (!open_text(text, length, &copy)) return 0;
    pedparse_ide_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int active = 0, ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = trim(line);
        if (*start && !strchr(start, ',')) active = equal(start, "peds");
        else if (active && *start) {
            char *tokens[32];
            size_t n = 0;
            tokens[n++] = start;
            for (char *p = start; *p && n < 32; ++p)
                if (*p == ',') {
                    *p = 0;
                    tokens[n++] = p + 1;
                }
            if (n >= 8) {
                for (size_t i = 0; i < n; ++i) tokens[i] = trim(tokens[i]);
                pedparse_ide_entry entry = {0};
                int valid = number(tokens[0], 10, &entry.id) &&
                            label(entry.model, sizeof entry.model, tokens[1], 0) &&
                            label(entry.txd, sizeof entry.txd, tokens[2], 0) &&
                            label(entry.type, sizeof entry.type, tokens[3], 1) &&
                            label(entry.stat, sizeof entry.stat, tokens[4], 1) &&
                            label(entry.anim, sizeof entry.anim, tokens[5], 0) &&
                            (!*tokens[6] || unsigned_number(tokens[6], 16, &entry.cars)) &&
                            (!*tokens[7] || unsigned_number(tokens[7], 0, &entry.flags));
                if (valid && strcmp(entry.model, "null")) {
                    if (!grow((void **)&result, &capacity, used, sizeof *result)) ok = 0;
                    else result[used++] = entry;
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

int pedparse_stats(const char *text, size_t length,
                   pedparse_stat_entry **entries, size_t *count) {
    if (!entries || !count) return 0;
    *entries = NULL; *count = 0;
    char *copy;
    if (!open_text(text, length, &copy)) return 0;
    pedparse_stat_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *tokens[11];
        size_t n = 0;
        for (char *p = line; *p && n < 11;) {
            while (isspace((unsigned char)*p)) ++p;
            if (!*p) break;
            tokens[n++] = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = 0;
        }
        int stat = n >= 10 && strlen(tokens[0]) >= 5;
        for (unsigned i = 0; i < 5 && stat; ++i)
            if (toupper((unsigned char)tokens[0][i]) != "STAT_"[i]) stat = 0;
        if (stat) {
            pedparse_stat_entry entry = {0};
            int valid = label(entry.name, sizeof entry.name, tokens[0], 1);
            for (size_t i = 1; i < n && valid; ++i)
                valid = decimal(tokens[i], &entry.values[i - 1]);
            if (valid) {
                if (!grow((void **)&result, &capacity, used, sizeof *result)) ok = 0;
                else result[used++] = entry;
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

int pedparse_zones(const char *text, size_t length,
                   pedparse_zone_entry **entries, size_t *count) {
    if (!entries || !count) return 0;
    *entries = NULL; *count = 0;
    char *copy;
    if (!open_text(text, length, &copy)) return 0;
    pedparse_zone_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *tokens[16];
        size_t n = 0;
        tokens[n++] = trim(line);
        for (char *p = line; *p && n < 16; ++p)
            if (*p == ',') {
                *p = 0;
                tokens[n++] = p + 1;
            }
        if (n >= 9) {
            for (size_t i = 0; i < n; ++i) tokens[i] = trim(tokens[i]);
            int32_t type;
            double x1, y1, x2, y2;
            if (number(tokens[1], 10, &type) &&
                decimal(tokens[2], &x1) && decimal(tokens[3], &y1) &&
                decimal(tokens[5], &x2) && decimal(tokens[6], &y2) &&
                (type == 0 || type == 1)) {
                pedparse_zone_entry entry = {0};
                if (label(entry.name, sizeof entry.name, tokens[0], 1)) {
                    entry.bounds[0] = x1 < x2 ? x1 : x2;
                    entry.bounds[1] = y1 < y2 ? y1 : y2;
                    entry.bounds[2] = x1 > x2 ? x1 : x2;
                    entry.bounds[3] = y1 > y2 ? y1 : y2;
                    if (!grow((void **)&result, &capacity, used, sizeof *result)) ok = 0;
                    else result[used++] = entry;
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

int pedparse_animgrp(const char *text, size_t length,
                     pedparse_anim_event **events, size_t *count) {
    if (!events || !count) return 0;
    *events = NULL; *count = 0;
    char *copy;
    if (!open_text(text, length, &copy)) return 0;
    pedparse_anim_event *result = NULL;
    size_t used = 0, capacity = 0;
    int active = 0, ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = trim(line);
        if (strchr(start, ',')) {
            char *a = start, *b = strchr(start, ',');
            *b++ = 0;
            char *c = strchr(b, ',');
            active = 0;
            if (c) {
                *c++ = 0;
                char *last = strchr(c, ',');
                if (last) *last = 0;
                if (equal(trim(c), "walkcycle")) {
                    pedparse_anim_event event = {0};
                    if (label(event.name, sizeof event.name, trim(a), 0)) {
                        if (!grow((void **)&result, &capacity, used, sizeof *result)) ok = 0;
                        else {
                            result[used++] = event;
                            active = 1;
                        }
                    }
                }
            }
        } else if (active && *start) {
            pedparse_anim_event event = {0};
            if (equal(start, "end")) {
                event.kind = 2;
                active = 0;
            } else {
                event.kind = 1;
                char *tail = start;
                while (*tail && !isspace((unsigned char)*tail)) ++tail;
                *tail = 0;
                if (strlen(start) >= sizeof event.name) {
                    line = end ? end + 1 : copy + length;
                    continue;
                }
                memcpy(event.name, start, strlen(start) + 1);
            }
            if (!grow((void **)&result, &capacity, used, sizeof *result)) ok = 0;
            else result[used++] = event;
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *events = result;
        *count = used;
    } else free(result);
    return ok;
}

static const pedparse_named_model *find_model(const pedparse_named_model *models,
                                               size_t count, const char *name) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(models[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && !strcmp(models[lo].name, name) ? &models[lo] : NULL;
}

int pedparse_groups(const char *text, size_t length,
                    const pedparse_named_model *models, size_t model_count,
                    uint16_t max_groups, int skip_slashes,
                    pedparse_group_member **members, size_t *member_count,
                    uint16_t *group_count) {
    if (!members || !member_count || !group_count ||
        (model_count && !models)) return 0;
    *members = NULL;
    *member_count = 0;
    *group_count = 0;
    char *copy;
    if (!open_text(text, length, &copy)) return 0;
    pedparse_group_member *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = copy; line < copy + length && *group_count < max_groups && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = trim(line);
        if (*start && !(skip_slashes && start[0] == '/' && start[1] == '/')) {
            int any = 0;
            for (char *p = start; *p;) {
                while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
                if (!*p) break;
                char *word = p;
                while (*p && !isspace((unsigned char)*p) && *p != ',') {
                    *p = (char)tolower((unsigned char)*p);
                    ++p;
                }
                char delimiter = *p;
                *p = 0;
                any = 1;
                const pedparse_named_model *model = find_model(models, model_count, word);
                if (model) {
                    if (!grow((void **)&result, &capacity, used, sizeof *result)) {
                        ok = 0;
                        break;
                    }
                    result[used++] = (pedparse_group_member){*group_count, model->index};
                }
                if (delimiter) ++p;
            }
            if (any) ++*group_count;
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *members = result;
        *member_count = used;
    } else free(result);
    return ok;
}
