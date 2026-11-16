#include "dff_mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t type;
    size_t start, end;
} chunk;

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static double read_float(const uint8_t *p, int finite_only) {
    uint32_t bits = read32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return finite_only && !isfinite(value) ? 0.0 : value;
}

static int next(const uint8_t *data, size_t limit, size_t *pos, chunk *c) {
    size_t p = *pos;
    if (p > limit || limit - p < 12) return 0;
    c->type = read32(data + p);
    c->start = p + 12;
    size_t n = read32(data + p + 4);
    c->end = n < limit - c->start ? c->start + n : limit;
    *pos = c->start + n;
    return 1;
}

static int first(const uint8_t *data, size_t start, size_t end,
                 uint32_t kind, chunk *match) {
    size_t pos = start;
    chunk c;
    while (next(data, end, &pos, &c))
        if (c.type == kind) {
            *match = c;
            return 1;
        }
    return 0;
}

static void read_name(char *out, size_t capacity, const uint8_t *src, size_t n) {
    size_t i = 0;
    for (; i + 1 < capacity && i < n && src[i]; ++i) out[i] = (char)src[i];
    out[i] = 0;
}

static int append_nodes(const uint8_t *data, chunk c, dff_model *m) {
    if (c.end - c.start < 20) return 1;
    int32_t count = (int32_t)read32(data + c.start + 8);
    if (count <= 0 || (size_t)count > (c.end - c.start - 20) / 12) return 1;
    if ((size_t)count > UINT32_MAX - m->hanim_count) return 0;
    size_t old = m->hanim_count, total = old + (size_t)count;
    dff_hanim *nodes = realloc(m->hanim, total * sizeof *nodes);
    if (!nodes) return 0;
    m->hanim = nodes;
    for (size_t i = 0; i < (size_t)count; ++i) {
        const uint8_t *p = data + c.start + 20 + i * 12;
        nodes[old + i] = (dff_hanim){(int32_t)read32(p), read32(p + 4), read32(p + 8)};
    }
    m->hanim_count = (uint32_t)total;
    return 1;
}

static int parse_frames(const uint8_t *data, chunk list, dff_model *m) {
    chunk raw;
    if (!first(data, list.start, list.end, 1, &raw) ||
        raw.end - raw.start < 4) return 0;
    uint32_t n = read32(data + raw.start);
    if (n > (raw.end - raw.start - 4) / 56) return 0;
    m->frames = n ? calloc(n, sizeof *m->frames) : NULL;
    if (n && !m->frames) return 0;
    m->frame_count = n;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *p = data + raw.start + 4 + (size_t)i * 56;
        for (size_t j = 0; j < 9; ++j)
            m->frames[i].rot[j] = read_float(p + j * 4, 0);
        for (size_t j = 0; j < 3; ++j)
            m->frames[i].pos[j] = read_float(p + 36 + j * 4, 0);
        m->frames[i].parent = (int32_t)read32(p + 48);
        m->frames[i].bone_id = -1;
    }
    size_t pos = list.start, index = 0;
    chunk ext;
    while (next(data, list.end, &pos, &ext)) {
        if (ext.type != 3) continue;
        if (index >= n) break;
        size_t inner = ext.start;
        chunk c;
        while (next(data, ext.end, &inner, &c)) {
            if (c.type == 0x253f2fe)
                read_name(m->frames[index].name, sizeof m->frames[index].name,
                          data + c.start, c.end - c.start);
            else if (c.type == 0x11e) {
                if (c.end - c.start < 8 ||
                    !append_nodes(data, c, m)) return 0;
                m->frames[index].bone_id = (int32_t)read32(data + c.start + 4);
            }
        }
        ++index;
    }
    return 1;
}

static int parse_material(const uint8_t *data, chunk c, dff_material *material) {
    chunk body;
    if (!first(data, c.start, c.end, 1, &body) ||
        body.end - body.start < 8) return 0;
    memcpy(material->rgba, data + body.start + 4, 4);
    chunk texture, label;
    if (first(data, c.start, c.end, 6, &texture) &&
        first(data, texture.start, texture.end, 2, &label))
        read_name(material->texture, sizeof material->texture,
                  data + label.start, label.end - label.start);
    return 1;
}

