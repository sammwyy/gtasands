#include "dsnd.h"

#include "adpcm.h"
#include "resample.h"

#include <math.h>
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

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)read16(p) | (uint32_t)read16(p + 2) << 16;
}

int dsnd_encode(const int16_t *pcm, size_t count, const dsnd_options *p,
                uint8_t **output, size_t *length, uint8_t **checkpoints,
                size_t *checkpoint_length) {
    if (!p || !output || !length || !checkpoints || !checkpoint_length ||
        (!pcm && count) || !p->rate || !p->sample_rate ||
        p->rate > 192000 || p->sample_rate > UINT16_MAX ||
        p->format > 2 || !isfinite(p->volume) || !isfinite(p->max_seconds) ||
        p->max_seconds < 0 || count > UINT32_MAX) return 0;
    *output = NULL;
    *length = 0;
    *checkpoints = NULL;
    *checkpoint_length = 0;
    int16_t *samples = count ? malloc(count * sizeof *samples) : NULL;
    if (count && !samples) return 0;
    for (size_t i = 0; i < count; ++i) {
        double scaled = (double)pcm[i] * p->volume;
        if (scaled < -32768) scaled = -32768;
        if (scaled > 32767) scaled = 32767;
        samples[i] = (int16_t)scaled;
    }
    uint32_t rate = p->rate;
    size_t n = count;
    if (p->sample_rate < p->rate && count) {
        int16_t *downsampled = NULL;
        size_t out_count = 0;
        if (!resample_pcm16(samples, count, p->rate, p->sample_rate,
                            &downsampled, &out_count)) {
            free(samples);
            return 0;
        }
        free(samples);
        samples = downsampled;
        n = out_count;
        rate = p->sample_rate;
    }
    int32_t loop = p->loop;
    if (loop >= 0) loop = (int32_t)((double)loop * rate / p->rate);
    if (p->max_seconds) {
        size_t max_count = (size_t)(p->max_seconds * rate);
        if (n > max_count) n = max_count;
    }
    if (loop >= (int64_t)n) loop = -1;
    uint8_t *raw = NULL, *saved = NULL;
    size_t raw_length = 0, saved_length = 0;
    int ok = 1;
    if (p->format == 2)
        ok = adpcm_encode(samples, n, p->checkpoints, &raw, &raw_length,
                          &saved, &saved_length);
    else {
        raw_length = n * (p->format == 1 ? 2 : 1);
        raw = raw_length ? malloc(raw_length) : NULL;
        if (raw_length && !raw) ok = 0;
        for (size_t i = 0; i < n && ok; ++i)
            if (p->format == 1) put16(raw + i * 2, (uint16_t)samples[i]);
            else raw[i] = (uint8_t)((uint16_t)samples[i] >> 8);
    }
    free(samples);
    size_t padded = (raw_length + 3) & ~(size_t)3;
    if (padded > SIZE_MAX - 24 || saved_length > SIZE_MAX - 24 - padded ||
        raw_length > UINT32_MAX || n > UINT32_MAX) ok = 0;
    uint8_t *result = ok ? calloc(24 + padded + saved_length, 1) : NULL;
    if (!result) ok = 0;
    if (ok) {
        memcpy(result, "DSND", 4);
        result[4] = p->format;
        put16(result + 6, (uint16_t)rate);
        put32(result + 8, (uint32_t)n);
        put32(result + 12, (uint32_t)loop);
        put16(result + 16, (uint16_t)p->headroom);
        put32(result + 20, (uint32_t)raw_length);
        if (raw_length) memcpy(result + 24, raw, raw_length);
        if (saved_length) memcpy(result + 24 + padded, saved, saved_length);
        *output = result;
        *length = 24 + padded + saved_length;
        *checkpoints = saved;
        *checkpoint_length = saved_length;
    } else {
        free(result);
        free(saved);
    }
    free(raw);
    return ok;
}

