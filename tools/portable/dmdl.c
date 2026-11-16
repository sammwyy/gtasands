#include "dmdl.h"

#include "cluster.h"
#include "dff_mesh.h"
#include "geometry.h"
#include "qem.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size, capacity;
} buffer;

typedef struct {
    double pos[3], uv[2], norm[3];
    uint8_t color[4];
    uint8_t bone;
    uint32_t original;
} vertex;

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static int append(buffer *out, const void *data, size_t n) {
    if (n > SIZE_MAX - out->size) return 0;
    size_t needed = out->size + n;
    if (needed > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 1024;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        uint8_t *next = realloc(out->data, capacity);
        if (!next) return 0;
        out->data = next;
        out->capacity = capacity;
    }
    if (n) memcpy(out->data + out->size, data, n);
    out->size = needed;
    return 1;
}

static double nearest(double v) {
    double whole;
    double part = modf(v, &whole);
    if (part > 0.5 || (part == 0.5 && fmod(whole, 2))) return whole + 1;
    if (part < -0.5 || (part == -0.5 && fmod(whole, 2))) return whole - 1;
    return whole;
}

static int32_t fx(double value) {
    value *= 4096.0;
    if (!isfinite(value)) return 0;
    if (value >= INT32_MAX) return INT32_MAX;
    if (value <= INT32_MIN) return INT32_MIN;
    return (int32_t)nearest(value);
}

static int16_t fx16(double value) {
    value *= 4096.0;
    if (!isfinite(value)) return 0;
    if (value >= INT16_MAX) return INT16_MAX;
    if (value <= INT16_MIN) return INT16_MIN;
    return (int16_t)nearest(value);
}

static unsigned char lower(unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + 32) : ch;
}

static int matches(const char *pattern, const char *name) {
    if (!*pattern) return !*name;
    if (*pattern == '*') {
        do ++pattern; while (*pattern == '*');
        if (!*pattern) return 1;
        for (; *name; ++name)
            if (matches(pattern, name)) return 1;
        return matches(pattern, name);
    }
    return *name && (*pattern == '?' ||
                     lower((unsigned char)*pattern) == lower((unsigned char)*name)) &&
           matches(pattern + 1, name + 1);
}

static int dropped(const dff_model *m, uint32_t frame, const dmdl_options *opts) {
    size_t checks = 0;
    while (frame < m->frame_count && ++checks <= m->frame_count) {
        const dff_frame *f = &m->frames[frame];
        if ((opts->drop_damage && matches("*_dam", f->name)) ||
            (opts->drop_vlo && matches("*_vlo", f->name))) return 1;
        for (size_t i = 0; i < opts->drop_count; ++i)
            if (matches(opts->drop_frames[i], f->name)) return 1;
        if (f->parent < 0 || (uint32_t)f->parent >= m->frame_count) break;
        frame = (uint32_t)f->parent;
    }
    return 0;
}

static void multiply(const double *a, const double *b, double *out) {
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col) {
            double sum = 0;
            for (unsigned k = 0; k < 3; ++k)
                sum += a[row * 3 + k] * b[k * 3 + col];
            out[row * 3 + col] = sum;
        }
    for (unsigned row = 0; row < 3; ++row) {
        double sum = 0;
        for (unsigned col = 0; col < 3; ++col)
            sum += a[9 + col] * b[col * 3 + row];
        out[9 + row] = sum + b[9 + row];
    }
}

static int invert(const double *m, double *out) {
    double a = m[0], b = m[1], c = m[2];
    double d = m[3], e = m[4], f = m[5];
    double g = m[6], h = m[7], i = m[8];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!det || !isfinite(det)) return 0;
    out[0] = (e * i - f * h) / det;
    out[1] = (c * h - b * i) / det;
    out[2] = (b * f - c * e) / det;
    out[3] = (f * g - d * i) / det;
    out[4] = (a * i - c * g) / det;
    out[5] = (c * d - a * f) / det;
    out[6] = (d * h - e * g) / det;
    out[7] = (b * g - a * h) / det;
    out[8] = (a * e - b * d) / det;
    for (unsigned j = 0; j < 3; ++j) {
        double sum = 0;
        for (unsigned k = 0; k < 3; ++k)
            sum += m[9 + k] * out[k * 3 + j];
        out[9 + j] = -sum;
    }
    return 1;
}

