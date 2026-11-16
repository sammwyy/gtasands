#include "qem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *items;
    size_t count, capacity;
} list;

typedef struct {
    uint64_t key;
    uint32_t count;
    uint8_t used;
} edge;

typedef struct {
    double cost;
    uint32_t u, v, u_stamp, v_stamp;
} candidate;

typedef struct {
    candidate *data;
    size_t count, capacity;
} heap;

typedef struct {
    const cluster_vertex *verts;
    cluster_face *faces;
    list *incident;
    size_t nverts, nfaces;
    uint8_t *alive, *locked;
    uint32_t *stamps, *mark_vertex, *mark_triangle, *neighbors;
    const uint32_t *wpos;
    size_t wedge_count;
    uint32_t *wedge_mark, *wedge_remap;
    uint32_t generation;
    double *quad;
    heap queue;
} context;

static uint32_t position(const context *c, uint32_t wedge) {
    return c->wpos ? c->wpos[wedge] : wedge;
}

static int append(list *l, uint32_t index) {
    if (l->count == l->capacity) {
        size_t size = l->capacity ? l->capacity * 2 : 8;
        if (size < l->capacity || size > SIZE_MAX / sizeof *l->items) return 0;
        uint32_t *next = realloc(l->items, size * sizeof *next);
        if (!next) return 0;
        l->items = next;
        l->capacity = size;
    }
    l->items[l->count++] = index;
    return 1;
}

static int before(candidate a, candidate b) {
    if (a.cost != b.cost) return a.cost < b.cost;
    if (a.u != b.u) return a.u < b.u;
    if (a.v != b.v) return a.v < b.v;
    if (a.u_stamp != b.u_stamp) return a.u_stamp < b.u_stamp;
    return a.v_stamp < b.v_stamp;
}

static int push(heap *h, candidate entry) {
    if (h->count == h->capacity) {
        size_t size = h->capacity ? h->capacity * 2 : 128;
        if (size < h->capacity || size > SIZE_MAX / sizeof *h->data) return 0;
        candidate *next = realloc(h->data, size * sizeof *next);
        if (!next) return 0;
        h->data = next;
        h->capacity = size;
    }
    size_t i = h->count++;
    while (i) {
        size_t parent = (i - 1) / 2;
        if (!before(entry, h->data[parent])) break;
        h->data[i] = h->data[parent];
        i = parent;
    }
    h->data[i] = entry;
    return 1;
}

static candidate pop(heap *h) {
    candidate top = h->data[0], last = h->data[--h->count];
    if (h->count) {
        size_t i = 0;
        while (i * 2 + 1 < h->count) {
            size_t child = i * 2 + 1;
            if (child + 1 < h->count && before(h->data[child + 1], h->data[child]))
                ++child;
            if (!before(h->data[child], last)) break;
            h->data[i] = h->data[child];
            i = child;
        }
        h->data[i] = last;
    }
    return top;
}

static uint64_t edge_key(uint32_t a, uint32_t b) {
    if (a > b) {
        uint32_t temp = a;
        a = b;
        b = temp;
    }
    return ((uint64_t)a << 32) | b;
}

static uint64_t mix(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ull;
    return key ^ key >> 33;
}

static int initialize_edges(context *c) {
    size_t cap = 16;
    if (c->nfaces > (SIZE_MAX / 6 - 1)) return 0;
    while (cap < c->nfaces * 6 + 1 && cap <= SIZE_MAX / 2) cap *= 2;
    edge *entries = calloc(cap, sizeof *entries);
    if (!entries) return 0;
    for (size_t i = 0; i < c->nfaces; ++i) {
        const cluster_face *f = &c->faces[i];
        uint32_t corners[3] = {position(c, f->a), position(c, f->b),
                               position(c, f->c)};
        for (size_t j = 0; j < 3; ++j) {
            uint64_t key = edge_key(corners[j], corners[(j + 1) % 3]);
            size_t slot = mix(key) & (cap - 1);
            while (entries[slot].used && entries[slot].key != key)
                slot = (slot + 1) & (cap - 1);
            entries[slot].key = key;
            entries[slot].used = 1;
            ++entries[slot].count;
        }
    }
    for (size_t i = 0; i < cap; ++i)
        if (entries[i].used && entries[i].count != 2) {
            c->locked[entries[i].key >> 32] = 1;
            c->locked[(uint32_t)entries[i].key] = 1;
        }
    free(entries);
    return 1;
}

