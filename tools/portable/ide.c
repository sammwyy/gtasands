#include "ide.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *data;
    size_t count, capacity;
} list;

static int append(list *dst, const void *item, size_t unit) {
    if (dst->count == dst->capacity) {
        size_t cap = dst->capacity ? dst->capacity * 2 : 32;
        if (cap <= dst->capacity || cap > SIZE_MAX / unit) return 0;
        void *next = realloc(dst->data, cap * unit);
        if (!next) return 0;
        dst->data = next;
        dst->capacity = cap;
    }
    memcpy((uint8_t *)dst->data + dst->count++ * unit, item, unit);
    return 1;
}

static char *strip(char *value) {
    while (isspace((unsigned char)*value)) ++value;
    size_t n = strlen(value);
    while (n && isspace((unsigned char)value[n - 1])) value[--n] = 0;
    return value;
}

static int integer(char *text, int64_t *value) {
    char *end;
    *value = strtoll(text, &end, 10);
    return end != text && !*strip(end);
}

static int floating(char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*strip(end);
}

static int copy_lower(char *target, size_t size, char *source) {
    size_t n = strlen(source);
    if (n >= size) return 0;
    for (size_t i = 0; i < n; ++i)
        target[i] = (char)tolower((unsigned char)source[i]);
    target[n] = 0;
    return 1;
}

int ide_parse(const char *source, size_t length, ide_object **objects, size_t *object_count,
              ide_parent **parents, size_t *parent_count) {
    if (!source || !objects || !object_count || !parents || !parent_count ||
        length == SIZE_MAX) return 0;
    *objects = NULL; *object_count = 0;
    *parents = NULL; *parent_count = 0;
    char *text = malloc(length + 1);
    if (!text) return 0;
    memcpy(text, source, length);
    text[length] = 0;
    list models = {0}, links = {0};
    enum {NONE, OBJS, TOBJ, ANIM, TXDP} section = NONE;
    int ok = 1;
    for (char *line = text; line < text + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = strip(line);
        if (*start) {
            if (!strchr(start, ',')) {
                for (char *p = start; *p; ++p)
                    *p = (char)tolower((unsigned char)*p);
                if (!strcmp(start, "objs")) section = OBJS;
                else if (!strcmp(start, "tobj")) section = TOBJ;
                else if (!strcmp(start, "anim")) section = ANIM;
                else if (!strcmp(start, "txdp")) section = TXDP;
                else if (!strcmp(start, "end") || !strcmp(start, "path") ||
                         !strcmp(start, "2dfx") || !strcmp(start, "weap") ||
                         !strcmp(start, "hier") || !strcmp(start, "cars") ||
                         !strcmp(start, "peds")) section = NONE;
            } else {
                char *parts[128];
                size_t count = 0;
                parts[count++] = start;
                for (char *p = start; *p; ++p)
                    if (*p == ',') {
                        *p = 0;
                        if (count < 128) parts[count++] = p + 1;
                    }
                for (size_t i = 0; i < count; ++i) parts[i] = strip(parts[i]);
                if (section == TXDP && count >= 2) {
                    ide_parent item = {0};
                    if (copy_lower(item.name, sizeof item.name, parts[0]) &&
                        copy_lower(item.parent, sizeof item.parent, parts[1]))
                        ok = append(&links, &item, sizeof item);
                } else if (((section == OBJS || section == TOBJ) && count >= 5) ||
                           (section == ANIM && count >= 6)) {
                    ide_object item = {0};
                    int64_t id, flags, on = 24, off = 24, n = 0;
                    size_t total = count;
                    int valid = 1;
                    if (section == TOBJ) {
                        valid = integer(parts[count - 2], &on) &&
                                integer(parts[count - 1], &off);
                        total -= 2;
                    }
                    if (valid && section != ANIM && total != 5)
                        valid = total > 4 && integer(parts[3], &n) &&
                                n >= 0 && n < INT32_MAX && (uint64_t)n < total - 4;
                    size_t distance = section == ANIM ? 4 : total == 5 ? 3 : 4;
                    size_t flag = section == ANIM ? 5 : total == 5 ? 4 : 4 + (size_t)n;
                    if (valid && distance < total && flag < total &&
                        integer(parts[0], &id) && id >= INT32_MIN && id <= INT32_MAX &&
                        integer(parts[flag], &flags) &&
                        floating(parts[distance], &item.draw_distance) &&
                        on >= INT32_MIN && on <= INT32_MAX &&
                        off >= INT32_MIN && off <= INT32_MAX &&
                        copy_lower(item.name, sizeof item.name, parts[1]) &&
                        copy_lower(item.txd, sizeof item.txd, parts[2])) {
                        item.id = (int32_t)id;
                        item.flags = flags;
                        item.on = (int32_t)on;
                        item.off = (int32_t)off;
                        ok = append(&models, &item, sizeof item);
                    }
                }
            }
        }
        line = end ? end + 1 : text + length;
    }
    free(text);
    if (!ok) {
        free(models.data);
        free(links.data);
        return 0;
    }
    *objects = models.data;
    *object_count = models.count;
    *parents = links.data;
    *parent_count = links.count;
    return 1;
}
