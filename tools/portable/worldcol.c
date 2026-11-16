#include "worldcol.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int16_t xyz[3];
} point;

typedef struct {
    uint32_t vertices[3];
    uint8_t material, light;
    int16_t normal[3];
    int32_t min[3], max[3];
} face;

typedef struct {
    int32_t min[3], max[3];
    uint16_t a, b;
} node;

typedef struct {
    const point *vertices;
    const face *faces;
    uint32_t *order, *scratch;
    node *nodes;
    uint32_t order_count, node_count, capacity;
} tree;

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static double round_even(double value) {
    double whole;
    double fraction = modf(value, &whole);
    if (fraction > 0.5 || (fraction == 0.5 && fmod(whole, 2))) return whole + 1;
    if (fraction < -0.5 || (fraction == -0.5 && fmod(whole, 2))) return whole - 1;
    return whole;
}

static int16_t quantize(double value) {
    value *= 128.0;
    if (value <= INT16_MIN) return INT16_MIN;
    if (value >= INT16_MAX) return INT16_MAX;
    return (int16_t)round_even(value);
}

static int16_t normal_quantize(double value) {
    value = round_even(value * 4096);
    if (value < -4096) return -4096;
    if (value > 4096) return 4096;
    return (int16_t)value;
}

static int cmp_face(const tree *t, uint32_t a, uint32_t b, unsigned axis) {
    int32_t left = t->faces[a].min[axis] + t->faces[a].max[axis];
    int32_t right = t->faces[b].min[axis] + t->faces[b].max[axis];
    return left < right ? -1 : left > right ? 1 : 0;
}

static void sort_faces(tree *t, uint32_t *ids, uint32_t n, unsigned axis) {
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t base = 0; base < n; base += width * 2) {
            uint32_t left = base, middle = base + width, right = middle + width;
            if (middle > n) middle = n;
            if (right > n) right = n;
            uint32_t a = left, b = middle;
            for (uint32_t i = left; i < right; ++i)
                t->scratch[i] = b == right ||
                                (a < middle && cmp_face(t, ids[a], ids[b], axis) <= 0) ?
                                ids[a++] : ids[b++];
        }
        memcpy(ids, t->scratch, n * sizeof *ids);
        if (width > n / 2) break;
    }
}

