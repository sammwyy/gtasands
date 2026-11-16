#ifndef GTASADS_DSPK_H
#define GTASADS_DSPK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t size;
    void *source;
} dspk_entry;

typedef int (*dspk_read_fn)(void *context, void *source, uint32_t offset, void *data, size_t size);
typedef int (*dspk_write_fn)(void *context, const void *data, size_t size);

int dspk_write(const dspk_entry *entries, size_t count, dspk_read_fn read_fn,
               dspk_write_fn write_fn, void *context);

#endif