static cluster_vertex sub(cluster_vertex a, cluster_vertex b) {
    return (cluster_vertex){a.x - b.x, a.y - b.y, a.z - b.z};
}

static cluster_vertex cross(cluster_vertex a, cluster_vertex b) {
    return (cluster_vertex){a.y * b.z - a.z * b.y,
                            a.z * b.x - a.x * b.z,
                            a.x * b.y - a.y * b.x};
}

static double dot(cluster_vertex a, cluster_vertex b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static cluster_vertex normal(const context *c, cluster_face f, uint32_t old, uint32_t replacement) {
    uint32_t a = position(c, f.a), b = position(c, f.b), cc = position(c, f.c);
    if (a == old) a = replacement;
    if (b == old) b = replacement;
    if (cc == old) cc = replacement;
    return cross(sub(c->verts[b], c->verts[a]), sub(c->verts[cc], c->verts[a]));
}

static void initialize_quads(context *c) {
    for (size_t i = 0; i < c->nfaces; ++i) {
        cluster_face f = c->faces[i];
        cluster_vertex v = normal(c, f, UINT32_MAX, 0);
        double length = sqrt(dot(v, v));
        if (length < 1e-12) continue;
        double a = v.x / length, b = v.y / length, cc = v.z / length;
        cluster_vertex face_normal = {a, b, cc};
        double d = -dot(face_normal, c->verts[position(c, f.a)]);
        double weight = length * 0.5;
        double q[10] = {a * a, a * b, a * cc, a * d, b * b,
                        b * cc, b * d, cc * cc, cc * d, d * d};
        uint32_t ids[3] = {f.a, f.b, f.c};
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned k = 0; k < 10; ++k)
                c->quad[position(c, ids[j]) * 10 + k] += q[k] * weight;
    }
}

static size_t neighbors(context *c, uint32_t u, uint32_t *out) {
    uint32_t mark = ++c->generation;
    if (!mark) {
        memset(c->mark_vertex, 0, c->nverts * sizeof *c->mark_vertex);
        mark = ++c->generation;
    }
    size_t count = 0;
    for (size_t i = 0; i < c->incident[u].count; ++i) {
        uint32_t ti = c->incident[u].items[i];
        if (!c->alive[ti]) continue;
        const cluster_face *f = &c->faces[ti];
        uint32_t ids[3] = {f->a, f->b, f->c};
        for (unsigned j = 0; j < 3; ++j)
            if (position(c, ids[j]) != u &&
                c->mark_vertex[position(c, ids[j])] != mark) {
                uint32_t id = position(c, ids[j]);
                c->mark_vertex[id] = mark;
                out[count++] = id;
            }
    }
    return count;
}

static double cost(const context *c, uint32_t u, uint32_t v) {
    double q[10];
    for (unsigned k = 0; k < 10; ++k)
        q[k] = c->quad[u * 10 + k] + c->quad[v * 10 + k];
    cluster_vertex p = c->verts[v];
    return q[0] * p.x * p.x + 2 * q[1] * p.x * p.y + 2 * q[2] * p.x * p.z +
           2 * q[3] * p.x + q[4] * p.y * p.y + 2 * q[5] * p.y * p.z +
           2 * q[6] * p.y + q[7] * p.z * p.z + 2 * q[8] * p.z + q[9];
}

static int push_neighbors(context *c, uint32_t u, const uint8_t *bones) {
    if (c->locked[u]) return 1;
    size_t n = neighbors(c, u, c->neighbors);
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = c->neighbors[i];
        if (bones && bones[u] != bones[v]) continue;
        if (!push(&c->queue, (candidate){cost(c, u, v), u, v,
                                        c->stamps[u], c->stamps[v]})) return 0;
    }
    return 1;
}

