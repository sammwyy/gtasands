#include "dspk.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const dspk_entry *entry;
    uint32_t hash;
    uint32_t name_offset;
    size_t index;
} item;

static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

static int order(const void *a, const void *b) {
    const item *x = a, *y = b;
    if (x->hash != y->hash)
        return x->hash < y->hash ? -1 : 1;
    const unsigned char *p = (const unsigned char *)x->entry->name;
    const unsigned char *q = (const unsigned char *)y->entry->name;
    while (*p && *q && lower(*p) == lower(*q))
        ++p, ++q;
    if (lower(*p) != lower(*q))
        return lower(*p) < lower(*q) ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static int put32(dspk_write_fn write_fn, void *context, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                              (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return write_fn(context, bytes, sizeof bytes);
}

static int zeros(dspk_write_fn write_fn, void *context, size_t size) {
    static const unsigned char empty[4] = {0};
    return !size || write_fn(context, empty, size);
}

int dspk_write(const dspk_entry *entries, size_t count, dspk_read_fn read_fn,
               dspk_write_fn write_fn, void *context) {
    if ((!entries && count) || !read_fn || !write_fn ||
        count > (UINT32_MAX - 16u) / 16u || count > SIZE_MAX / sizeof(item))
        return 0;
    item *items = count ? malloc(count * sizeof *items) : NULL;
    if (count && !items)
        return 0;
    uint64_t names_size = 0;
    int ok = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *p = (const unsigned char *)entries[i].name;
        if (!p || !*p)
            goto done;
        uint32_t hash = 0x811c9dc5u;
        size_t length = 0;
        for (; *p; ++p, ++length) {
            if (*p > 127)
                goto done;
            hash = (hash ^ lower(*p)) * 0x01000193u;
        }
        if (names_size + length + 1 > UINT32_MAX)
            goto done;
        items[i] = (item){&entries[i], hash, 0, i};
        names_size += length + 1;
    }
    qsort(items, count, sizeof *items, order);
    uint64_t names_off = 16u + count * 16u;
    uint64_t data_off = names_off + ((names_size + 3u) & ~3u);
    if (data_off > UINT32_MAX)
        goto done;
    uint64_t pos = data_off;
    for (size_t i = 0; i < count; ++i) {
        pos = (pos + 3u) & ~3u;
        pos += items[i].entry->size;
        if (pos > UINT32_MAX)
            goto done;
    }
    unsigned char hdr[8] = {'D', 'S', 'P', 'K', 1, 0, 0, 0};
    if (!write_fn(context, hdr, sizeof hdr) ||
        !put32(write_fn, context, (uint32_t)count) ||
        !put32(write_fn, context, (uint32_t)names_off))
        goto done;
    uint32_t name_pos = 0;
    pos = data_off;
    for (size_t i = 0; i < count; ++i) {
        pos = (pos + 3u) & ~3u;
        items[i].name_offset = name_pos;
        name_pos += (uint32_t)strlen(items[i].entry->name) + 1u;
        if (!put32(write_fn, context, items[i].hash) ||
            !put32(write_fn, context, items[i].name_offset) ||
            !put32(write_fn, context, (uint32_t)pos) ||
            !put32(write_fn, context, items[i].entry->size))
            goto done;
        pos += items[i].entry->size;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t length = strlen(items[i].entry->name) + 1;
        if (!write_fn(context, items[i].entry->name, length))
            goto done;
    }
    if (!zeros(write_fn, context, (size_t)((4u - names_size % 4u) % 4u)))
        goto done;
    uint64_t cursor = data_off;
    unsigned char buffer[65536];
    for (size_t i = 0; i < count; ++i) {
        size_t padding = (size_t)((4u - cursor % 4u) % 4u);
        if (!zeros(write_fn, context, padding))
            goto done;
        cursor += padding;
        uint32_t offset = 0;
        while (offset < items[i].entry->size) {
            size_t chunk = items[i].entry->size - offset;
            if (chunk > sizeof buffer)
                chunk = sizeof buffer;
            if (!read_fn(context, items[i].entry->source, offset, buffer, chunk) ||
                !write_fn(context, buffer, chunk))
                goto done;
            offset += (uint32_t)chunk;
            cursor += chunk;
        }
    }
    ok = 1;
done:
    free(items);
    return ok;
}