static int append_skin(buffer *out, const dff_model *model, const dff_skin *skin) {
    size_t n = skin->num_bones;
    if (n && !model->hanim_count) return 0;
    double *ib = n ? malloc(n * 12 * sizeof *ib) : NULL;
    double *bindw = n ? malloc(n * 12 * sizeof *bindw) : NULL;
    int32_t *parents = model->hanim_count ? malloc(model->hanim_count * sizeof *parents) : NULL;
    int32_t *stack = model->hanim_count ? malloc(model->hanim_count * sizeof *stack) : NULL;
    if ((n && (!ib || !bindw)) || (model->hanim_count && (!parents || !stack))) {
        free(ib); free(bindw); free(parents); free(stack);
        return 0;
    }
    int ok = 1;
    for (size_t i = 0; i < n && ok; ++i) {
        const double *raw = skin->inv_matrices + i * 16;
        for (unsigned j = 0; j < 12; ++j)
            ib[i * 12 + j] = raw[(j / 3) * 4 + j % 3];
        for (unsigned j = 0; j < 12 && ok; ++j) {
            uint8_t bytes[4];
            put32(bytes, (uint32_t)fx(ib[i * 12 + j]));
            ok = append(out, bytes, sizeof bytes);
        }
        if (ok) ok = invert(ib + i * 12, bindw + i * 12);
    }
    size_t top = 0;
    int32_t parent = -1;
    for (size_t i = 0; i < model->hanim_count; ++i) {
        parents[i] = parent;
        if (model->hanim[i].flags & 2) stack[top++] = parent;
        parent = (int32_t)i;
        if (model->hanim[i].flags & 1)
            parent = top ? stack[--top] : -1;
    }
    for (size_t i = 0; i < n && ok; ++i) {
        int32_t par, id;
        if (i < model->hanim_count) {
            par = parents[i];
            id = model->hanim[i].id;
        } else {
            par = (int32_t)model->hanim_count - 1;
            id = model->hanim[model->hanim_count - 1].id + 1;
        }
        if (par < INT16_MIN || par > INT16_MAX ||
            id < INT16_MIN || id > INT16_MAX || par >= (int32_t)n) {
            ok = 0;
            break;
        }
        uint8_t entry[4];
        put16(entry, (uint16_t)par);
        put16(entry + 2, (uint16_t)id);
        ok = append(out, entry, sizeof entry);
    }
    for (size_t i = 0; i < n && ok; ++i) {
        int32_t par = i < model->hanim_count ? parents[i] :
                      (int32_t)model->hanim_count - 1;
        double loc[12];
        if (par < 0) memcpy(loc, bindw + i * 12, sizeof loc);
        else multiply(bindw + i * 12, ib + par * 12, loc);
        for (unsigned j = 0; j < 12 && ok; ++j) {
            uint8_t bytes[4];
            put32(bytes, (uint32_t)fx(loc[j]));
            ok = append(out, bytes, sizeof bytes);
        }
    }
    for (size_t i = 0; i < n && ok; ++i) {
        int32_t par = i < model->hanim_count ? parents[i] :
                      (int32_t)model->hanim_count - 1;
        for (unsigned j = 0; j < 3 && ok; ++j) {
            double value = 0;
            if (par >= 0) {
                double *inv = ib + par * 12, *w = bindw + i * 12;
                for (unsigned k = 0; k < 3; ++k)
                    value += w[9 + k] * inv[k * 3 + j];
                value += inv[9 + j];
            }
            uint8_t bytes[4];
            put32(bytes, (uint32_t)fx(value));
            ok = append(out, bytes, sizeof bytes);
        }
    }
    free(ib);
    free(bindw);
    free(parents);
    free(stack);
    return ok;
}

