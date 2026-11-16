#ifndef GTASADS_QEM_H
#define GTASADS_QEM_H

#include "cluster.h"

#include <stddef.h>
#include <stdint.h>

int qem_decimate(const cluster_vertex *vertices, size_t vertex_count,
                 const cluster_face *faces, size_t face_count, size_t target,
                 const uint8_t *bones, cluster_face **result, size_t *result_count);
int wedge_decimate(const cluster_vertex *vertices, size_t vertex_count,
                   const cluster_face *faces, size_t face_count, size_t target,
                   cluster_face **result, size_t *result_count);

#endif
