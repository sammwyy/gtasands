#include "dff.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double local[12];
    double world[12];
    int32_t parent;
    uint8_t state;
} frame;

typedef struct {
    double sphere[4];
} geometry;

typedef struct {
    uint32_t frame, geom;
} atomic;

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static double f32(const uint8_t *p) {
    uint32_t bits = u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return isfinite(value) ? value : 0.0;
}

static int chunk(const uint8_t *data, size_t end, size_t *position,
                 uint32_t *type, size_t *start, size_t *finish) {
    size_t o = *position;
    if (o > end || end - o < 12) return 0;
    *type = u32(data + o);
    *start = o + 12;
    size_t size = u32(data + o + 4);
    *finish = size > end - *start ? end : *start + size;
    if (size > SIZE_MAX - *start) return 0;
    *position = *start + size;
    return 1;
}

static int subchunk(const uint8_t *data, size_t start, size_t end,
                    uint32_t wanted, size_t *body, size_t *body_end) {
    size_t pos = start, s, e;
    uint32_t type;
    while (chunk(data, end, &pos, &type, &s, &e)) {
        if (type == wanted) {
            *body = s;
            *body_end = e;
            return 1;
        }
    }
    return 0;
}

static int parse_frames(const uint8_t *data, size_t start, size_t end,
                        frame **output, size_t *count) {
    size_t s, e;
    if (!subchunk(data, start, end, 1, &s, &e) || e - s < 4)
        return 0;
    size_t n = u32(data + s);
    if (n > (e - s - 4) / 56) return 0;
    frame *frames = n ? calloc(n, sizeof *frames) : NULL;
    if (n && !frames) return 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *p = data + s + 4 + i * 56;
        for (size_t j = 0; j < 12; ++j)
            frames[i].local[j] = f32(p + j * 4);
        frames[i].parent = (int32_t)u32(p + 48);
    }
    *output = frames;
    *count = n;
    return 1;
}

static int parse_geometry(const uint8_t *data, size_t start, size_t end,
                          geometry *result) {
    size_t s, e;
    if (!subchunk(data, start, end, 1, &s, &e) || e - s < 16) return 0;
    const uint8_t *head = data + s;
    uint32_t flags = u32(head), ntris = u32(head + 4), nverts = u32(head + 8), nmorph = u32(head + 12);
    size_t position = s + 16;
    if (!(flags & 0x01000000u)) {
        size_t textures = (flags >> 16) & 255u;
        if (!textures) textures = flags & 0x80u ? 2 : flags & 0x04u ? 1 : 0;
        size_t per_vertex = (flags & 8u ? 4 : 0) + textures * 8;
        if (nverts && per_vertex > (e - position) / nverts) return 0;
        position += nverts * per_vertex;
        if (ntris > (e - position) / 8) return 0;
        position += ntris * 8;
    }
    for (size_t i = 0; i < 4; ++i) result->sphere[i] = 0;
    if (nmorph) {
        if (position > e || e - position < 24) return 0;
        for (size_t i = 0; i < 4; ++i)
            result->sphere[i] = f32(data + position + 4 * i);
    }
    return 1;
}

static int push_atomic(atomic **list, size_t *count, size_t *capacity,
                       uint32_t frame_index, uint32_t geom_index) {
    if (*count == *capacity) {
        size_t next_size = *capacity ? *capacity * 2 : 8;
        if (next_size < *capacity || next_size > SIZE_MAX / sizeof(atomic)) return 0;
        atomic *next = realloc(*list, next_size * sizeof *next);
        if (!next) return 0;
        *list = next;
        *capacity = next_size;
    }
    (*list)[(*count)++] = (atomic){frame_index, geom_index};
    return 1;
}

