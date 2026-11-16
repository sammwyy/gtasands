#include "col.h"
#include "cluster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} buffer;

typedef struct {
    const uint8_t *spheres;
    const uint8_t *boxes;
    const uint8_t *verts;
    const uint8_t *faces;
    uint32_t sphere_count, box_count, vert_count, face_count;
    float bounds[10];
    int version;
    int model_id;
} model;

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float f32(const uint8_t *p) {
    uint32_t bits = u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int32_t fixed(float value) {
    double scaled = (double)value * 4096.0;
    if (!isfinite(scaled)) return 0;
    if (scaled <= -2147483648.0) return INT32_MIN;
    if (scaled >= 2147483647.0) return INT32_MAX;
    double whole;
    double frac = modf(scaled, &whole);
    int64_t result = (int64_t)whole;
    if (frac > 0.5 || (frac == 0.5 && result % 2)) ++result;
    if (frac < -0.5 || (frac == -0.5 && result % 2)) --result;
    return (int32_t)result;
}

static int16_t vertex(double value) {
    double scaled = (double)value * 128;
    if (scaled <= -32768.0) return -32768;
    if (scaled >= 32767.0) return 32767;
    double whole;
    double frac = modf(scaled, &whole);
    int result = (int)whole;
    if (frac > 0.5 || (frac == 0.5 && result % 2)) ++result;
    if (frac < -0.5 || (frac == -0.5 && result % 2)) --result;
    return (int16_t)result;
}

static int append(buffer *b, const void *data, size_t count) {
    if (count > SIZE_MAX - b->length) return 0;
    size_t need = b->length + count;
    if (need > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 1024;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        uint8_t *next = realloc(b->bytes, cap);
        if (!next) return 0;
        b->bytes = next;
        b->capacity = cap;
    }
    if (count) memcpy(b->bytes + b->length, data, count);
    b->length = need;
    return 1;
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static int append32(buffer *b, uint32_t value) {
    uint8_t bytes[4];
    put32(bytes, value);
    return append(b, bytes, 4);
}

static int section(const uint8_t *data, size_t size, uint32_t relative,
                   uint32_t count, size_t stride, const uint8_t **result) {
    if (relative > size || count > (size - relative) / stride) return 0;
    *result = data + relative;
    return 1;
}

static int legacy_count(const uint8_t *data, size_t limit, size_t *offset,
                        size_t stride, uint32_t *count, const uint8_t **section_start) {
    if (*offset > limit || limit - *offset < 4) return 0;
    uint32_t n = u32(data + *offset);
    *offset += 4;
    if (n > (limit - *offset) / stride) n = 0;
    *section_start = data + *offset;
    *count = n;
    *offset += (size_t)n * stride;
    return 1;
}

static int parse_model(const uint8_t *data, size_t size, size_t base,
                       size_t end, model *m) {
    if (base > size || size - base < 32 || end > size || end < base + 32)
        return 0;
    const uint8_t *p = data + base;
    memset(m, 0, sizeof *m);
    m->version = p[3] == 'L' ? 1 : p[3] >= '2' && p[3] <= '4' ? p[3] - '0' : 0;
    if (!m->version) return 0;
    m->model_id = u16(p + 30);
    size_t o = base + 32;
    if (size - o < 40) return 0;
    float raw[10];
    for (unsigned i = 0; i < 10; ++i) raw[i] = f32(data + o + i * 4);
    if (m->version == 1) {
        for (unsigned i = 0; i < 3; ++i) {
            m->bounds[i] = raw[i + 4];
            m->bounds[i + 3] = raw[i + 7];
            m->bounds[i + 6] = raw[i + 1];
        }
        m->bounds[9] = raw[0];
        o += 40;
        const uint8_t *ignored;
        if (!legacy_count(data, end, &o, 20, &m->sphere_count, &m->spheres) ||
            !legacy_count(data, end, &o, 24, &(uint32_t){0}, &ignored) ||
            !legacy_count(data, end, &o, 28, &m->box_count, &m->boxes) ||
            !legacy_count(data, end, &o, 12, &m->vert_count, &m->verts) ||
            !legacy_count(data, end, &o, 16, &m->face_count, &m->faces))
            return 0;
        return 1;
    }
    memcpy(m->bounds, raw, sizeof raw);
    if (size - o < 76) return 0;
    m->sphere_count = u16(data + o + 40);
    m->box_count = u16(data + o + 42);
    m->face_count = u16(data + o + 44);
    uint32_t sphere_offset = u32(data + o + 52);
    uint32_t box_offset = u32(data + o + 56);
    uint32_t vert_offset = u32(data + o + 64);
    uint32_t face_offset = u32(data + o + 68);
    const uint8_t *relative = data + base + 4;
    size_t room = size - base - 4;
    if (!section(relative, room, sphere_offset, m->sphere_count, 20, &m->spheres) ||
        !section(relative, room, box_offset, m->box_count, 28, &m->boxes) ||
        !section(relative, room, face_offset, m->face_count, 8, &m->faces))
        return 0;
    uint32_t max_vertex = 0;
    for (uint32_t i = 0; i < m->face_count; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            unsigned index = u16(m->faces + i * 8 + j * 2);
            if (index >= max_vertex) max_vertex = index + 1;
        }
    m->vert_count = max_vertex;
    return section(relative, room, vert_offset, m->vert_count, 6, &m->verts);
}

static int emit_model(buffer *b, const uint8_t *input, size_t base,
                      const model *m, int mode, double ratio,
                      uint32_t min_faces, uint32_t *written_faces) {
    int simple = mode == 2 && m->face_count > 0;
    cluster_result reduced = {0};
    cluster_vertex *vertices = NULL;
    cluster_face *faces_in = NULL;
    uint32_t *ordered = NULL, *remap = NULL;
    uint8_t *used = NULL;
    uint32_t verts = mode == 1 ? 0 : m->vert_count;
    uint32_t faces = mode == 1 ? 0 : m->face_count;
    int ok = 0;
    if (simple) {
        vertices = malloc((size_t)m->vert_count * sizeof *vertices);
        faces_in = malloc((size_t)m->face_count * sizeof *faces_in);
        ordered = malloc((size_t)m->vert_count * sizeof *ordered);
        remap = malloc((size_t)m->vert_count * sizeof *remap);
        used = calloc(m->vert_count, 1);
        if (!vertices || !faces_in || !ordered || !remap || !used)
            goto cleanup;
        for (uint32_t v = 0; v < m->vert_count; ++v) {
            const uint8_t *p = m->verts + (size_t)v * (m->version == 1 ? 12 : 6);
            vertices[v] = m->version == 1 ?
                          (cluster_vertex){f32(p), f32(p + 4), f32(p + 8)} :
                          (cluster_vertex){(int16_t)u16(p) / 128.0,
                                           (int16_t)u16(p + 2) / 128.0,
                                           (int16_t)u16(p + 4) / 128.0};
        }
        for (uint32_t f = 0; f < m->face_count; ++f) {
            const uint8_t *p = m->faces + (size_t)f * (m->version == 1 ? 16 : 8);
            faces_in[f] = (cluster_face){
                m->version == 1 ? u32(p) : u16(p),
                m->version == 1 ? u32(p + 4) : u16(p + 2),
                m->version == 1 ? u32(p + 8) : u16(p + 4),
                (uint16_t)(p[m->version == 1 ? 12 : 6] |
                           (uint16_t)p[m->version == 1 ? 15 : 7] << 8)
            };
        }
        uint32_t target = (uint32_t)(m->face_count * ratio);
        if (min_faces > target) target = min_faces;
        if (!cluster_decimate(vertices, m->vert_count, faces_in, m->face_count,
                              target, &reduced))
            goto cleanup;
        faces = (uint32_t)reduced.count;
        for (size_t f = 0; f < reduced.count; ++f) {
            cluster_face t = faces_in[reduced.kept[f]];
            used[t.a] = used[t.b] = used[t.c] = 1;
        }
        verts = 0;
        for (uint32_t v = 0; v < m->vert_count; ++v)
            if (used[v]) {
                remap[v] = verts;
                ordered[verts++] = v;
            }
    }
    size_t start = b->length;
    uint8_t zero[4] = {0};
    if (!append(b, zero, 4)) goto cleanup;
    uint8_t name[24] = {0};
    size_t i = 0;
    for (; i < 23 && input[base + 8 + i]; ++i)
        name[i] = input[base + 8 + i];
    if (!append(b, name, 24)) goto cleanup;
    uint8_t header[4];
    put16(header, (uint16_t)m->model_id);
    header[2] = (uint8_t)m->version;
    header[3] = 0;
    if (!append(b, header, 4)) goto cleanup;
    for (unsigned k = 0; k < 10; ++k)
        if (!append32(b, (uint32_t)fixed(m->bounds[k]))) goto cleanup;
    uint8_t counts[8];
    if (m->sphere_count > UINT16_MAX || m->box_count > UINT16_MAX ||
        verts > UINT16_MAX || faces > UINT16_MAX) goto cleanup;
    put16(counts, (uint16_t)m->sphere_count);
    put16(counts + 2, (uint16_t)m->box_count);
    put16(counts + 4, (uint16_t)verts);
    put16(counts + 6, (uint16_t)faces);
    if (!append(b, counts, sizeof counts)) goto cleanup;
    for (uint32_t s = 0; s < m->sphere_count; ++s) {
        const uint8_t *src = m->spheres + s * 20;
        for (unsigned k = 0; k < 4; ++k)
            if (!append32(b, (uint32_t)fixed(f32(src + (m->version == 1 ?
                          (k + 1) % 4 : k) * 4)))) goto cleanup;
        if (!append(b, src + 16, 4)) goto cleanup;
    }
    for (uint32_t s = 0; s < m->box_count; ++s) {
        const uint8_t *src = m->boxes + s * 28;
        for (unsigned k = 0; k < 6; ++k)
            if (!append32(b, (uint32_t)fixed(f32(src + k * 4)))) goto cleanup;
        if (!append(b, src + 24, 4)) goto cleanup;
    }
    for (uint32_t v = 0; v < verts; ++v) {
        uint8_t coords[6];
        uint32_t original = simple ? ordered[v] : v;
        const cluster_vertex *point = simple ?
            (reduced.representatives ? &reduced.representatives[original] :
             &vertices[original]) : NULL;
        for (unsigned k = 0; k < 3; ++k) {
            int16_t value = simple ?
                vertex(k == 0 ? point->x : k == 1 ? point->y : point->z) :
                m->version == 1 ?
                    vertex(f32(m->verts + v * 12 + k * 4)) :
                    (int16_t)u16(m->verts + v * 6 + k * 2);
            put16(coords + 2 * k, (uint16_t)value);
        }
        if (!append(b, coords, 6)) goto cleanup;
    }
    if (verts & 1u) {
        uint8_t padding[2] = {0};
        if (!append(b, padding, 2)) goto cleanup;
    }
    for (uint32_t f = 0; f < faces; ++f) {
        const uint8_t *src = m->faces + (size_t)(simple ? reduced.kept[f] : f) *
                            (m->version == 1 ? 16 : 8);
        uint8_t record[8];
        for (unsigned k = 0; k < 3; ++k) {
            uint32_t index = m->version == 1 ? u32(src + 4 * k) : u16(src + 2 * k);
            if (simple) {
                if (index >= m->vert_count) goto cleanup;
                index = remap[index];
            }
            if (index > UINT16_MAX) goto cleanup;
            put16(record + 2 * k, (uint16_t)index);
        }
        record[6] = src[m->version == 1 ? 12 : 6];
        record[7] = src[m->version == 1 ? 15 : 7];
        if (!append(b, record, 8)) goto cleanup;
    }
    if (b->length - start - 4 > UINT32_MAX) goto cleanup;
    put32(b->bytes + start, (uint32_t)(b->length - start - 4));
    *written_faces = faces;
    ok = 1;
cleanup:
    free(vertices);
    free(faces_in);
    free(ordered);
    free(remap);
    free(used);
    cluster_free(&reduced);
    return ok;
}

int col_convert(const uint8_t *source, size_t size, int mode,
                double face_ratio, uint32_t min_faces, uint8_t **output,
                size_t *length, uint32_t *faces_in, uint32_t *faces_out) {
    if (!source || !output || !length || !faces_in || !faces_out ||
        mode < 0 || mode > 2 || face_ratio < 0 || face_ratio > 1)
        return 0;
    buffer result = {0};
    uint8_t header[8] = {'D', 'C', 'O', 'L', 1, 0, 0, 0};
    int ok = append(&result, header, 8);
    size_t position = 0;
    uint32_t count = 0, faces = 0, written = 0;
    while (ok && position + 32 <= size) {
        if (memcmp(source + position, "COL", 3)) {
            size_t next = position < 8 ? 0 : position - 8;
            size_t end = position + 16;
            if (end > size - 3) end = size - 3;
            for (; next < end && memcmp(source + next, "COL", 3); ++next) {}
            if (next >= end || next == position) break;
            position = next;
            continue;
        }
        uint32_t entry_size = u32(source + position + 4);
        if (entry_size > size - position - 8) {
            ok = 0;
            break;
        }
        model m;
        ok = parse_model(source, size, position, position + 8 + entry_size, &m);
        uint32_t model_faces = 0;
        if (ok) ok = emit_model(&result, source, position, &m, mode, face_ratio,
                                min_faces, &model_faces);
        if (ok) {
            if (count == UINT16_MAX || m.face_count > UINT32_MAX - faces ||
                model_faces > UINT32_MAX - written) ok = 0;
            else {
                ++count;
                faces += m.face_count;
                written += model_faces;
            }
        }
        position += 8 + entry_size;
    }
    if (ok) {
        put16(result.bytes + 6, (uint16_t)count);
        *output = result.bytes;
        *length = result.length;
        *faces_in = faces;
        *faces_out = written;
    } else free(result.bytes);
    return ok;
}