int dsnd_encode_simple(const int16_t *pcm, size_t count,
                       uint32_t rate, uint32_t sample_rate,
                       uint8_t format, int checkpoints,
                       double volume, double max_seconds,
                       int32_t loop, int16_t headroom,
                       uint8_t **output, size_t *length) {
    dsnd_options options = {rate, sample_rate, format, !!checkpoints,
                            volume, max_seconds, loop, headroom};
    uint8_t *saved = NULL;
    size_t saved_length = 0;
    int ok = dsnd_encode(pcm, count, &options, output, length,
                         &saved, &saved_length);
    free(saved);
    return ok;
}

void audio_decrypt_parts(const uint8_t *source, size_t length,
                         uint32_t offset_low, uint32_t offset_high,
                         uint8_t *output) {
    audio_decrypt(source, length, (uint64_t)offset_high << 32 | offset_low,
                  output);
}

int dsbk_convert(const uint8_t *raw, size_t length, const dsnd_options *options,
                 uint8_t **output, size_t *output_length, uint16_t *sounds) {
    if (!raw || !options || !output || !output_length || !sounds ||
        length < 0x12c4) return 0;
    int32_t n = (int16_t)read16(raw);
    if (n < 0) n = 1;
    if ((size_t)n > (0x12c4u - 4) / 12) return 0;
    uint8_t **blobs = n ? calloc((size_t)n, sizeof *blobs) : NULL;
    size_t *sizes = n ? calloc((size_t)n, sizeof *sizes) : NULL;
    int ok = !n || (blobs && sizes);
    size_t data_len = length - 0x12c4u, total = 8 + (size_t)n * 4;
    dsnd_options opts = *options;
    opts.checkpoints = 0;
    for (int32_t i = 0; i < n && ok; ++i) {
        const uint8_t *entry = raw + 4 + i * 12;
        size_t off = read32(entry);
        size_t end = i + 1 < n ? read32(entry + 12) : data_len;
        if (end > data_len) end = data_len;
        size_t count = off < data_len && end > off ? (end - off) / 2 : 0;
        int16_t *pcm = count ? malloc(count * sizeof *pcm) : NULL;
        if (count && !pcm) {
            ok = 0;
            break;
        }
        for (size_t j = 0; j < count; ++j)
            pcm[j] = (int16_t)read16(raw + 0x12c4u + off + j * 2);
        opts.rate = read16(entry + 8);
        if (!opts.rate) opts.rate = 22050;
        opts.loop = (int32_t)read32(entry + 4);
        opts.headroom = (int16_t)read16(entry + 10);
        uint8_t *checkpoints = NULL;
        size_t checkpoint_length = 0;
        ok = dsnd_encode(pcm, count, &opts, &blobs[i], &sizes[i],
                         &checkpoints, &checkpoint_length);
        free(checkpoints);
        free(pcm);
        if (sizes[i] > SIZE_MAX - total) ok = 0;
        else total += sizes[i];
    }
    if (total > UINT32_MAX) ok = 0;
    uint8_t *result = ok ? malloc(total) : NULL;
    if (!result) ok = 0;
    if (ok) {
        memcpy(result, "DSBK", 4);
        put16(result + 4, 1);
        put16(result + 6, (uint16_t)n);
        size_t cursor = 8 + (size_t)n * 4;
        for (int32_t i = 0; i < n; ++i) {
            put32(result + 8 + (size_t)i * 4, (uint32_t)cursor);
            memcpy(result + cursor, blobs[i], sizes[i]);
            cursor += sizes[i];
        }
        *output = result;
        *output_length = total;
        *sounds = (uint16_t)n;
    } else free(result);
    for (int32_t i = 0; i < n; ++i) free(blobs ? blobs[i] : NULL);
    free(blobs);
    free(sizes);
    return ok;
}

static int compare_entries(const void *left, const void *right) {
    const dsst_entry *a = left, *b = right;
    if (a->track != b->track) return a->track < b->track ? -1 : 1;
    size_t length = a->length < b->length ? a->length : b->length;
    int difference = length ? memcmp(a->data, b->data, length) : 0;
    if (difference) return difference;
    return a->length < b->length ? -1 : a->length > b->length ? 1 : 0;
}