static int linked(context *c, uint32_t u, uint32_t v, uint32_t *shared) {
    size_t nshared = 0, nu = neighbors(c, u, c->neighbors);
    uint32_t *u_neighbors = malloc(nu * sizeof *u_neighbors);
    if (nu && !u_neighbors) return -1;
    if (nu) memcpy(u_neighbors, c->neighbors, nu * sizeof *u_neighbors);
    size_t nv = neighbors(c, v, c->neighbors), common = 0;
    for (size_t i = 0; i < nu; ++i)
        for (size_t j = 0; j < nv; ++j)
            if (u_neighbors[i] == c->neighbors[j]) {
                ++common;
                break;
            }
    free(u_neighbors);
    uint32_t mark = ++c->generation;
    for (size_t i = 0; i < c->incident[v].count; ++i)
        if (c->alive[c->incident[v].items[i]])
            c->mark_triangle[c->incident[v].items[i]] = mark;
    for (size_t i = 0; i < c->incident[u].count; ++i) {
        uint32_t ti = c->incident[u].items[i];
        if (c->alive[ti] && c->mark_triangle[ti] == mark)
            shared[nshared++] = ti;
    }
    return nshared && nshared == common ? (int)nshared : 0;
}

static int collapse(context *c, uint32_t u, uint32_t v, size_t *alive, const uint8_t *bones) {
    uint32_t *shared = malloc(c->incident[u].count * sizeof *shared);
    if (!shared && c->incident[u].count) return -1;
    int nshared = linked(c, u, v, shared);
    if (nshared <= 0) {
        free(shared);
        return nshared;
    }
    uint32_t mark = ++c->generation;
    for (int i = 0; i < nshared; ++i) c->mark_triangle[shared[i]] = mark;
    uint32_t wedge_mark = ++c->generation;
    if (c->wpos) {
        for (int i = 0; i < nshared; ++i) {
            cluster_face f = c->faces[shared[i]];
            uint32_t ids[3] = {f.a, f.b, f.c};
            uint32_t wu = UINT32_MAX, wv = UINT32_MAX;
            for (unsigned j = 0; j < 3; ++j) {
                if (c->wpos[ids[j]] == u) wu = ids[j];
                if (c->wpos[ids[j]] == v) wv = ids[j];
            }
            if (wu == UINT32_MAX || wv == UINT32_MAX ||
                (c->wedge_mark[wu] == wedge_mark && c->wedge_remap[wu] != wv)) {
                free(shared);
                return 0;
            }
            c->wedge_mark[wu] = wedge_mark;
            c->wedge_remap[wu] = wv;
        }
    }
    for (size_t i = 0; i < c->incident[u].count; ++i) {
        uint32_t ti = c->incident[u].items[i];
        if (!c->alive[ti] || c->mark_triangle[ti] == mark) continue;
        if (c->wpos) {
            cluster_face f = c->faces[ti];
            uint32_t ids[3] = {f.a, f.b, f.c};
            for (unsigned j = 0; j < 3; ++j)
                if (c->wpos[ids[j]] == u && c->wedge_mark[ids[j]] != wedge_mark) {
                    free(shared);
                    return 0;
                }
        }
        cluster_vertex a = normal(c, c->faces[ti], UINT32_MAX, 0);
        cluster_vertex b = normal(c, c->faces[ti], u, v);
        double l2 = dot(b, b);
        if (l2 < 1e-14 || dot(a, b) <= 0.3 * sqrt(dot(a, a) * l2)) {
            free(shared);
            return 0;
        }
    }
    for (int i = 0; i < nshared; ++i) {
        c->alive[shared[i]] = 0;
        --*alive;
    }
    free(shared);
    for (size_t i = 0; i < c->incident[u].count; ++i) {
        uint32_t ti = c->incident[u].items[i];
        if (!c->alive[ti]) continue;
        cluster_face *f = &c->faces[ti];
        if (position(c, f->a) == u) f->a = c->wpos ? c->wedge_remap[f->a] : v;
        if (position(c, f->b) == u) f->b = c->wpos ? c->wedge_remap[f->b] : v;
        if (position(c, f->c) == u) f->c = c->wpos ? c->wedge_remap[f->c] : v;
        if (!append(&c->incident[v], ti)) return -1;
    }
    c->incident[u].count = 0;
    for (unsigned k = 0; k < 10; ++k)
        c->quad[v * 10 + k] += c->quad[u * 10 + k];
    size_t touched = neighbors(c, v, c->neighbors);
    uint32_t *ids = malloc((touched + 1) * sizeof *ids);
    if (!ids) return -1;
    if (touched) memcpy(ids, c->neighbors, touched * sizeof *ids);
    ids[touched++] = v;
    for (size_t i = 0; i < touched; ++i) ++c->stamps[ids[i]];
    for (size_t i = 0; i < touched; ++i)
        if (!push_neighbors(c, ids[i], bones)) {
            free(ids);
            return -1;
        }
    free(ids);
    return 1;
}