static int synthesize_rigid(dff_model *model, const dmdl_options *opts) {
    if (!model->hanim_count || model->hanim_count > 64) return 0;
    size_t frames = model->frame_count, ngeoms = model->geometry_count;
    double *world = frames ? malloc(frames * 12 * sizeof *world) : NULL;
    double *inverse = malloc(model->hanim_count * 12 * sizeof *inverse);
    uint8_t *seen = ngeoms ? calloc(ngeoms, 1) : NULL;
    if ((frames && !world) || !inverse || (ngeoms && !seen)) {
        free(world); free(inverse); free(seen);
        return 0;
    }
    int ok = 1;
    for (size_t i = 0; i < frames && ok; ++i) {
        const dff_frame *f = &model->frames[i];
        double local[12];
        memcpy(local, f->rot, sizeof f->rot);
        memcpy(local + 9, f->pos, sizeof f->pos);
        if (f->parent >= 0) {
            if ((size_t)f->parent >= i) {
                ok = 0;
                break;
            }
            multiply(local, world + f->parent * 12, world + i * 12);
        } else memcpy(world + i * 12, local, sizeof local);
    }
    for (size_t i = 0; i < model->hanim_count && ok; ++i) {
        uint32_t frame = model->hanim[i].frame;
        ok = frame < frames &&
             invert(world + frame * 12, inverse + i * 12);
    }
    for (size_t i = 0; i < model->atomic_count && ok; ++i) {
        dff_atomic a = model->atomics[i];
        if (a.geometry >= ngeoms || dropped(model, a.frame, opts)) continue;
        if (a.frame >= frames || seen[a.geometry]) {
            ok = 0;
            break;
        }
        seen[a.geometry] = 1;
        dff_geometry *g = &model->geometries[a.geometry];
        if (g->skin) {
            ok = 0;
            break;
        }
        int32_t node = (int32_t)a.frame, bone = -1;
        for (size_t depth = 0; depth < frames && node >= 0; ++depth) {
            for (size_t j = 0; j < model->hanim_count; ++j)
                if (model->hanim[j].frame == (uint32_t)node)
                    bone = (int32_t)j;
            if (bone >= 0) break;
            node = model->frames[node].parent;
        }
        if (bone < 0) {
            ok = 0;
            break;
        }
        dff_skin *sk = calloc(1, sizeof *sk);
        if (!sk) {
            ok = 0;
            break;
        }
        g->skin = sk;
        sk->num_bones = model->hanim_count;
        size_t n = g->vertices ? g->vertex_count : 0;
        sk->indices = n ? calloc(n * 4, 1) : NULL;
        sk->weights = n ? calloc(n * 4, sizeof *sk->weights) : NULL;
        sk->inv_matrices = calloc(model->hanim_count * 16, sizeof *sk->inv_matrices);
        if ((n && (!sk->indices || !sk->weights)) || !sk->inv_matrices) {
            ok = 0;
            break;
        }
        for (size_t j = 0; j < model->hanim_count; ++j) {
            const double *inv = inverse + j * 12;
            double *mtx = sk->inv_matrices + j * 16;
            for (size_t k = 0; k < 12; ++k)
                mtx[(k / 3) * 4 + k % 3] = inv[k];
            mtx[15] = 1;
        }
        for (size_t j = 0; j < n; ++j) {
            sk->indices[j * 4] = (uint8_t)bone;
            sk->weights[j * 4] = 1.0;
            double original[3];
            memcpy(original, g->vertices + j * 3, sizeof original);
            for (size_t k = 0; k < 3; ++k) {
                double value = 0;
                for (size_t l = 0; l < 3; ++l)
                    value += original[l] * world[a.frame * 12 + l * 3 + k];
                g->vertices[j * 3 + k] = value + world[a.frame * 12 + 9 + k];
            }
        }
    }
    free(world);
    free(inverse);
    free(seen);
    return ok;
}

static int same_vertex(const vertex *a, const vertex *b, int uv, int norm, int color, int skin) {
    for (unsigned i = 0; i < 3; ++i) {
        if (a->pos[i] != b->pos[i]) return 0;
        if (norm && a->norm[i] != b->norm[i]) return 0;
    }
    for (unsigned i = 0; i < 2; ++i)
        if (uv && a->uv[i] != b->uv[i]) return 0;
    return (!color || !memcmp(a->color, b->color, 4)) &&
           (!skin || a->bone == b->bone);
}

