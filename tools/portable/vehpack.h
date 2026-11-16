#ifndef GTASADS_VEHPACK_H
#define GTASADS_VEHPACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char model[24], txd[24], gxt[8], anims[16];
    uint16_t id;
    uint8_t type, color_count;
    uint32_t flags;
    uint8_t palette[16];
    double wheel_front, wheel_rear;
    double handling[26];
    uint8_t gears, drive, engine, abs;
    uint32_t model_flags, handling_flags, value;
    uint8_t front_lights, rear_lights, anim_group, submerged;
    uint8_t special_kind, has_fly;
    double steer, special[15], flying[21];
} vehpack_model;

typedef struct {
    double x, y, z;
} vehpack_vec3;

typedef struct {
    vehpack_vec3 vertices[4];
    uint8_t count, flags;
} vehpack_water;

typedef struct {
    const vehpack_vec3 *nodes;
    size_t count;
} vehpack_track;

typedef struct {
    const vehpack_model *models;
    size_t model_count;
    const uint8_t *colors;
    size_t color_count;
    const uint8_t *surfaces;
    size_t surface_count;
    const double *adhesion;
    const vehpack_water *water;
    size_t water_count;
    const vehpack_track *tracks;
    size_t track_count;
} vehpack_input;

int vehpack_encode(const vehpack_input *input, uint8_t **output, size_t *length);

typedef struct {
    double mass, drag, engine_accel, max_vel, brake_decel, damage_mult;
    double max_flat, max_reverse;
    uint32_t handling_flags;
    uint8_t drive;
} vehpack_handling;

int vehpack_convert_handling(vehpack_handling *handling);

#endif
