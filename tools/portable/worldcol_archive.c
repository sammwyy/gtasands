#include "worldcol.h"

#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16;
}

static float f32(const uint8_t *p) {
    uint32_t bits = u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int section(const uint8_t *source, size_t length, uint64_t offset,
                   size_t count, size_t stride, const uint8_t **out) {
    if (offset > length || count > (length - offset) / stride) return 0;
    *out = source + (size_t)offset;
    return 1;
}

static int legacy(const uint8_t *data, size_t end, size_t *cursor,
                  size_t stride, uint32_t *count, const uint8_t **out) {
    if (*cursor > end || end - *cursor < 4) return 0;
    uint32_t amount = u32(data + *cursor);
    *cursor += 4;
    if (amount > (end - *cursor) / stride) amount = 0;
    *out = data + *cursor;
    *count = amount;
    *cursor += (size_t)amount * stride;
    return 1;
}

static int load_model(const uint8_t *source, size_t length, size_t base, size_t end,
                      worldcol_entry *entry) {
    if (end > length || end < base + 72) return 0;
    int version = source[base + 3] == 'L' ? 1 :
                  source[base + 3] >= '2' && source[base + 3] <= '4' ?
                  source[base + 3] - '0' : 0;
    if (!version) return 0;
    if (version != 1 && end < base + 108) return 0;
    size_t nname = 0;
    while (nname < 22 && source[base + 8 + nname]) {
        entry->name[nname] = (char)source[base + 8 + nname];
        ++nname;
    }
    entry->name[nname] = 0;
    const uint8_t *spheres = NULL, *boxes = NULL, *verts = NULL, *faces = NULL;
    uint32_t ns = 0, nb = 0, nv = 0, nf = 0;
    if (version == 1) {
        size_t cursor = base + 72;
        const uint8_t *ignored;
        uint32_t skipped;
        if (!legacy(source, end, &cursor, 20, &ns, &spheres) ||
            !legacy(source, end, &cursor, 24, &skipped, &ignored) ||
            !legacy(source, end, &cursor, 28, &nb, &boxes) ||
            !legacy(source, end, &cursor, 12, &nv, &verts) ||
            !legacy(source, end, &cursor, 16, &nf, &faces)) return 0;
    } else {
        ns = u16(source + base + 72);
        nb = u16(source + base + 74);
        nf = u16(source + base + 76);
        uint64_t rel = (uint64_t)base + 4;
        if (!section(source, end, rel + u32(source + base + 84), ns, 20, &spheres) ||
            !section(source, end, rel + u32(source + base + 88), nb, 28, &boxes) ||
            !section(source, end, rel + u32(source + base + 100), nf, 8, &faces))
            return 0;
        for (uint32_t i = 0; i < nf; ++i)
            for (unsigned j = 0; j < 3; ++j) {
                uint32_t id = u16(faces + (size_t)i * 8 + j * 2);
                if (id >= nv) nv = id + 1;
            }
        if (!section(source, end, rel + u32(source + base + 96), nv, 6, &verts))
            return 0;
    }
    worldcol_vec3 *points = nv ? malloc((size_t)nv * sizeof *points) : NULL;
    worldcol_face *triangles = nf ? malloc((size_t)nf * sizeof *triangles) : NULL;
    worldcol_sphere *balls = ns ? malloc((size_t)ns * sizeof *balls) : NULL;
    worldcol_box *cuboids = nb ? malloc((size_t)nb * sizeof *cuboids) : NULL;
    int ok = (!nv || points) && (!nf || triangles) && (!ns || balls) && (!nb || cuboids);
    for (uint32_t i = 0; i < nv && ok; ++i) {
        const uint8_t *p = verts + (size_t)i * (version == 1 ? 12 : 6);
        points[i] = version == 1 ?
            (worldcol_vec3){f32(p), f32(p + 4), f32(p + 8)} :
            (worldcol_vec3){(int16_t)u16(p) / 128.0,
                            (int16_t)u16(p + 2) / 128.0,
                            (int16_t)u16(p + 4) / 128.0};
    }
    for (uint32_t i = 0; i < nf && ok; ++i) {
        const uint8_t *p = faces + (size_t)i * (version == 1 ? 16 : 8);
        triangles[i] = (worldcol_face){
            version == 1 ? u32(p) : u16(p),
            version == 1 ? u32(p + 4) : u16(p + 2),
            version == 1 ? u32(p + 8) : u16(p + 4),
            p[version == 1 ? 12 : 6], p[version == 1 ? 15 : 7]
        };
    }
    for (uint32_t i = 0; i < ns && ok; ++i) {
        const uint8_t *p = spheres + (size_t)i * 20;
        balls[i].center = version == 1 ?
            (worldcol_vec3){f32(p + 4), f32(p + 8), f32(p + 12)} :
            (worldcol_vec3){f32(p), f32(p + 4), f32(p + 8)};
        balls[i].radius = f32(p + (version == 1 ? 0 : 12));
        memcpy(balls[i].surface, p + 16, 4);
    }
    for (uint32_t i = 0; i < nb && ok; ++i) {
        const uint8_t *p = boxes + (size_t)i * 28;
        cuboids[i].min = (worldcol_vec3){f32(p), f32(p + 4), f32(p + 8)};
        cuboids[i].max = (worldcol_vec3){f32(p + 12), f32(p + 16), f32(p + 20)};
        memcpy(cuboids[i].surface, p + 24, 4);
    }
    if (ok) {
        worldcol_model model = {points, nv, triangles, nf, balls, ns, cuboids, nb};
        ok = worldcol_pack(&model, &entry->record, &entry->length,
                           &entry->radius, &entry->faces);
    }
    free(points);
    free(triangles);
    free(balls);
    free(cuboids);
    return ok;
}

void worldcol_free_archive(worldcol_entry *entries, size_t count) {
    for (size_t i = 0; i < count; ++i) free(entries[i].record);
    free(entries);
}

int worldcol_parse_archive(const uint8_t *source, size_t length,
                           worldcol_entry **entries, size_t *entry_count) {
    if (!source || !entries || !entry_count) return 0;
    *entries = NULL;
    *entry_count = 0;
    worldcol_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (size_t offset = 0; offset <= length && length - offset >= 32;) {
        if (memcmp(source + offset, "COL", 3)) {
            size_t first = offset > 8 ? offset - 8 : 0;
            size_t last = offset + 16 < length ? offset + 16 : length;
            size_t next = first;
            for (; next + 3 <= last && memcmp(source + next, "COL", 3); ++next) {}
            if (next + 3 > last || next == offset) break;
            offset = next;
            continue;
        }
        size_t size = u32(source + offset + 4);
        if (size > length - offset - 8) {
            ok = 0;
            break;
        }
        if (used == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 16;
            if (next_capacity > SIZE_MAX / sizeof *result) {
                ok = 0;
                break;
            }
            worldcol_entry *next = realloc(result, next_capacity * sizeof *next);
            if (!next) {
                ok = 0;
                break;
            }
            result = next;
            capacity = next_capacity;
        }
        memset(&result[used], 0, sizeof result[used]);
        if (!load_model(source, length, offset, offset + 8 + size, &result[used])) {
            ok = 0;
            break;
        }
        ++used;
        offset += 8 + size;
    }
    if (ok) {
        *entries = result;
        *entry_count = used;
    } else worldcol_free_archive(result, used);
    return ok;
}