static int decimate(const cluster_vertex *vertices, size_t vertex_count,
                    const cluster_face *faces, size_t face_count, size_t target,
                    const uint8_t *bones, const uint32_t *wpos, size_t wedge_count,
                    cluster_face **result, size_t *result_count) {
    if (!result || !result_count || (vertex_count && !vertices) ||
        (face_count && !faces) || vertex_count > UINT32_MAX || face_count > UINT32_MAX)
        return 0;
    *result = NULL;
    *result_count = 0;
    if (face_count > SIZE_MAX / sizeof *faces || vertex_count > SIZE_MAX / 10 / sizeof(double))
        return 0;
    context c = {0};
    c.verts = vertices;
    c.nverts = vertex_count;
    c.nfaces = face_count;
    c.wpos = wpos;
    c.wedge_count = wedge_count;
    c.faces = face_count ? malloc(face_count * sizeof *faces) : NULL;
    c.incident = vertex_count ? calloc(vertex_count, sizeof *c.incident) : NULL;
    c.alive = face_count ? malloc(face_count) : NULL;
    c.locked = vertex_count ? calloc(vertex_count, 1) : NULL;
    c.stamps = vertex_count ? calloc(vertex_count, sizeof *c.stamps) : NULL;
    c.mark_vertex = vertex_count ? calloc(vertex_count, sizeof *c.mark_vertex) : NULL;
    c.mark_triangle = face_count ? calloc(face_count, sizeof *c.mark_triangle) : NULL;
    c.neighbors = vertex_count ? malloc(vertex_count * sizeof *c.neighbors) : NULL;
    c.quad = vertex_count ? calloc(vertex_count * 10, sizeof *c.quad) : NULL;
    c.wedge_mark = wpos && wedge_count ? calloc(wedge_count, sizeof *c.wedge_mark) : NULL;
    c.wedge_remap = wpos && wedge_count ? malloc(wedge_count * sizeof *c.wedge_remap) : NULL;
    int ok = (!face_count || (c.faces && c.alive && c.mark_triangle)) &&
             (!vertex_count || (c.incident && c.locked && c.stamps &&
                                c.mark_vertex && c.neighbors && c.quad)) &&
             (!wpos || !wedge_count || (c.wedge_mark && c.wedge_remap));
    if (ok && face_count) {
        memcpy(c.faces, faces, face_count * sizeof *faces);
        memset(c.alive, 1, face_count);
    }
    for (size_t i = 0; i < face_count && ok; ++i) {
        cluster_face f = faces[i];
        if (f.a >= wedge_count || f.b >= wedge_count || f.c >= wedge_count) {
            ok = 0;
            break;
        }
        uint32_t a = position(&c, f.a), b = position(&c, f.b), d = position(&c, f.c);
        if (a >= vertex_count || b >= vertex_count || d >= vertex_count ||
            !append(&c.incident[a], (uint32_t)i) ||
            (b != a && !append(&c.incident[b], (uint32_t)i)) ||
            (d != a && d != b && !append(&c.incident[d], (uint32_t)i)))
            ok = 0;
    }
    if (ok && target < face_count && face_count) {
        ok = initialize_edges(&c);
        if (ok) initialize_quads(&c);
        for (uint32_t i = 0; i < vertex_count && ok; ++i)
            ok = push_neighbors(&c, i, bones);
        size_t alive = face_count;
        while (ok && alive > target && c.queue.count) {
            candidate top = pop(&c.queue);
            if (top.u_stamp != c.stamps[top.u] ||
                top.v_stamp != c.stamps[top.v] || !c.incident[top.u].count)
                continue;
            int done = collapse(&c, top.u, top.v, &alive, bones);
            if (done < 0) ok = 0;
        }
    }
    if (ok) {
        size_t count = 0;
        for (size_t i = 0; i < face_count; ++i)
            if (c.alive[i]) ++count;
        cluster_face *out = count ? malloc(count * sizeof *out) : NULL;
        if (count && !out) ok = 0;
        else {
            size_t j = 0;
            for (size_t i = 0; i < face_count; ++i)
                if (c.alive[i]) out[j++] = c.faces[i];
            *result = out;
            *result_count = count;
        }
    }
    for (size_t i = 0; i < vertex_count; ++i) free(c.incident ? c.incident[i].items : NULL);
    free(c.faces);
    free(c.incident);
    free(c.alive);
    free(c.locked);
    free(c.stamps);
    free(c.mark_vertex);
    free(c.mark_triangle);
    free(c.neighbors);
    free(c.quad);
    free(c.queue.data);
    free(c.wedge_mark);
    free(c.wedge_remap);
    return ok;
}

