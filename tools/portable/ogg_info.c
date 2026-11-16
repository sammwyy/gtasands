#include "ogg_info.h"

#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const uint8_t *p) {
    return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

int ogg_read_vorbis_info(const uint8_t *data, size_t length, ogg_vorbis_info *info) {
    if (!data || !info) return 0;
    memset(info, 0, sizeof *info);
    size_t offset = 0;
    uint32_t serial = 0;
    int first = 1, eos = 0;
    while (length - offset >= 27) {
        const uint8_t *page = data + offset;
        if (memcmp(page, "OggS", 4) || page[4] != 0 ||
            (size_t)page[26] > length - offset - 27) return 0;
        size_t header = 27 + page[26];
        size_t payload = 0;
        for (size_t i = 0; i < page[26]; ++i) payload += page[27 + i];
        if (payload > length - offset - header) return 0;
        if (first) {
            if (!(page[5] & 2) || payload < 30) return 0;
            const uint8_t *ident = page + header;
            if (ident[0] != 1 || memcmp(ident + 1, "vorbis", 6) ||
                read32(ident + 7) != 0) return 0;
            uint8_t small = ident[28] & 15, large = ident[28] >> 4;
            if (small < 6 || large < small || large > 13 ||
                !ident[11] || !read32(ident + 12) || !(ident[29] & 1)) return 0;
            info->channels = ident[11];
            info->sample_rate = read32(ident + 12);
            info->small_block = (uint16_t)(1u << small);
            info->large_block = (uint16_t)(1u << large);
            serial = read32(page + 14);
            first = 0;
        } else if (read32(page + 14) != serial) return 0;
        if (page[5] & 4) {
            info->samples = read64(page + 6);
            eos = 1;
            break;
        }
        offset += header + payload;
    }
    return eos && info->samples != UINT64_MAX;
}

int ogg_read_stream_info(const uint8_t *data, size_t length,
                         uint32_t *rate, uint32_t *samples,
                         uint32_t *channels) {
    ogg_vorbis_info info;
    if (!rate || !samples || !channels ||
        !ogg_read_vorbis_info(data, length, &info) ||
        info.samples > UINT32_MAX) return 0;
    *rate = info.sample_rate;
    *samples = (uint32_t)info.samples;
    *channels = info.channels;
    return 1;
}
