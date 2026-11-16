#ifndef GTASADS_VEHPARSE_H
#define GTASADS_VEHPARSE_H

#include <stddef.h>
#include <stdint.h>

#include "vehpack.h"

typedef struct {
    int32_t id;
    char model[128], txd[128], handling[128], gxt[128], anims[128];
    uint8_t type;
    uint32_t flags;
    double wheel_front, wheel_rear, steer;
} vehparse_ide_entry;

int vehparse_ide(const char *text, size_t length, vehparse_ide_entry **entries,
                 size_t *count);

typedef struct {
    char name[32];
    double mass, turn_mass, drag, com[3], traction_mult, traction_loss, traction_bias;
    double max_vel, engine_accel, engine_inertia, brake_decel, brake_bias;
    double steering_lock, suspension[7], seat_offset, damage_mult, max_flat, max_reverse;
    int32_t submerged, gears, abs, value, front_lights, rear_lights, anim_group;
    uint64_t model_flags, handling_flags;
    uint8_t drive, engine;
} vehparse_handling_entry;

int vehparse_handling(const char *text, size_t length,
                      vehparse_handling_entry **entries, size_t *count);

typedef struct {
    vehpack_vec3 vertices[4];
    uint8_t count;
    int32_t flags;
} vehparse_water_entry;

int vehparse_water(const char *text, size_t length,
                   vehparse_water_entry **entries, size_t *count);
int vehparse_track(const char *text, size_t length,
                   vehpack_vec3 **nodes, size_t *count);

typedef struct {
    char name[32];
    uint8_t kind;
    double values[21];
} vehparse_special_entry;

int vehparse_special(const char *text, size_t length,
                     vehparse_special_entry **entries, size_t *count);

typedef struct {
    int32_t r, g, b;
} vehparse_color;

typedef struct {
    char name[128];
    int32_t pairs[8][2];
    uint8_t count;
} vehparse_car_color;

int vehparse_carcols(const char *text, size_t length,
                      vehparse_color **colors, size_t *color_count,
                      vehparse_car_color **cars, size_t *car_count);

typedef struct {
    int32_t group, wet, skid;
} vehparse_surface;

int vehparse_surfaces(const char *limits, size_t limits_length,
                       const char *info, size_t info_length, double adhesion[36],
                       vehparse_surface **surfaces, size_t *count);

#endif