static int parse(const uint8_t *data, size_t length, frame **frames, size_t *nframes,
                 geometry **geometries, size_t *ngeoms, atomic **atoms, size_t *natoms) {
    size_t clump, clump_end;
    if (!subchunk(data, 0, length, 0x10, &clump, &clump_end)) return 0;
    size_t pos = clump, start, end, capacity = 0;
    uint32_t type;
    while (chunk(data, clump_end, &pos, &type, &start, &end)) {
        if (type == 0x0e) {
            if (!parse_frames(data, start, end, frames, nframes)) return 0;
        } else if (type == 0x1a) {
            size_t gp = start, gs, ge;
            uint32_t gt;
            while (chunk(data, end, &gp, &gt, &gs, &ge)) {
                if (gt != 0x0f) continue;
                if (*ngeoms == SIZE_MAX / sizeof(geometry)) return 0;
                geometry *next = realloc(*geometries, (*ngeoms + 1) * sizeof(geometry));
                if (!next) return 0;
                *geometries = next;
                if (!parse_geometry(data, gs, ge, next + *ngeoms)) return 0;
                ++*ngeoms;
            }
        } else if (type == 0x14) {
            size_t s, e;
            if (!subchunk(data, start, end, 1, &s, &e) || e - s < 8 ||
                !push_atomic(atoms, natoms, &capacity, u32(data + s), u32(data + s + 4)))
                return 0;
        }
    }
    return 1;
}

static int matrix(frame *frames, size_t count, size_t index) {
    frame *f = &frames[index];
    if (f->state == 2) return 1;
    if (f->state == 1) return 0;
    f->state = 1;
    memcpy(f->world, f->local, sizeof f->local);
    int32_t parent = f->parent;
    if (parent >= 0 && (size_t)parent < count && (size_t)parent != index) {
        if (!matrix(frames, count, (size_t)parent)) return 0;
        const double *m = frames[parent].world;
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col)
                f->world[row * 3 + col] =
                    f->local[row * 3] * m[col] +
                    f->local[row * 3 + 1] * m[3 + col] +
                    f->local[row * 3 + 2] * m[6 + col];
        for (unsigned col = 0; col < 3; ++col)
            f->world[9 + col] =
                f->local[9] * m[col] +
                f->local[10] * m[3 + col] +
                f->local[11] * m[6 + col] + m[9 + col];
    }
    f->state = 2;
    return 1;
}

int dff_radius(const uint8_t *data, size_t length, uint16_t *radius) {
    if (!data || !radius) return 0;
    frame *frames = NULL;
    geometry *geometries = NULL;
    atomic *atoms = NULL;
    size_t nframes = 0, ngeoms = 0, natoms = 0;
    int ok = parse(data, length, &frames, &nframes, &geometries, &ngeoms, &atoms, &natoms);
    double maximum = 0;
    int found = 0;
    for (size_t i = 0; i < natoms && ok; ++i) {
        atomic a = atoms[i];
        if (a.frame >= nframes || a.geom >= ngeoms) continue;
        if (!matrix(frames, nframes, a.frame)) {
            ok = 0;
            break;
        }
        const double *m = frames[a.frame].world, *s = geometries[a.geom].sphere;
        double center[3];
        for (unsigned col = 0; col < 3; ++col)
            center[col] = s[0] * m[col] + s[1] * m[3 + col] +
                          s[2] * m[6 + col] + m[9 + col];
        double scale = 0;
        for (unsigned row = 0; row < 3; ++row) {
            double norm = sqrt(m[row * 3] * m[row * 3] +
                               m[row * 3 + 1] * m[row * 3 + 1] +
                               m[row * 3 + 2] * m[row * 3 + 2]);
            if (norm > scale) scale = norm;
        }
        double value = sqrt(center[0] * center[0] +
                            center[1] * center[1] + center[2] * center[2]) +
                       s[3] * scale;
        if (!found || value > maximum) maximum = value;
        found = 1;
    }
    if (ok) {
        if (!found) maximum = 8;
        *radius = (uint16_t)(maximum > 65535 ? 65535 :
                             maximum < 1 ? 1 : ceil(maximum));
    }
    free(frames);
    free(geometries);
    free(atoms);
    return ok;
}
