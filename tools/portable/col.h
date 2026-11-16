#ifndef GTASADS_COL_H
#define GTASADS_COL_H

#include <stddef.h>
#include <stdint.h>

int col_convert(const uint8_t *source, size_t size, int mode,
                double face_ratio, uint32_t min_faces, uint8_t **output,
                size_t *length, uint32_t *faces_in, uint32_t *faces_out);

#endif
