#ifndef GTASADS_RADAR_H
#define GTASADS_RADAR_H

#include <stddef.h>
#include <stdint.h>

int radar_convert(const uint8_t *txd, size_t size, uint8_t **output, size_t *length);

#endif