static uint32_t hash_double(uint32_t seed, double value) {
    uint64_t bits = 0;
    if (value != 0) memcpy(&bits, &value, sizeof bits);
    seed = (seed ^ (uint32_t)bits) * 16777619u;
    return (seed ^ (uint32_t)(bits >> 32)) * 16777619u;
}

static uint32_t hash_vertex(const vertex *v, int uv, int norm, int color, int skin) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 3; ++i) {
        hash = hash_double(hash, v->pos[i]);
        if (norm) hash = hash_double(hash, v->norm[i]);
    }
    for (unsigned i = 0; i < 2; ++i)
        if (uv) hash = hash_double(hash, v->uv[i]);
    if (color)
        for (unsigned i = 0; i < 4; ++i)
            hash = (hash ^ v->color[i]) * 16777619u;
    if (skin) hash = (hash ^ v->bone) * 16777619u;
    return hash;
}

static vertex vertex_from(const dff_geometry *g, const cluster_result *r,
                          uint32_t i, int uv, int norm, int color, int skin) {
    vertex v = {0};
    v.original = i;
    const cluster_vertex *rep = r->representatives ? &r->representatives[i] : NULL;
    for (unsigned k = 0; k < 3; ++k) {
        v.pos[k] = rep ? (k == 0 ? rep->x : k == 1 ? rep->y : rep->z) :
                    g->vertices[i * 3 + k];
        if (norm) v.norm[k] = g->normals[i * 3 + k];
    }
    if (uv)
        for (unsigned k = 0; k < 2; ++k) v.uv[k] = g->uvs[i * 2 + k];
    if (color) memcpy(v.color, g->colors + i * 4, 4);
    if (skin) {
        size_t strongest = 0;
        for (size_t k = 1; k < 4; ++k)
            if (g->skin->weights[i * 4 + k] > g->skin->weights[i * 4 + strongest])
                strongest = k;
        v.bone = g->skin->indices[i * 4 + strongest];
    }
    return v;
}

static int is_flat(const cluster_vertex *positions, const cluster_face *faces, uint32_t count) {
    double total = 0, up = 0;
    for (uint32_t i = 0; i < count; ++i) {
        cluster_face t = faces[i];
        cluster_vertex a = positions[t.a], b = positions[t.b], c = positions[t.c];
        double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        double wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
        double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz;
        double nz = ux * wy - uy * wx;
        double length = sqrt(nx * nx + ny * ny + nz * nz);
        total += length;
        if (length && fabs(nz) / length > 0.7) up += length;
    }
    return total > 0 && up >= 0.6 * total;
}

