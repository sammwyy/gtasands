#ifndef GTASADS_WORLDCOL_H
#define GTASADS_WORLDCOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x, y, z;
} worldcol_vec3;

typedef struct {
    uint32_t a, b, c;
    uint8_t material, light;
} worldcol_face;

typedef struct {
    worldcol_vec3 center;
    double radius;
    uint8_t surface[4];
} worldcol_sphere;

typedef struct {
    worldcol_vec3 min, max;
    uint8_t surface[4];
} worldcol_box;

typedef struct {
    const worldcol_vec3 *vertices;
    size_t vertex_count;
    const worldcol_face *faces;
    size_t face_count;
    const worldcol_sphere *spheres;
    size_t sphere_count;
    const worldcol_box *boxes;
    size_t box_count;
} worldcol_model;

int worldcol_pack(const worldcol_model *model, uint8_t **output, size_t *length,
                  uint16_t *radius, uint32_t *face_count);

typedef struct {
    char name[24];
    uint8_t *record;
    size_t length;
    uint16_t radius;
    uint32_t faces;
} worldcol_entry;

int worldcol_parse_archive(const uint8_t *source, size_t length,
                           worldcol_entry **entries, size_t *entry_count);
void worldcol_free_archive(worldcol_entry *entries, size_t count);

#endif
