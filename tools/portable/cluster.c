#include "cluster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t c[3];
} cell;

typedef struct {
    cell min, max, a, b, c;
    uint16_t material;
} key;

typedef struct {
    cell c;
    cluster_vertex sum;
    uint32_t count;
    uint8_t occupied;
} group;

static uint32_t hash_bytes(const void *data, size_t size) {
    const uint8_t *p = data;
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static int cmp_cell(const cell *a, const cell *b) {
    for (unsigned i = 0; i < 3; ++i)
        if (a->c[i] != b->c[i])
            return a->c[i] < b->c[i] ? -1 : 1;
    return 0;
}

static int cmp_key(const key *a, const key *b) {
    return !cmp_cell(&a->min, &b->min) && !cmp_cell(&a->max, &b->max) &&
           !cmp_cell(&a->a, &b->a) && !cmp_cell(&a->b, &b->b) &&
           !cmp_cell(&a->c, &b->c) && a->material == b->material;
}

static size_t table_size(size_t n) {
    size_t size = 8;
    while (size < n * 2 + 1 && size <= SIZE_MAX / 2) size *= 2;
    return size;
}

static void make_cells(const cluster_vertex *vertices, size_t n,
                       const double origin[3], double step, cell *out) {
    for (size_t i = 0; i < n; ++i) {
        out[i].c[0] = (int32_t)((vertices[i].x - origin[0]) / step);
        out[i].c[1] = (int32_t)((vertices[i].y - origin[1]) / step);
        out[i].c[2] = (int32_t)((vertices[i].z - origin[2]) / step);
    }
}

static int run(const cluster_face *faces, size_t face_count,
               const cell *cells, uint32_t *kept, size_t *used) {
    size_t capacity = table_size(face_count);
    key *keys = calloc(capacity, sizeof *keys);
    uint8_t *occupied = calloc(capacity, 1);
    if (!keys || !occupied) {
        free(keys);
        free(occupied);
        return 0;
    }
    *used = 0;
    for (size_t i = 0; i < face_count; ++i) {
        const cluster_face *f = &faces[i];
        cell a = cells[f->a], b = cells[f->b], c = cells[f->c];
        if (!cmp_cell(&a, &b) || !cmp_cell(&a, &c) || !cmp_cell(&b, &c))
            continue;
        cell min = a, max = a;
        if (cmp_cell(&b, &min) < 0) min = b;
        if (cmp_cell(&c, &min) < 0) min = c;
        if (cmp_cell(&b, &max) > 0) max = b;
        if (cmp_cell(&c, &max) > 0) max = c;
        key k;
        memset(&k, 0, sizeof k);
        k.min = min;
        k.max = max;
        k.a = a;
        k.b = b;
        k.c = c;
        k.material = f->material;
        size_t slot = hash_bytes(&k, sizeof k) & (capacity - 1);
        while (occupied[slot] && !cmp_key(&keys[slot], &k))
            slot = (slot + 1) & (capacity - 1);
        if (occupied[slot]) continue;
        keys[slot] = k;
        occupied[slot] = 1;
        kept[(*used)++] = (uint32_t)i;
    }
    free(keys);
    free(occupied);
    return 1;
}

int cluster_decimate(const cluster_vertex *vertices, size_t vertex_count,
                     const cluster_face *faces, size_t face_count,
                     size_t target, cluster_result *result) {
    if (!result || (!vertices && vertex_count) || (!faces && face_count) ||
        vertex_count > UINT32_MAX || face_count > UINT32_MAX ||
        vertex_count > (SIZE_MAX - 1) / 2 || face_count > (SIZE_MAX - 1) / 2)
        return 0;
    *result = (cluster_result){0};
    for (size_t i = 0; i < face_count; ++i)
        if (faces[i].a >= vertex_count || faces[i].b >= vertex_count ||
            faces[i].c >= vertex_count)
            return 0;
    uint32_t *kept = face_count ? malloc(face_count * sizeof *kept) : NULL;
    if (face_count && !kept) return 0;
    if (target >= face_count || !face_count) {
        for (size_t i = 0; i < face_count; ++i) kept[i] = (uint32_t)i;
        result->kept = kept;
        result->count = face_count;
        return 1;
    }
    double origin[3] = {vertices[0].x, vertices[0].y, vertices[0].z};
    double high[3] = {origin[0], origin[1], origin[2]};
    for (size_t i = 0; i < vertex_count; ++i) {
        const double v[3] = {vertices[i].x, vertices[i].y, vertices[i].z};
        for (unsigned j = 0; j < 3; ++j) {
            if (v[j] < origin[j]) origin[j] = v[j];
            if (v[j] > high[j]) high[j] = v[j];
        }
    }
    double extent = high[0] - origin[0];
    for (unsigned j = 1; j < 3; ++j)
        if (high[j] - origin[j] > extent) extent = high[j] - origin[j];
    if (!extent) extent = 1.0;
    cell *cells = malloc(vertex_count * sizeof *cells);
    cell *best = malloc(vertex_count * sizeof *best);
    uint32_t *candidate = malloc(face_count * sizeof *candidate);
    if (!cells || !best || !candidate) {
        free(cells);
        free(best);
        free(candidate);
        free(kept);
        return 0;
    }
    int low = 1, high_resolution = 2048, found = 0;
    size_t best_count = 0;
    while (low <= high_resolution) {
        int mid = (low + high_resolution) / 2;
        make_cells(vertices, vertex_count, origin, extent / mid, cells);
        size_t n = 0;
        if (!run(faces, face_count, cells, candidate, &n)) goto fail;
        if (n <= target) {
            found = 1;
            best_count = n;
            memcpy(best, cells, vertex_count * sizeof *best);
            memcpy(kept, candidate, n * sizeof *kept);
            low = mid + 1;
        } else high_resolution = mid - 1;
    }
    if (!found) {
        make_cells(vertices, vertex_count, origin, extent, best);
        if (!run(faces, face_count, best, kept, &best_count)) goto fail;
    }
    size_t capacity = table_size(vertex_count);
    group *groups = calloc(capacity, sizeof *groups);
    cluster_vertex *reps = malloc(vertex_count * sizeof *reps);
    if (!groups || !reps) {
        free(groups);
        free(reps);
        goto fail;
    }
    for (size_t i = 0; i < vertex_count; ++i) {
        size_t slot = hash_bytes(&best[i], sizeof best[i]) & (capacity - 1);
        while (groups[slot].occupied && cmp_cell(&groups[slot].c, &best[i]))
            slot = (slot + 1) & (capacity - 1);
        if (!groups[slot].occupied) {
            groups[slot].occupied = 1;
            groups[slot].c = best[i];
        }
        groups[slot].sum.x += vertices[i].x;
        groups[slot].sum.y += vertices[i].y;
        groups[slot].sum.z += vertices[i].z;
        ++groups[slot].count;
    }
    for (size_t i = 0; i < vertex_count; ++i) {
        size_t slot = hash_bytes(&best[i], sizeof best[i]) & (capacity - 1);
        while (cmp_cell(&groups[slot].c, &best[i]))
            slot = (slot + 1) & (capacity - 1);
        reps[i] = (cluster_vertex){
            groups[slot].sum.x / groups[slot].count,
            groups[slot].sum.y / groups[slot].count,
            groups[slot].sum.z / groups[slot].count
        };
    }
    result->representatives = reps;
    result->kept = kept;
    result->count = best_count;
    free(groups);
    free(cells);
    free(best);
    free(candidate);
    return 1;
fail:
    free(cells);
    free(best);
    free(candidate);
    free(kept);
    return 0;
}

void cluster_free(cluster_result *result) {
    if (!result) return;
    free(result->representatives);
    free(result->kept);
    *result = (cluster_result){0};
}

static double area(const cluster_vertex *vertices, const cluster_face *faces,
                   const cluster_result *result) {
    double sum = 0;
    for (size_t i = 0; i < result->count; ++i) {
        const cluster_face *f = &faces[result->kept[i]];
        const cluster_vertex *points = result->representatives ?
                                        result->representatives : vertices;
        cluster_vertex a = points[f->a], b = points[f->b], c = points[f->c];
        double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        double wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
        double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz;
        double nz = ux * wy - uy * wx;
        sum += sqrt(nx * nx + ny * ny + nz * nz);
    }
    return sum * 0.5;
}

int cluster_decimate_limited(const cluster_vertex *vertices, size_t vertex_count,
                             const cluster_face *faces, size_t face_count, size_t target,
                             double min_area, double max_shift, size_t max_output,
                             cluster_result *result) {
    if (!result || min_area < 0 || max_shift < 0) return 0;
    if ((!min_area && !max_shift) || target >= face_count || !face_count)
        return cluster_decimate(vertices, vertex_count, faces, face_count, target, result);
    cluster_result full = {0}, fallback = {0};
    if (!cluster_decimate(vertices, vertex_count, faces, face_count, face_count, &full))
        return 0;
    double full_area = min_area ? area(vertices, faces, &full) : 0;
    cluster_free(&full);
    for (;;) {
        cluster_result next = {0};
        if (!cluster_decimate(vertices, vertex_count, faces, face_count, target, &next)) {
            cluster_free(&fallback);
            return 0;
        }
        int area_ok = !min_area || !next.representatives ||
                      area(vertices, faces, &next) >= min_area * full_area;
        if (area_ok && max_output && next.count > max_output && fallback.representatives) {
            cluster_free(&next);
            *result = fallback;
            return 1;
        }
        int shift_ok = !max_shift || !next.representatives;
        if (!shift_ok) {
            shift_ok = 1;
            double max2 = max_shift * max_shift;
            for (size_t i = 0; i < next.count && shift_ok; ++i) {
                cluster_face f = faces[next.kept[i]];
                uint32_t ids[3] = {f.a, f.b, f.c};
                for (unsigned j = 0; j < 3; ++j) {
                    uint32_t index = ids[j];
                    cluster_vertex a = vertices[index], b = next.representatives[index];
                    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                    if (dx * dx + dy * dy + dz * dz > max2) {
                        shift_ok = 0;
                        break;
                    }
                }
            }
        }
        if (area_ok && (shift_ok || (max_output && next.count > max_output))) {
            cluster_free(&fallback);
            *result = next;
            return 1;
        }
        if (area_ok) {
            cluster_free(&fallback);
            fallback = next;
        } else cluster_free(&next);
        if (target >= face_count) target = face_count;
        else {
            size_t next_target = (size_t)(target * 1.25) + 1;
            target = next_target > target ? next_target : face_count;
        }
    }
}
