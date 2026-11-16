#ifndef GTASADS_POINTS_H
#define GTASADS_POINTS_H

#include <stddef.h>
#include <stdint.h>

int points_from_denx(const uint8_t *input, size_t size, uint8_t **output, size_t *length);

#endif
