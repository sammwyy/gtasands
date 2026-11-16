#ifndef GTASADS_CLUSTER_H
#define GTASADS_CLUSTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x, y, z;
} cluster_vertex;

typedef struct {
    uint32_t a, b, c;
    uint16_t material;
} cluster_face;

typedef struct {
    cluster_vertex *representatives;
    uint32_t *kept;
    size_t count;
} cluster_result;

int cluster_decimate(const cluster_vertex *vertices, size_t vertex_count,
                     const cluster_face *faces, size_t face_count,
                     size_t target, cluster_result *result);
int cluster_decimate_limited(const cluster_vertex *vertices, size_t vertex_count,
                             const cluster_face *faces, size_t face_count, size_t target,
                             double min_area, double max_shift, size_t max_output,
                             cluster_result *result);
void cluster_free(cluster_result *result);

#endif