static int prepare_geometry(const dff_geometry *g, const dmdl_options *opts,
                            uint8_t **out, size_t *size, uint32_t *in, uint32_t *written) {
    uint32_t n = g->vertices ? g->vertex_count : 0;
    int use_uv = opts->uvs && g->uvs, use_n = opts->normals && g->normals;
    int use_c = opts->colors && g->colors, use_b = opts->skin && g->skin;
    cluster_vertex *positions = n ? malloc((size_t)n * sizeof *positions) : NULL;
    cluster_face *faces = g->triangle_count ? malloc((size_t)g->triangle_count * sizeof *faces) : NULL;
    if ((n && !positions) || (g->triangle_count && !faces)) {
        free(positions);
        free(faces);
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i)
        positions[i] = (cluster_vertex){g->vertices ? g->vertices[i * 3] : 0,
                                        g->vertices ? g->vertices[i * 3 + 1] : 0,
                                        g->vertices ? g->vertices[i * 3 + 2] : 0};
    uint32_t valid = 0;
    for (uint32_t i = 0; i < g->triangle_count; ++i) {
        const uint16_t *t = &g->triangles[i * 4];
        if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
        faces[valid++] = (cluster_face){t[0], t[1], t[2], t[3]};
    }
    *in = g->triangle_count;
    uint32_t target = (uint32_t)(valid * opts->ratio);
    if (target < opts->min_tris) target = opts->min_tris;
    if (opts->max_tris && target > opts->max_tris) target = opts->max_tris;
    cluster_result reduced = {0};
    int ok;
    int wedge = opts->decimate == 2 ||
                (opts->ground_qem && is_flat(positions, faces, valid));
    if (opts->decimate == 1 || wedge) {
        uint8_t *bones = !wedge && opts->skin && g->skin && n ? malloc(n) : NULL;
        if (!wedge && opts->skin && g->skin && n && !bones) {
            free(positions);
            free(faces);
            return 0;
        }
        if (bones)
            for (uint32_t i = 0; i < n; ++i) {
                size_t strongest = 0;
                for (size_t k = 1; k < 4; ++k)
                    if (g->skin->weights[i * 4 + k] > g->skin->weights[i * 4 + strongest])
                        strongest = k;
                bones[i] = g->skin->indices[i * 4 + strongest];
            }
        cluster_face *simplified = NULL;
        size_t count = 0;
        ok = wedge ? wedge_decimate(positions, n, faces, valid, target,
                                     &simplified, &count) :
                     qem_decimate(positions, n, faces, valid, target,
                                  bones, &simplified, &count);
        free(bones);
        if (ok) {
            free(faces);
            faces = simplified;
            valid = (uint32_t)count;
            reduced.kept = valid ? malloc((size_t)valid * sizeof *reduced.kept) : NULL;
            if (valid && !reduced.kept) ok = 0;
            else {
                reduced.count = valid;
                for (uint32_t i = 0; i < valid; ++i) reduced.kept[i] = i;
            }
        }
    } else
        ok = cluster_decimate_limited(positions, n, faces, valid, target,
                                      opts->min_area, opts->max_shift, opts->max_tris,
                                      &reduced);
    free(positions);
    if (!ok) {
        free(faces);
        return 0;
    }
    uint32_t *map = n ? malloc((size_t)n * sizeof *map) : NULL;
    vertex *compact = n ? malloc((size_t)n * sizeof *compact) : NULL;
    size_t capacity = 8;
    while (capacity < (size_t)n * 2 + 1 && capacity <= SIZE_MAX / 2) capacity *= 2;
    uint32_t *table = calloc(capacity, sizeof *table);
    uint32_t *kept = reduced.count ? malloc(reduced.count * sizeof *kept) : NULL;
    if ((n && (!map || !compact)) || !table || (reduced.count && !kept)) {
        ok = 0;
        goto end;
    }
    for (uint32_t i = 0; i < n; ++i) map[i] = UINT32_MAX;
    uint32_t count = 0;
    for (size_t i = 0; i < reduced.count && ok; ++i) {
        cluster_face f = faces[reduced.kept[i]];
        uint32_t indices[3] = {f.a, f.b, f.c};
        for (unsigned j = 0; j < 3; ++j) {
            uint32_t original = indices[j];
            if (map[original] != UINT32_MAX) continue;
            vertex v = vertex_from(g, &reduced, original, use_uv, use_n, use_c, use_b);
            size_t slot = hash_vertex(&v, use_uv, use_n, use_c, use_b) & (capacity - 1);
            while (table[slot] && !same_vertex(&compact[table[slot] - 1], &v,
                                               use_uv, use_n, use_c, use_b))
                slot = (slot + 1) & (capacity - 1);
            if (!table[slot]) {
                if (count == UINT16_MAX) {
                    ok = 0;
                    break;
                }
                compact[count++] = v;
                table[slot] = count;
            }
            map[original] = table[slot] - 1;
        }
    }
    if (!ok) goto end;
    for (size_t i = 0; i < reduced.count; ++i) kept[i] = reduced.kept[i];
    for (size_t i = 1; i < reduced.count; ++i) {
        uint32_t face = kept[i];
        size_t j = i;
        while (j && faces[kept[j - 1]].material > faces[face].material) {
            kept[j] = kept[j - 1];
            --j;
        }
        kept[j] = face;
    }
    uint16_t *indices = reduced.count ? malloc(reduced.count * 3 * sizeof *indices) : NULL;
    geometry_material *mats = calloc(g->material_count ? g->material_count : 1, sizeof *mats);
    double *coords = count ? malloc((size_t)count * 3 * sizeof *coords) : NULL;
    double *uvs = use_uv && count ? malloc((size_t)count * 2 * sizeof *uvs) : NULL;
    double *normals = use_n && count ? malloc((size_t)count * 3 * sizeof *normals) : NULL;
    uint8_t *colors = use_c && count ? malloc((size_t)count * 4) : NULL;
    uint8_t *bones = use_b && count ? malloc(count) : NULL;
    if ((reduced.count && !indices) || !mats || (count && !coords) ||
        (use_uv && count && !uvs) || (use_n && count && !normals) ||
        (use_c && count && !colors) || (use_b && count && !bones)) {
        free(indices); free(mats); free(coords); free(uvs); free(normals); free(colors);
        free(bones);
        ok = 0;
        goto end;
    }
    for (uint32_t i = 0; i < count; ++i) {
        memcpy(coords + i * 3, compact[i].pos, sizeof compact[i].pos);
        if (use_uv) memcpy(uvs + i * 2, compact[i].uv, sizeof compact[i].uv);
        if (use_n) memcpy(normals + i * 3, compact[i].norm, sizeof compact[i].norm);
        if (use_c) memcpy(colors + i * 4, compact[i].color, 4);
        if (use_b) bones[i] = compact[i].bone;
    }
    uint32_t index_count = 0, material_count = 0;
    for (uint32_t mi = 0; mi < (g->material_count ? g->material_count : 1); ++mi) {
        uint32_t first_index = index_count;
        for (size_t i = 0; i < reduced.count; ++i) {
            cluster_face f = faces[kept[i]];
            if (g->material_count && f.material != mi) continue;
            indices[index_count++] = (uint16_t)map[f.a];
            indices[index_count++] = (uint16_t)map[f.b];
            indices[index_count++] = (uint16_t)map[f.c];
        }
        if (index_count > first_index) {
            geometry_material *dst = &mats[material_count++];
            dst->first = first_index;
            dst->count = index_count - first_index;
            dst->alpha = 255;
            dst->color = 0x7fff;
            if (g->material_count) {
                const dff_material *src = &g->materials[mi];
                memcpy(dst->texture, src->texture, 31);
                dst->alpha = src->rgba[3];
                dst->color = (uint16_t)((src->rgba[0] >> 3) |
                             (src->rgba[1] >> 3) << 5 | (src->rgba[2] >> 3) << 10);
            }
        }
    }
    *written = index_count / 3;
    double empty_uv[2] = {0}, empty_normals[3] = {0};
    uint8_t empty_colors[4] = {0}, empty_bone = 0;
    ok = geometry_encode(coords, use_uv ? (uvs ? uvs : empty_uv) : NULL,
                         use_n ? (normals ? normals : empty_normals) : NULL,
                         use_c ? (colors ? colors : empty_colors) : NULL,
                         use_b ? (bones ? bones : &empty_bone) : NULL, indices, count,
                         index_count, mats, material_count, out, size);
    free(indices);
    free(mats);
    free(coords);
    free(uvs);
    free(normals);
    free(colors);
    free(bones);
end:
    free(map);
    free(compact);
    free(table);
    free(kept);
    free(faces);
    cluster_free(&reduced);
    return ok;
}