static int parse_materials(const uint8_t *data, chunk geometry, dff_geometry *g) {
    chunk list, body;
    if (!first(data, geometry.start, geometry.end, 8, &list)) return 1;
    if (!first(data, list.start, list.end, 1, &body) ||
        body.end - body.start < 4) return 0;
    uint32_t n = read32(data + body.start);
    if (n > (body.end - body.start - 4) / 4) return 0;
    g->materials = n ? calloc(n, sizeof *g->materials) : NULL;
    if (n && !g->materials) return 0;
    g->material_count = n;
    size_t position = list.start;
    chunk mat;
    uint32_t next_mat = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t ref = (int32_t)read32(data + body.start + 4 + (size_t)i * 4);
        if (ref >= 0) {
            if ((uint32_t)ref >= i) return 0;
            g->materials[i] = g->materials[ref];
            continue;
        }
        int found = 0;
        while (next(data, list.end, &position, &mat))
            if (mat.type == 7) {
                found = 1;
                break;
            }
        if (!found || !parse_material(data, mat, &g->materials[i])) return 0;
        ++next_mat;
    }
    (void)next_mat;
    return 1;
}

static int parse_skin(const uint8_t *data, chunk geometry, dff_geometry *g) {
    chunk extension, chunk_skin;
    if (!first(data, geometry.start, geometry.end, 3, &extension) ||
        !first(data, extension.start, extension.end, 0x116, &chunk_skin)) return 1;
    if (chunk_skin.end - chunk_skin.start < 4) return 0;
    const uint8_t *base = data + chunk_skin.start;
    size_t length = chunk_skin.end - chunk_skin.start, n = g->vertex_count;
    uint32_t bones = base[0];
    size_t offset = 4 + base[1];
    if (offset > length || n > (length - offset) / 4) return 0;
    dff_skin *skin = calloc(1, sizeof *skin);
    if (!skin) return 0;
    g->skin = skin;
    skin->num_bones = bones;
    if (n) {
        skin->indices = malloc(n * 4);
        skin->weights = malloc(n * 4 * sizeof *skin->weights);
        if (!skin->indices || !skin->weights) return 0;
        memcpy(skin->indices, base + offset, n * 4);
    }
    offset += n * 4;
    if (n > (length - offset) / 16) return 0;
    for (size_t i = 0; i < n * 4; ++i)
        skin->weights[i] = read_float(base + offset + i * 4, 0);
    offset += n * 16;
    if (bones > (length - offset) / 64) return 0;
    if (bones) {
        skin->inv_matrices = malloc((size_t)bones * 16 * sizeof *skin->inv_matrices);
        if (!skin->inv_matrices) return 0;
        for (size_t i = 0; i < (size_t)bones * 16; ++i)
            skin->inv_matrices[i] = read_float(base + offset + i * 4, 0);
    }
    return 1;
}

