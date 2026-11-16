#ifndef GTASADS_IPL_H
#define GTASADS_IPL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t model;
    double transform[7];
    int32_t area, lod;
} ipl_instance;

int ipl_parse_binary(const uint8_t *source, size_t length,
                     ipl_instance **instances, size_t *count);
int ipl_parse_text(const char *source, size_t length,
                   ipl_instance **instances, size_t *count);
void ipl_rotation(double x, double y, double z, double w, double out[9]);

#endif
