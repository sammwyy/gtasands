#ifndef GTASADS_DFF_MESH_H
#define GTASADS_DFF_MESH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[128];
    double rot[9];
    double pos[3];
    int32_t parent;
    int32_t bone_id;
} dff_frame;

typedef struct {
    uint8_t rgba[4];
    char texture[128];
} dff_material;

typedef struct {
    uint32_t num_bones;
    uint8_t *indices;
    double *weights;
    double *inv_matrices;
} dff_skin;

typedef struct {
    uint32_t flags;
    uint32_t vertex_count, triangle_count, material_count;
    double sphere[4];
    double *vertices, *uvs, *normals;
    uint8_t *colors;
    uint16_t *triangles;
    dff_material *materials;
    dff_skin *skin;
} dff_geometry;

typedef struct {
    uint32_t frame, geometry;
} dff_atomic;

typedef struct {
    int32_t id;
    uint32_t frame, flags;
} dff_hanim;

typedef struct {
    uint32_t frame_count, geometry_count, atomic_count, hanim_count;
    dff_frame *frames;
    dff_geometry *geometries;
    dff_atomic *atomics;
    dff_hanim *hanim;
} dff_model;

int dff_parse_model(const uint8_t *bytes, size_t length, dff_model *model);
void dff_free_model(dff_model *model);

#endif