int qem_decimate(const cluster_vertex *vertices, size_t vertex_count,
                 const cluster_face *faces, size_t face_count, size_t target,
                 const uint8_t *bones, cluster_face **result, size_t *result_count) {
    return decimate(vertices, vertex_count, faces, face_count, target, bones,
                    NULL, vertex_count, result, result_count);
}

typedef struct {
    int64_t key[3];
    uint32_t index;
    uint8_t used;
} welded_vertex;

static int64_t rounded_thousandth(double value) {
    char text[128];
    snprintf(text, sizeof text, "%.3f", value);
    return (int64_t)nearbyint(strtod(text, NULL) * 1000);
}

int wedge_decimate(const cluster_vertex *vertices, size_t vertex_count,
                   const cluster_face *faces, size_t face_count, size_t target,
                   cluster_face **result, size_t *result_count) {
    if (!result || !result_count || (vertex_count && !vertices) ||
        (face_count && !faces) || vertex_count > UINT32_MAX || face_count > UINT32_MAX ||
        vertex_count > (SIZE_MAX - 1) / 2) return 0;
    if (target >= face_count || !face_count)
        return qem_decimate(vertices, vertex_count, faces, face_count, target,
                            NULL, result, result_count);
    size_t cap = 8;
    while (cap < vertex_count * 2 + 1 && cap <= SIZE_MAX / 2) cap *= 2;
    welded_vertex *table = calloc(cap, sizeof *table);
    uint32_t *wpos = vertex_count ? malloc(vertex_count * sizeof *wpos) : NULL;
    cluster_vertex *points = vertex_count ? malloc(vertex_count * sizeof *points) : NULL;
    cluster_face *filtered = face_count ? malloc(face_count * sizeof *filtered) : NULL;
    if (!table || (vertex_count && (!wpos || !points)) || (face_count && !filtered)) {
        free(table); free(wpos); free(points); free(filtered);
        return 0;
    }
    size_t positions = 0, count = 0;
    int ok = 1;
    for (size_t i = 0; i < vertex_count; ++i) {
        const double xyz[3] = {vertices[i].x, vertices[i].y, vertices[i].z};
        int64_t key[3];
        for (unsigned j = 0; j < 3; ++j) {
            if (!isfinite(xyz[j]) || fabs(xyz[j]) > (double)INT32_MAX / 1000)
                ok = 0;
            else key[j] = rounded_thousandth(xyz[j]);
        }
        if (!ok) break;
        uint64_t hash = mix((uint64_t)key[0] ^ mix((uint64_t)key[1]) ^
                            mix((uint64_t)key[2]));
        size_t slot = hash & (cap - 1);
        while (table[slot].used &&
               (key[0] != table[slot].key[0] ||
                key[1] != table[slot].key[1] ||
                key[2] != table[slot].key[2]))
            slot = (slot + 1) & (cap - 1);
        if (!table[slot].used) {
            table[slot].used = 1;
            memcpy(table[slot].key, key, sizeof key);
            table[slot].index = (uint32_t)positions;
            points[positions++] = vertices[i];
        }
        wpos[i] = table[slot].index;
    }
    for (size_t i = 0; i < face_count && ok; ++i) {
        cluster_face f = faces[i];
        if (f.a >= vertex_count || f.b >= vertex_count || f.c >= vertex_count) {
            ok = 0;
            break;
        }
        if (wpos[f.a] != wpos[f.b] && wpos[f.a] != wpos[f.c] &&
            wpos[f.b] != wpos[f.c])
            filtered[count++] = f;
    }
    if (ok) ok = decimate(points, positions, filtered, count, target, NULL, wpos,
                           vertex_count, result, result_count);
    free(table);
    free(wpos);
    free(points);
    free(filtered);
    return ok;
}
