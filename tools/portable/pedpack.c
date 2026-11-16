#include "pedpack.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

int pedpack_encode(const pedpack_input *p, uint8_t **output, size_t *length) {
    if (!p || !output || !length || !p->gw || !p->gh || !p->cell)
        return 0;
    uint64_t offsets[14];
    uint64_t total = 100;
    for (size_t i = 0; i < 14; ++i) {
        if (p->lengths[i] && !p->parts[i]) return 0;
        offsets[i] = total;
        total += p->lengths[i];
        if (total > UINT32_MAX || total > SIZE_MAX) return 0;
    }
    uint8_t *out = malloc((size_t)total);
    if (!out) return 0;
    memcpy(out, "DPED", 4);
    put16(out + 4, 5);
    put16(out + 6, p->models);
    put16(out + 8, p->stats);
    put16(out + 10, p->anims);
    put16(out + 12, p->groups);
    put16(out + 14, p->zones);
    put32(out + 16, p->nodes);
    put32(out + 20, p->links);
    put16(out + 24, (uint16_t)p->gx0);
    put16(out + 26, (uint16_t)p->gy0);
    put16(out + 28, p->gw);
    put16(out + 30, p->gh);
    put16(out + 32, p->cell);
    put16(out + 34, 0);
    for (size_t i = 0; i < 10; ++i)
        put32(out + 36 + i * 4, (uint32_t)offsets[i]);
    put32(out + 76, p->road_nodes);
    put32(out + 80, p->road_links);
    put32(out + 84, (uint32_t)offsets[10]);
    put32(out + 88, (uint32_t)offsets[11]);
    put32(out + 92, (uint32_t)offsets[12]);
    put32(out + 96, (uint32_t)offsets[13]);
    for (size_t i = 0; i < 14; ++i)
        if (p->lengths[i]) memcpy(out + offsets[i], p->parts[i], p->lengths[i]);
    *output = out;
    *length = (size_t)total;
    return 1;
}

int pedpop_rescale(int32_t *rows, size_t count) {
    if (!rows && count) return 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t sum = 0;
        for (size_t j = 6; j < 24; ++j)
            sum += rows[i * 24 + j];
        if (sum <= 0 || sum >= 100) continue;
        for (size_t j = 6; j < 24; ++j) {
            int64_t numerator = (int64_t)rows[i * 24 + j] * 100;
            int64_t quotient = numerator / sum;
            if (numerator % sum < 0) --quotient;
            if (quotient < INT32_MIN || quotient > INT32_MAX) return 0;
            rows[i * 24 + j] = (int32_t)quotient;
        }
    }
    return 1;
}

int pedpop_parse(const char *text, size_t length, int32_t **rows, size_t *count) {
    if (!text || !rows || !count || length == SIZE_MAX) return 0;
    *rows = NULL;
    *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    int32_t *result = calloc(480 * 24, sizeof *result);
    if (!result) {
        free(copy);
        return 0;
    }
    size_t used = 0;
    for (char *line = copy; line < copy + length && used < 480;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strstr(line, "//");
        if (comment) *comment = 0;
        char *p = line;
        int valid = 1;
        for (size_t i = 0; i < 24; ++i) {
            while (isspace((unsigned char)*p)) ++p;
            if (!*p) {
                valid = 0;
                break;
            }
            char *tail;
            long value = strtol(p, &tail, 10);
            if (p == tail || (*tail && !isspace((unsigned char)*tail)) ||
                value < INT32_MIN || value > INT32_MAX) {
                valid = 0;
                break;
            }
            result[used * 24 + i] = (int32_t)value;
            p = tail;
        }
        if (valid) ++used;
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (!pedpop_rescale(result, 480)) {
        free(result);
        return 0;
    }
    *rows = result;
    *count = 480;
    return 1;
}

int pedpack_paths(const pedpack_path_node *nodes, size_t count, size_t cell_count,
                  int road, uint8_t **node_blob, size_t *node_length,
                  uint8_t **link_blob, size_t *link_length,
                  uint8_t **cells, size_t *cells_length, uint32_t *links) {
    if ((count && !nodes) || !node_blob || !node_length || !link_blob ||
        !link_length || !cells || !cells_length || !links ||
        count > UINT16_MAX || cell_count > SIZE_MAX / 4) return 0;
    size_t total_links = 0;
    uint32_t prev_cell = 0;
    for (size_t i = 0; i < count; ++i) {
        const pedpack_path_node *n = &nodes[i];
        if (n->cell >= cell_count || (i && n->cell < prev_cell) ||
            n->link_count > UINT8_MAX || (n->link_count && !n->links) ||
            n->link_count > UINT32_MAX - total_links) return 0;
        prev_cell = n->cell;
        total_links += n->link_count;
    }
    size_t nbytes = count * 16;
    size_t lbytes = total_links * 2 + (!road && (total_links & 1) ? 2 : 0);
    uint8_t *nblob = nbytes ? calloc(nbytes, 1) : NULL;
    uint8_t *lblob = lbytes ? calloc(lbytes, 1) : NULL;
    uint8_t *cblob = cell_count ? calloc(cell_count * 4, 1) : NULL;
    if ((nbytes && !nblob) || (lbytes && !lblob) || (cell_count && !cblob)) {
        free(nblob); free(lblob); free(cblob);
        return 0;
    }
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        const pedpack_path_node *n = &nodes[i];
        uint8_t *cell = cblob + n->cell * 4, *record = nblob + i * 16;
        uint16_t present = (uint16_t)cell[2] | (uint16_t)cell[3] << 8;
        if (!present) put16(cell, (uint16_t)i);
        put16(cell + 2, (uint16_t)(present + 1));
        put16(record, (uint16_t)n->x);
        put16(record + 2, (uint16_t)n->y);
        put16(record + 4, (uint16_t)n->z);
        put32(record + 8, (uint32_t)offset);
        record[12] = (uint8_t)n->link_count;
        record[13] = n->width;
        uint8_t f = road ?
            (uint8_t)(((n->flags >> 5) & 1) |
                      (((n->flags >> 6) & 1) << 1) |
                      (((n->flags >> 7) & 1) << 2)) :
            (uint8_t)((n->flags & (1u << 10) ? 1 : 0) |
                      (n->flags & (1u << 5) ? 2 : 0) |
                      (n->flags & (1u << 4) ? 4 : 0));
        record[14] = (uint8_t)(f | (((n->flags >> 16) & 15) << 4));
        for (size_t j = 0; j < n->link_count; ++j)
            put16(lblob + (offset + j) * 2, n->links[j]);
        offset += n->link_count;
    }
    *node_blob = nblob;
    *node_length = nbytes;
    *link_blob = lblob;
    *link_length = lbytes;
    *cells = cblob;
    *cells_length = cell_count * 4;
    *links = (uint32_t)total_links;
    return 1;
}