static int build_node(tree *t, uint32_t *ids, uint32_t count, uint16_t *index) {
    if (t->node_count == t->capacity || t->node_count > UINT16_MAX) return 0;
    uint32_t at = t->node_count++;
    node *current = &t->nodes[at];
    for (unsigned j = 0; j < 3; ++j) {
        current->min[j] = INT32_MAX;
        current->max[j] = INT32_MIN;
    }
    for (uint32_t i = 0; i < count; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            if (t->faces[ids[i]].min[j] < current->min[j])
                current->min[j] = t->faces[ids[i]].min[j];
            if (t->faces[ids[i]].max[j] > current->max[j])
                current->max[j] = t->faces[ids[i]].max[j];
        }
    if (count <= 6) {
        current->a = (uint16_t)t->order_count;
        current->b = (uint16_t)(0x8000 | count);
        memcpy(t->order + t->order_count, ids, count * sizeof *ids);
        t->order_count += count;
    } else {
        int32_t low[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
        int32_t high[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
        for (uint32_t i = 0; i < count; ++i)
            for (unsigned j = 0; j < 3; ++j) {
                int32_t centroid = t->faces[ids[i]].min[j] + t->faces[ids[i]].max[j];
                if (centroid < low[j]) low[j] = centroid;
                if (centroid > high[j]) high[j] = centroid;
            }
        unsigned axis = 0;
        for (unsigned j = 1; j < 3; ++j)
            if (high[j] - low[j] > high[axis] - low[axis])
                axis = j;
        sort_faces(t, ids, count, axis);
        uint32_t half = count / 2;
        if (!build_node(t, ids, half, &current->a) ||
            !build_node(t, ids + half, count - half, &current->b)) return 0;
    }
    *index = (uint16_t)at;
    return 1;
}

static void extend(int32_t min[3], int32_t max[3], const int32_t value[3]) {
    for (unsigned j = 0; j < 3; ++j) {
        if (value[j] < min[j]) min[j] = value[j];
        if (value[j] > max[j]) max[j] = value[j];
    }
}

int worldcol_pack(const worldcol_model *m, uint8_t **output, size_t *length,
                  uint16_t *radius, uint32_t *face_count) {
    if (!m || !output || !length || !radius || !face_count ||
        (m->vertex_count && !m->vertices) || (m->face_count && !m->faces) ||
        (m->sphere_count && !m->spheres) || (m->box_count && !m->boxes) ||
        m->vertex_count > UINT16_MAX || m->face_count > UINT16_MAX ||
        m->sphere_count > UINT16_MAX || m->box_count > UINT16_MAX ||
        m->face_count > (SIZE_MAX - 1) / 2) return 0;
    point *vertices = m->vertex_count ? malloc(m->vertex_count * sizeof *vertices) : NULL;
    face *faces = m->face_count ? malloc(m->face_count * sizeof *faces) : NULL;
    uint32_t *order = m->face_count ? malloc(m->face_count * sizeof *order) : NULL;
    uint32_t *scratch = m->face_count ? malloc(m->face_count * sizeof *scratch) : NULL;
    node *nodes = m->face_count ? malloc((m->face_count * 2 + 1) * sizeof *nodes) : NULL;
    uint8_t *used = m->vertex_count ? calloc(m->vertex_count, 1) : NULL;
    uint16_t *remap = m->vertex_count ? malloc(m->vertex_count * sizeof *remap) : NULL;
    int ok = (!m->vertex_count || (vertices && used && remap)) &&
             (!m->face_count || (faces && order && scratch && nodes));
    for (size_t i = 0; i < m->vertex_count && ok; ++i) {
        vertices[i].xyz[0] = quantize(m->vertices[i].x);
        vertices[i].xyz[1] = quantize(m->vertices[i].y);
        vertices[i].xyz[2] = quantize(m->vertices[i].z);
    }
    uint32_t nf = 0;
    int32_t bounds_min[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
    int32_t bounds_max[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    for (size_t i = 0; i < m->face_count && ok; ++i) {
        worldcol_face input = m->faces[i];
        if (input.a >= m->vertex_count || input.b >= m->vertex_count ||
            input.c >= m->vertex_count) continue;
        const int16_t *a = vertices[input.a].xyz, *b = vertices[input.c].xyz;
        const int16_t *c = vertices[input.b].xyz;
        int32_t u[3], v[3], normal[3];
        for (unsigned j = 0; j < 3; ++j) {
            u[j] = (int32_t)b[j] - a[j];
            v[j] = (int32_t)c[j] - a[j];
        }
        normal[0] = u[1] * v[2] - u[2] * v[1];
        normal[1] = u[2] * v[0] - u[0] * v[2];
        normal[2] = u[0] * v[1] - u[1] * v[0];
        double ln = sqrt((double)normal[0] * normal[0] +
                         (double)normal[1] * normal[1] +
                         (double)normal[2] * normal[2]);
        if (ln < 1e-6) continue;
        face *f = &faces[nf++];
        f->vertices[0] = input.a;
        f->vertices[1] = input.c;
        f->vertices[2] = input.b;
        f->material = input.material;
        f->light = input.light;
        for (unsigned j = 0; j < 3; ++j) {
            f->normal[j] = normal_quantize(normal[j] / ln);
            f->min[j] = a[j] < b[j] ? (a[j] < c[j] ? a[j] : c[j]) :
                        (b[j] < c[j] ? b[j] : c[j]);
            f->max[j] = a[j] > b[j] ? (a[j] > c[j] ? a[j] : c[j]) :
                        (b[j] > c[j] ? b[j] : c[j]);
        }
        extend(bounds_min, bounds_max, f->min);
        extend(bounds_min, bounds_max, f->max);
    }
    for (size_t i = 0; i < m->box_count && ok; ++i) {
        int32_t min[3], max[3];
        const double lo[3] = {m->boxes[i].min.x, m->boxes[i].min.y, m->boxes[i].min.z};
        const double hi[3] = {m->boxes[i].max.x, m->boxes[i].max.y, m->boxes[i].max.z};
        for (unsigned j = 0; j < 3; ++j) {
            min[j] = quantize(lo[j] < hi[j] ? lo[j] : hi[j]);
            max[j] = quantize(lo[j] > hi[j] ? lo[j] : hi[j]);
        }
        extend(bounds_min, bounds_max, min);
        extend(bounds_min, bounds_max, max);
    }
    for (size_t i = 0; i < m->sphere_count && ok; ++i) {
        const worldcol_sphere *s = &m->spheres[i];
        const double center[3] = {s->center.x, s->center.y, s->center.z};
        double scaled = round_even(s->radius * 128);
        int32_t r = scaled < 1 ? 1 : scaled > UINT16_MAX ? UINT16_MAX : (int32_t)scaled;
        int32_t lower[3], upper[3];
        for (unsigned j = 0; j < 3; ++j) {
            lower[j] = quantize(center[j]) - r;
            upper[j] = quantize(center[j]) + r;
        }
        extend(bounds_min, bounds_max, lower);
        extend(bounds_min, bounds_max, upper);
    }
    uint8_t *packed = NULL;
    size_t packed_size = 0;
    uint32_t used_count = 0;
    tree tree = {vertices, faces, order, scratch, nodes, 0, 0, (uint32_t)(m->face_count * 2 + 1)};
    if (ok && bounds_min[0] != INT32_MAX) {
        for (unsigned j = 0; j < 3; ++j)
            if (bounds_min[j] < INT16_MIN || bounds_max[j] > INT16_MAX)
                ok = 0;
        if (ok && nf) {
            for (uint32_t i = 0; i < nf; ++i) order[i] = i;
            uint16_t root;
            ok = build_node(&tree, order, nf, &root);
        }
        if (ok)
            for (uint32_t i = 0; i < nf; ++i)
                for (unsigned j = 0; j < 3; ++j)
                    used[faces[order[i]].vertices[j]] = 1;
        if (ok)
            for (uint32_t i = 0; i < m->vertex_count; ++i)
                if (used[i]) remap[i] = (uint16_t)used_count++;
        if (nf > UINT16_MAX || tree.node_count > UINT16_MAX ||
            used_count > UINT16_MAX) ok = 0;
        size_t raw_size = 24 + m->sphere_count * 12 + m->box_count * 16 +
                          tree.node_count * 16 + (size_t)nf * 16 + (size_t)used_count * 6;
        packed_size = (raw_size + 3) & ~(size_t)3;
        if (ok) packed = calloc(packed_size, 1);
        if (ok && !packed) ok = 0;
        if (ok) {
            for (unsigned j = 0; j < 3; ++j) {
                put16(packed + j * 2, (uint16_t)bounds_min[j]);
                put16(packed + 6 + j * 2, (uint16_t)bounds_max[j]);
            }
            put16(packed + 12, (uint16_t)m->sphere_count);
            put16(packed + 14, (uint16_t)m->box_count);
            put16(packed + 16, (uint16_t)tree.node_count);
            put16(packed + 18, (uint16_t)used_count);
            put16(packed + 20, (uint16_t)nf);
            size_t cursor = 24;
            for (size_t i = 0; i < m->sphere_count; ++i) {
                const worldcol_sphere *s = &m->spheres[i];
                const double center[3] = {s->center.x, s->center.y, s->center.z};
                for (unsigned j = 0; j < 3; ++j)
                    put16(packed + cursor + j * 2, (uint16_t)quantize(center[j]));
                double scaled = round_even(s->radius * 128);
                put16(packed + cursor + 6, (uint16_t)(scaled < 1 ? 1 :
                      scaled > UINT16_MAX ? UINT16_MAX : (uint16_t)scaled));
                packed[cursor + 8] = s->surface[0];
                packed[cursor + 9] = s->surface[3];
                cursor += 12;
            }
            for (size_t i = 0; i < m->box_count; ++i) {
                const worldcol_box *b = &m->boxes[i];
                const double lo[3] = {b->min.x, b->min.y, b->min.z};
                const double hi[3] = {b->max.x, b->max.y, b->max.z};
                for (unsigned j = 0; j < 3; ++j) {
                    put16(packed + cursor + j * 2, (uint16_t)quantize(lo[j] < hi[j] ? lo[j] : hi[j]));
                    put16(packed + cursor + 6 + j * 2, (uint16_t)quantize(lo[j] > hi[j] ? lo[j] : hi[j]));
                }
                packed[cursor + 12] = b->surface[0];
                packed[cursor + 13] = b->surface[3];
                cursor += 16;
            }
            for (uint32_t i = 0; i < tree.node_count; ++i) {
                for (unsigned j = 0; j < 3; ++j) {
                    put16(packed + cursor + j * 2, (uint16_t)tree.nodes[i].min[j]);
                    put16(packed + cursor + 6 + j * 2, (uint16_t)tree.nodes[i].max[j]);
                }
                put16(packed + cursor + 12, tree.nodes[i].a);
                put16(packed + cursor + 14, tree.nodes[i].b);
                cursor += 16;
            }
            for (uint32_t i = 0; i < nf; ++i) {
                const face *f = &faces[order[i]];
                for (unsigned j = 0; j < 3; ++j) {
                    put16(packed + cursor + j * 2, remap[f->vertices[j]]);
                    put16(packed + cursor + 8 + j * 2, (uint16_t)f->normal[j]);
                }
                packed[cursor + 6] = f->material;
                packed[cursor + 7] = f->light;
                cursor += 16;
            }
            for (uint32_t i = 0; i < m->vertex_count; ++i)
                if (used[i]) {
                    for (unsigned j = 0; j < 3; ++j)
                        put16(packed + cursor + j * 2, (uint16_t)vertices[i].xyz[j]);
                    cursor += 6;
                }
            double r2 = 0;
            for (unsigned j = 0; j < 3; ++j) {
                double dist = bounds_min[j] < -bounds_max[j] ?
                              -(double)bounds_min[j] : bounds_max[j];
                r2 += dist * dist;
            }
            double r = sqrt(r2) / 128;
            if (r < 0.5) r = 0.5;
            r = ceil(r * 16);
            *radius = (uint16_t)(r > UINT16_MAX ? UINT16_MAX : r);
        }
    }
    if (ok) {
        *output = packed;
        *length = packed_size;
        *face_count = nf;
    } else free(packed);
    free(vertices);
    free(faces);
    free(order);
    free(scratch);
    free(nodes);
    free(used);
    free(remap);
    return ok;
}
