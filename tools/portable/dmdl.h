#ifndef GTASADS_DMDL_H
#define GTASADS_DMDL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double ratio;
    double min_area, max_shift;
    uint32_t min_tris, max_tris;
    uint8_t uvs, normals, colors, skin, drop_damage, drop_vlo;
    uint8_t decimate, ground_qem, rigid_skin;
    const char *const *drop_frames;
    size_t drop_count;
} dmdl_options;

int dmdl_convert(const uint8_t *source, size_t source_size, const dmdl_options *options,
                 uint8_t **output, size_t *output_size, uint32_t *tris_in,
                 uint32_t *tris_out);

#endif