int dsst_pack(const dsst_entry *entries, size_t count, int seekable,
              uint8_t **output, size_t *output_length) {
    if ((!entries && count) || !output || !output_length || count > UINT16_MAX)
        return 0;
    size_t unit = seekable ? 20 : 12;
    size_t total = 8 + count * unit;
    for (size_t i = 0; i < count; ++i) {
        if ((!entries[i].data && entries[i].length) ||
            entries[i].length > UINT32_MAX ||
            entries[i].length > UINT32_MAX - total) return 0;
        total += entries[i].length;
        if (seekable) {
            if (entries[i].length < 24) return 0;
            size_t raw_size = read32(entries[i].data + 20);
            size_t seek_at = 24 + ((raw_size + 3) & ~(size_t)3);
            if (seek_at > entries[i].length) return 0;
        }
    }
    dsst_entry *ordered = count ? malloc(count * sizeof *ordered) : NULL;
    uint8_t *result = malloc(total);
    if (!result || (count && !ordered)) {
        free(ordered); free(result);
        return 0;
    }
    if (count) {
        memcpy(ordered, entries, count * sizeof *ordered);
        qsort(ordered, count, sizeof *ordered, compare_entries);
    }
    memcpy(result, "DSST", 4);
    put16(result + 4, seekable ? 2 : 1);
    put16(result + 6, (uint16_t)count);
    size_t at = 8 + count * unit;
    for (size_t i = 0; i < count; ++i) {
        const dsst_entry *e = &ordered[i];
        uint8_t *entry = result + 8 + i * unit;
        put32(entry, e->track);
        put32(entry + 4, (uint32_t)at);
        put32(entry + 8, (uint32_t)e->length);
        if (seekable) {
            size_t raw_size = read32(e->data + 20);
            size_t seek_at = 24 + ((raw_size + 3) & ~(size_t)3);
            put32(entry + 12, (uint32_t)(at + seek_at));
            put32(entry + 16, (uint32_t)((e->length - seek_at) / 4));
        }
        if (e->length) memcpy(result + at, e->data, e->length);
        at += e->length;
    }
    free(ordered);
    *output = result;
    *output_length = total;
    return 1;
}

static const uint8_t audio_key[16] = {
    0xea, 0x3a, 0xc4, 0xa1, 0x9a, 0xa8, 0x14, 0xf3,
    0x48, 0xb0, 0xd7, 0x23, 0x9d, 0xe8, 0xff, 0xf1
};

void audio_decrypt(const uint8_t *source, size_t length, uint64_t offset, uint8_t *output) {
    for (size_t i = 0; i < length; ++i)
        output[i] = source[i] ^ audio_key[(offset + i) & 15];
}

int audio_aligned_offset(const uint8_t *encrypted, size_t length, uint64_t start,
                         uint64_t expected, uint64_t *result) {
    if (!encrypted || !result) return 0;
    const size_t header = 0x1f84;
    if (length < header + 4 || length > SIZE_MAX - 16) return 0;
    uint8_t *decrypted = malloc(length);
    if (!decrypted) return 0;
    audio_decrypt(encrypted, length, start, decrypted);
    int found = 0;
    uint64_t best_distance = UINT64_MAX;
    for (size_t i = 0; i <= length - header - 4; ++i) {
        int signature = 1;
        for (size_t j = 0; j < 16; ++j)
            if (decrypted[i + j] != (j % 8 < 4 ? 0xff : 0)) {
                signature = 0;
                break;
            }
        if (!signature || memcmp(decrypted + i + header, "OggS", 4)) continue;
        uint64_t candidate = start + i;
        uint64_t distance = candidate > expected ?
                            candidate - expected : expected - candidate;
        if (!found || distance < best_distance) {
            found = 1;
            best_distance = distance;
            *result = candidate;
        }
        i += 7;
    }
    free(decrypted);
    return found;
}