static int parse_geometry(const uint8_t *data, chunk geometry, dff_geometry *g) {
    chunk body;
    if (!first(data, geometry.start, geometry.end, 1, &body) ||
        body.end - body.start < 16) return 0;
    const uint8_t *p = data + body.start;
    uint32_t flags = read32(p), ntris = read32(p + 4), nverts = read32(p + 8);
    uint32_t nmorph = read32(p + 12);
    size_t pos = body.start + 16;
    g->flags = flags;
    g->vertex_count = nverts;
    if (!(flags & 0x01000000u)) {
        if (flags & 8) {
            if (nverts > (body.end - pos) / 4) return 0;
            g->colors = nverts ? malloc((size_t)nverts * 4) : NULL;
            if (nverts && !g->colors) return 0;
            if (nverts) memcpy(g->colors, data + pos, (size_t)nverts * 4);
            pos += (size_t)nverts * 4;
        }
        uint32_t textures = (flags >> 16) & 255u;
        if (!textures) textures = flags & 0x80 ? 2 : flags & 4 ? 1 : 0;
        for (uint32_t i = 0; i < textures; ++i) {
            if (nverts > (body.end - pos) / 8) return 0;
            if (!i && nverts) {
                g->uvs = malloc((size_t)nverts * 2 * sizeof *g->uvs);
                if (!g->uvs) return 0;
                for (size_t j = 0; j < (size_t)nverts * 2; ++j)
                    g->uvs[j] = read_float(data + pos + j * 4, 1);
            }
            pos += (size_t)nverts * 8;
        }
        if (ntris > (body.end - pos) / 8) return 0;
        if (ntris) {
            g->triangles = malloc((size_t)ntris * 4 * sizeof *g->triangles);
            if (!g->triangles) return 0;
        }
        g->triangle_count = ntris;
        for (size_t i = 0; i < ntris; ++i) {
            const uint8_t *t = data + pos + i * 8;
            g->triangles[i * 4] = read16(t + 2);
            g->triangles[i * 4 + 1] = read16(t);
            g->triangles[i * 4 + 2] = read16(t + 6);
            g->triangles[i * 4 + 3] = read16(t + 4);
        }
        pos += (size_t)ntris * 8;
    }
    if (nmorph) {
        if (pos > body.end || body.end - pos < 24) return 0;
        for (size_t i = 0; i < 4; ++i)
            g->sphere[i] = read_float(data + pos + i * 4, 1);
        uint32_t has_vertices = read32(data + pos + 16);
        uint32_t has_normals = read32(data + pos + 20);
        pos += 24;
        if (has_vertices) {
            if (nverts > (body.end - pos) / 12) return 0;
            g->vertices = nverts ? malloc((size_t)nverts * 3 * sizeof *g->vertices) : NULL;
            if (nverts && !g->vertices) return 0;
            for (size_t i = 0; i < (size_t)nverts * 3; ++i)
                g->vertices[i] = read_float(data + pos + i * 4, 1);
            pos += (size_t)nverts * 12;
        }
        if (has_normals) {
            if (nverts > (body.end - pos) / 12) return 0;
            g->normals = nverts ? malloc((size_t)nverts * 3 * sizeof *g->normals) : NULL;
            if (nverts && !g->normals) return 0;
            for (size_t i = 0; i < (size_t)nverts * 3; ++i)
                g->normals[i] = read_float(data + pos + i * 4, 1);
        }
    }
    return parse_materials(data, geometry, g) && parse_skin(data, geometry, g);
}

void dff_free_model(dff_model *model) {
    if (!model) return;
    for (uint32_t i = 0; i < model->geometry_count; ++i) {
        dff_geometry *g = &model->geometries[i];
        free(g->vertices);
        free(g->uvs);
        free(g->normals);
        free(g->colors);
        free(g->triangles);
        free(g->materials);
        if (g->skin) {
            free(g->skin->indices);
            free(g->skin->weights);
            free(g->skin->inv_matrices);
            free(g->skin);
        }
    }
    free(model->geometries);
    free(model->frames);
    free(model->hanim);
    free(model->atomics);
    memset(model, 0, sizeof *model);
}

int dff_parse_model(const uint8_t *data, size_t length, dff_model *model) {
    if (!data || !model) return 0;
    memset(model, 0, sizeof *model);
    chunk clump;
    if (!first(data, 0, length, 0x10, &clump)) return 0;
    size_t pos = clump.start;
    chunk c;
    while (next(data, clump.end, &pos, &c)) {
        if (c.type == 0x0e) {
            if (!parse_frames(data, c, model)) goto fail;
        } else if (c.type == 0x1a) {
            size_t inner = c.start;
            chunk g;
            while (next(data, c.end, &inner, &g)) {
                if (g.type != 0x0f) continue;
                if (model->geometry_count == UINT32_MAX) goto fail;
                dff_geometry *next_geoms = realloc(model->geometries,
                    ((size_t)model->geometry_count + 1) * sizeof *model->geometries);
                if (!next_geoms) goto fail;
                model->geometries = next_geoms;
                dff_geometry *current = &next_geoms[model->geometry_count++];
                memset(current, 0, sizeof *current);
                if (!parse_geometry(data, g, current)) goto fail;
            }
        } else if (c.type == 0x14) {
            chunk raw;
            if (!first(data, c.start, c.end, 1, &raw) ||
                raw.end - raw.start < 8 || model->atomic_count == UINT32_MAX)
                goto fail;
            dff_atomic *next_atoms = realloc(model->atomics,
                ((size_t)model->atomic_count + 1) * sizeof *model->atomics);
            if (!next_atoms) goto fail;
            model->atomics = next_atoms;
            next_atoms[model->atomic_count++] =
                (dff_atomic){read32(data + raw.start), read32(data + raw.start + 4)};
        }
    }
    return 1;
fail:
    dff_free_model(model);
    return 0;
}