int dmdl_convert(const uint8_t *source, size_t source_size, const dmdl_options *options,
                 uint8_t **output, size_t *output_size, uint32_t *tris_in,
                 uint32_t *tris_out) {
    if (!source || !options || !output || !output_size || !tris_in || !tris_out ||
        options->ratio < 0 || options->ratio > 1 || options->decimate > 2 ||
        options->min_area < 0 || options->max_shift < 0 ||
        (options->drop_count && !options->drop_frames)) return 0;
    dff_model model;
    if (!dff_parse_model(source, source_size, &model)) return 0;
    uint8_t *selected = model.geometry_count ? calloc(model.geometry_count, 1) : NULL;
    uint16_t *mapping = model.geometry_count ? calloc(model.geometry_count, sizeof *mapping) : NULL;
    if (model.geometry_count && (!selected || !mapping)) {
        free(selected);
        free(mapping);
        dff_free_model(&model);
        return 0;
    }
    size_t atoms = 0, geoms = 0;
    for (size_t i = 0; i < model.atomic_count; ++i) {
        dff_atomic a = model.atomics[i];
        if (a.geometry >= model.geometry_count || dropped(&model, a.frame, options)) continue;
        ++atoms;
        selected[a.geometry] = 1;
    }
    for (uint32_t i = 0; i < model.geometry_count; ++i)
        if (selected[i]) mapping[i] = (uint16_t)geoms++;
    if (options->rigid_skin && !synthesize_rigid(&model, options)) {
        free(selected);
        free(mapping);
        dff_free_model(&model);
        return 0;
    }
    const dff_skin *skin = NULL;
    if (options->skin)
        for (uint32_t i = 0; i < model.geometry_count; ++i)
            if (selected[i] && model.geometries[i].skin) {
                skin = model.geometries[i].skin;
                break;
            }
    int ok = geoms <= UINT16_MAX && atoms <= UINT16_MAX && model.frame_count <= UINT16_MAX;
    buffer out = {0};
    uint8_t header[32] = {'D', 'M', 'D', 'L', 2, 0};
    if (ok) {
        put16(header + 6, skin ? 1 : 0);
        put16(header + 8, (uint16_t)model.frame_count);
        put16(header + 10, (uint16_t)geoms);
        put16(header + 12, (uint16_t)atoms);
        put16(header + 14, skin ? (uint16_t)skin->num_bones : 0);
        double largest = -INFINITY;
        for (uint32_t i = 0; i < model.geometry_count; ++i)
            if (selected[i] && model.geometries[i].sphere[3] > largest) {
                for (unsigned j = 0; j < 4; ++j)
                    put32(header + 16 + j * 4, (uint32_t)fx(model.geometries[i].sphere[j]));
                largest = model.geometries[i].sphere[3];
            }
        ok = append(&out, header, sizeof header);
    }
    for (uint32_t i = 0; i < model.frame_count && ok; ++i) {
        dff_frame *f = &model.frames[i];
        uint8_t record[60] = {0};
        memcpy(record, f->name, 23);
        put16(record + 24, (uint16_t)f->parent);
        put16(record + 26, (uint16_t)f->bone_id);
        for (unsigned j = 0; j < 9; ++j)
            put16(record + 28 + j * 2, (uint16_t)fx16(f->rot[j]));
        for (unsigned j = 0; j < 3; ++j)
            put32(record + 48 + j * 4, (uint32_t)fx(f->pos[j]));
        ok = append(&out, record, sizeof record);
    }
    for (size_t i = 0; i < model.atomic_count && ok; ++i) {
        dff_atomic a = model.atomics[i];
        if (a.geometry >= model.geometry_count || dropped(&model, a.frame, options)) continue;
        uint8_t record[4];
        put16(record, (uint16_t)a.frame);
        put16(record + 2, mapping[a.geometry]);
        ok = append(&out, record, sizeof record);
    }
    if (ok && skin) ok = append_skin(&out, &model, skin);
    size_t offsets = out.size;
    for (size_t i = 0; i < geoms * 4 && ok; ++i) {
        const uint8_t zero = 0;
        ok = append(&out, &zero, 1);
    }
    uint32_t total_in = 0, total_out = 0;
    for (uint32_t i = 0; i < model.geometry_count && ok; ++i) {
        if (!selected[i]) continue;
        if (out.size > UINT32_MAX) {
            ok = 0;
            break;
        }
        put32(out.data + offsets + mapping[i] * 4, (uint32_t)out.size);
        uint8_t *encoded = NULL;
        size_t length = 0;
        uint32_t input_tris = 0, output_tris = 0;
        ok = prepare_geometry(&model.geometries[i], options, &encoded, &length,
                              &input_tris, &output_tris);
        if (ok && length <= UINT32_MAX - out.size)
            ok = append(&out, encoded, length);
        else ok = 0;
        free(encoded);
        if (input_tris > UINT32_MAX - total_in || output_tris > UINT32_MAX - total_out)
            ok = 0;
        total_in += input_tris;
        total_out += output_tris;
    }
    if (ok) {
        *output = out.data;
        *output_size = out.size;
        *tris_in = total_in;
        *tris_out = total_out;
    } else free(out.data);
    free(selected);
    free(mapping);
    dff_free_model(&model);
    return ok;
}
