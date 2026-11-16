#include "ifp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} buffer;

typedef struct {
    int32_t value[8];
} frame;

typedef struct {
    char name[64];
    int32_t bone;
    int translated;
    size_t count;
    frame *frames;
} sequence;

typedef struct {
    char name[64];
    size_t count;
    sequence *sequences;
} animation;

typedef struct {
    char block[64];
    size_t count;
    animation *animations;
} package;

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float f32(const uint8_t *p) {
    uint32_t bits = u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int32_t round_even(double value) {
    double integral;
    double fraction = modf(value, &integral);
    int64_t result = (int64_t)integral;
    if (fraction > 0.5 || (fraction == 0.5 && result % 2)) ++result;
    if (fraction < -0.5 || (fraction == -0.5 && result % 2)) --result;
    return (int32_t)result;
}

static int32_t clamp16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return value;
}

static int32_t rounded16(double value) {
    if (value < -32768.0) return -32768;
    if (value > 32767.0) return 32767;
    return round_even(value);
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static int append(buffer *out, const void *bytes, size_t n) {
    if (n > SIZE_MAX - out->length) return 0;
    size_t target = out->length + n;
    if (target > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 1024;
        while (capacity < target) {
            if (capacity > SIZE_MAX / 2) {
                capacity = target;
                break;
            }
            capacity *= 2;
        }
        uint8_t *next = realloc(out->data, capacity);
        if (!next) return 0;
        out->data = next;
        out->capacity = capacity;
    }
    if (n) memcpy(out->data + out->length, bytes, n);
    out->length = target;
    return 1;
}

static int pad4(buffer *out) {
    const uint8_t zeros[3] = {0};
    return append(out, zeros, (4 - out->length % 4) % 4);
}

static void copy_name(char *dest, size_t capacity, const uint8_t *src, size_t length) {
    size_t i = 0;
    while (i < length && i + 1 < capacity && src[i]) {
        dest[i] = (char)src[i];
        ++i;
    }
    dest[i] = 0;
}

static int section(const uint8_t *source, size_t limit, size_t *position,
                   const char *tag, const uint8_t **content, size_t *length) {
    size_t pos = *position;
    if (pos > limit || limit - pos < 8 || memcmp(source + pos, tag, 4)) return 0;
    size_t size = u32(source + pos + 4);
    if (size > limit - pos - 8 || size > SIZE_MAX - 3) return 0;
    *content = source + pos + 8;
    *length = size;
    *position = pos + 8 + ((size + 3) & ~(size_t)3);
    return *position <= limit + 3;
}

static void release(package *p) {
    for (size_t i = 0; i < p->count; ++i) {
        for (size_t j = 0; j < p->animations[i].count; ++j)
            free(p->animations[i].sequences[j].frames);
        free(p->animations[i].sequences);
    }
    free(p->animations);
}

static int parse_anp3(const uint8_t *source, size_t size, package *p) {
    if (size < 36 || memcmp(source, "ANP3", 4)) return 0;
    copy_name(p->block, sizeof p->block, source + 8, 24);
    uint32_t n = u32(source + 32);
    if (n > size / 36) return 0;
    p->animations = n ? calloc(n, sizeof *p->animations) : NULL;
    if (n && !p->animations) return 0;
    p->count = n;
    size_t pos = 36;
    for (size_t i = 0; i < n; ++i) {
        if (pos > size || size - pos < 36) return 0;
        animation *a = &p->animations[i];
        copy_name(a->name, sizeof a->name, source + pos, 24);
        uint32_t nseq = u32(source + pos + 24);
        pos += 36;
        if (nseq > (size - pos) / 36) return 0;
        a->sequences = nseq ? calloc(nseq, sizeof *a->sequences) : NULL;
        if (nseq && !a->sequences) return 0;
        a->count = nseq;
        for (size_t j = 0; j < nseq; ++j) {
            if (size - pos < 36) return 0;
            sequence *s = &a->sequences[j];
            copy_name(s->name, sizeof s->name, source + pos, 24);
            uint32_t type = u32(source + pos + 24), nframes = u32(source + pos + 28);
            s->bone = (int32_t)u32(source + pos + 32);
            s->translated = type == 2 || type == 4;
            pos += 36;
            size_t values = s->translated ? 8 : 5;
            size_t stride = values * (type == 3 || type == 4 ? 2 : 4);
            if (nframes > (size - pos) / stride) return 0;
            s->frames = nframes ? calloc(nframes, sizeof *s->frames) : NULL;
            if (nframes && !s->frames) return 0;
            s->count = nframes;
            for (size_t k = 0; k < nframes; ++k) {
                frame *f = &s->frames[k];
                for (size_t v = 0; v < values; ++v) {
                    const uint8_t *ptr = source + pos + k * stride + v * (stride / values);
                    if (type == 3 || type == 4) f->value[v] = (int16_t)u16(ptr);
                    else {
                        double value = f32(ptr);
                        if (!isfinite(value)) return 0;
                        int scale = v < 4 ? 4096 : v == 4 ? 60 : 1024;
                        value *= scale;
                        f->value[v] = rounded16(value);
                    }
                }
            }
            pos += (size_t)nframes * stride;
        }
    }
    return 1;
}

static int parse_anpk(const uint8_t *data, size_t size, package *p) {
    const uint8_t *info;
    size_t info_size, pos = 8;
    if (!section(data, size, &pos, "INFO", &info, &info_size) || info_size < 4)
        return 0;
    uint32_t count = u32(info);
    if (count > size / 16) return 0;
    copy_name(p->block, sizeof p->block, info + 4, info_size - 4);
    p->animations = count ? calloc(count, sizeof *p->animations) : NULL;
    if (count && !p->animations) return 0;
    p->count = count;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *label;
        size_t label_size;
        if (!section(data, size, &pos, "NAME", &label, &label_size) ||
            pos > size || size - pos < 8 || memcmp(data + pos, "DGAN", 4))
            return 0;
        size_t chunk_length = u32(data + pos + 4);
        if (chunk_length > size - pos - 8 || chunk_length > SIZE_MAX - 3) return 0;
        size_t end = pos + 8 + ((chunk_length + 3) & ~(size_t)3);
        if (end > size) return 0;
        animation *a = &p->animations[i];
        copy_name(a->name, sizeof a->name, label, label_size);
        size_t inner = pos + 8;
        const uint8_t *details;
        size_t details_size;
        if (!section(data, end, &inner, "INFO", &details, &details_size) ||
            details_size < 4) return 0;
        uint32_t nseq = u32(details);
        if (nseq > (end - inner) / 8) return 0;
        a->sequences = nseq ? calloc(nseq, sizeof *a->sequences) : NULL;
        if (nseq && !a->sequences) return 0;
        a->count = nseq;
        for (size_t j = 0; j < nseq; ++j) {
            if (inner > end || end - inner < 8 || memcmp(data + inner, "CPAN", 4))
                return 0;
            size_t cpan_len = u32(data + inner + 4);
            if (cpan_len > end - inner - 8) return 0;
            inner += 8;
            const uint8_t *desc;
            size_t desc_size;
            if (!section(data, end, &inner, "ANIM", &desc, &desc_size) ||
                desc_size < 32) return 0;
            sequence *s = &a->sequences[j];
            copy_name(s->name, sizeof s->name, desc, 28);
            uint32_t nframes = u32(desc + 28);
            s->bone = desc_size >= 44 ? (int32_t)u32(desc + 40) : -1;
            if (nframes) {
                if (inner > end || end - inner < 8) return 0;
                size_t stride = !memcmp(data + inner, "KR00", 4) ? 20 :
                                !memcmp(data + inner, "KRT0", 4) ? 32 :
                                !memcmp(data + inner, "KRTS", 4) ? 44 : 0;
                if (!stride) return 0;
                char tag[5] = {0};
                memcpy(tag, data + inner, 4);
                const uint8_t *payload;
                size_t payload_size;
                if (!section(data, end, &inner, tag, &payload, &payload_size) ||
                    nframes > payload_size / stride) return 0;
                s->translated = stride != 20;
                s->frames = calloc(nframes, sizeof *s->frames);
                if (!s->frames) return 0;
                s->count = nframes;
                for (size_t k = 0; k < nframes; ++k) {
                    const uint8_t *raw = payload + k * stride;
                    frame *f = &s->frames[k];
                    for (size_t v = 0; v < 4; ++v) {
                        double value = f32(raw + v * 4);
                        if (!isfinite(value)) value = 0;
                        value *= v == 3 ? 4096 : -4096;
                        f->value[v] = rounded16(value);
                    }
                    double time = f32(raw + stride - 4);
                    if (!isfinite(time) || time < 0)
                        return 0;
                    f->value[4] = rounded16(time * 60);
                    if (s->translated)
                        for (size_t v = 0; v < 3; ++v) {
                            double value = f32(raw + 16 + v * 4);
                            f->value[v + 5] = isfinite(value) && fabs(value) <= 512 ?
                                              round_even(value * 1024) : 0;
                        }
                }
            }
        }
        pos = end;
    }
    return 1;
}

static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

static int character_class(const char *pattern, unsigned char character,
                           const char **next) {
    const char *start = pattern + 1;
    int inverted = *start == '!';
    if (inverted) ++start;
    const char *search = start;
    if (*search == ']') ++search;
    const char *end = strchr(search, ']');
    if (!end) {
        *next = pattern + 1;
        return character == '[';
    }
    int found = 0;
    character = lower(character);
    for (const char *p = start; p < end; ++p) {
        unsigned char a = lower((unsigned char)*p);
        if (p + 2 < end && p[1] == '-') {
            unsigned char b = lower((unsigned char)p[2]);
            found |= a <= character && character <= b;
            p += 2;
        } else found |= a == character;
    }
    *next = end + 1;
    return inverted ? !found : found;
}

static int matches(const char *pattern, const char *name) {
    if (!*pattern) return !*name;
    if (*pattern == '*') {
        do ++pattern; while (*pattern == '*');
        if (!*pattern) return 1;
        for (; *name; ++name)
            if (matches(pattern, name)) return 1;
        return matches(pattern, name);
    }
    if (!*name) return 0;
    if (*pattern == '[') {
        const char *next;
        return character_class(pattern, (unsigned char)*name, &next) &&
               matches(next, name + 1);
    }
    return (*pattern == '?' || lower((unsigned char)*pattern) == lower((unsigned char)*name)) &&
           matches(pattern + 1, name + 1);
}

static int selected(const animation *a, const ifp_options *opts) {
    for (size_t i = 0; i < opts->remove_count; ++i)
        if (matches(opts->remove[i], a->name)) return 0;
    return 1;
}

static int rigid(const animation *a, const ifp_options *opts) {
    for (size_t i = 0; i < opts->rigid_count; ++i)
        {
            const char *pattern = opts->rigid_clips[i], *name = a->name;
            while (*name && *pattern == lower((unsigned char)*name)) {
                ++pattern;
                ++name;
            }
            if (!*pattern && !*name) return 1;
        }
    return 0;
}

static uint32_t chosen_index(uint32_t i, uint32_t count, uint32_t step) {
    if (step <= 1 || count <= 2) return i;
    uint32_t regular = (count - 1) / step + 1;
    if (i == regular && (count - 1) % step) return count - 1;
    return i * step;
}

static uint32_t selected_frame(uint32_t i, uint32_t n,
                               uint32_t keep_every, uint32_t max_frames) {
    uint32_t count = keep_every <= 1 || n <= 2 ? n :
                     (n - 1) / keep_every + 1 + !!((n - 1) % keep_every);
    if (max_frames && count > max_frames) {
        if (max_frames == 1) return 0;
        uint64_t numerator = (uint64_t)i * (count - 1);
        uint32_t divisor = max_frames - 1;
        uint64_t result = numerator / divisor, remainder = numerator % divisor;
        if (remainder * 2 > divisor || (remainder * 2 == divisor && (result & 1)))
            ++result;
        i = (uint32_t)result;
    }
    return chosen_index(i, n, keep_every);
}

static int write_seq(buffer *out, const sequence *seq, const animation *anim,
                     size_t seq_index, const ifp_options *options, int *wide,
                     uint32_t *keys_in, uint32_t *keys_out) {
    uint32_t step = options->keep_every ? options->keep_every : 1;
    if (seq->count > UINT16_MAX || seq->count > UINT32_MAX) return 0;
    uint32_t count = (uint32_t)seq->count;
    uint32_t kept = step <= 1 || count <= 2 ? count :
                    (count - 1) / step + 1 + !!((count - 1) % step);
    if (options->max_frames && kept > options->max_frames)
        kept = options->max_frames;
    if (kept > UINT16_MAX || *keys_in > UINT32_MAX - count ||
        *keys_out > UINT32_MAX - kept) return 0;
    *keys_in += count;
    *keys_out += kept;
    int translated = seq->translated && !(options->drop_translation && seq_index > 0);
    uint8_t shift = 0;
    if (translated) {
        uint32_t maxpos = 0;
        for (uint32_t i = 0; i < kept; ++i) {
            uint32_t index = selected_frame(i, count, step, options->max_frames);
            for (unsigned j = 5; j < 8; ++j) {
                int64_t raw = seq->frames[index].value[j];
                uint64_t absolute = (uint64_t)(raw < 0 ? -raw : raw);
                if (absolute > maxpos) maxpos = (uint32_t)absolute;
            }
        }
        while (maxpos > (32767u << shift) && shift < 12) ++shift;
        if (shift == 12) return 0;
        if (shift) *wide = 1;
    }
    uint8_t header[32] = {0};
    size_t name_len = strlen(seq->name);
    if (name_len > 23) name_len = 23;
    memcpy(header, seq->name, name_len);
    int32_t bone = seq->bone;
    if (bone < 0 && rigid(anim, options)) bone = (int32_t)seq_index;
    if (bone < INT16_MIN || bone > INT16_MAX) return 0;
    put16(header + 24, (uint16_t)bone);
    header[26] = (uint8_t)translated;
    header[27] = shift;
    put16(header + 28, (uint16_t)kept);
    if (!append(out, header, sizeof header)) return 0;
    for (uint32_t i = 0; i < kept; ++i) {
        const frame *f = &seq->frames[selected_frame(i, count, step, options->max_frames)];
        for (unsigned j = 0; j < 5; ++j) {
            uint8_t bytes[2];
            put16(bytes, (uint16_t)f->value[j]);
            if (!append(out, bytes, sizeof bytes)) return 0;
        }
        if (translated)
            for (unsigned j = 5; j < 8; ++j) {
                int32_t value = clamp16(round_even((double)f->value[j] / (1u << shift)));
                uint8_t bytes[2];
                put16(bytes, (uint16_t)value);
                if (!append(out, bytes, sizeof bytes)) return 0;
            }
    }
    return pad4(out);
}

int ifp_convert(const uint8_t *source, size_t size, const ifp_options *options,
                uint8_t **output, size_t *length, uint32_t *keys_in, uint32_t *keys_out) {
    if (!source || size < 8 || !options || !output || !length || !keys_in || !keys_out ||
        (options->remove_count && !options->remove) ||
        (options->rigid_count && !options->rigid_clips))
        return 0;
    package p = {0};
    int ok = !memcmp(source, "ANP3", 4) ? parse_anp3(source, size, &p) :
             !memcmp(source, "ANPK", 4) ? parse_anpk(source, size, &p) : 0;
    buffer out = {0};
    uint32_t input_keys = 0, output_keys = 0;
    if (ok && p.count <= UINT16_MAX) {
        uint32_t anims = 0;
        for (size_t i = 0; i < p.count; ++i)
            if (selected(&p.animations[i], options)) ++anims;
        uint8_t header[32] = {'D', 'A', 'N', 'M', 2, 0, 0, 0};
        put16(header + 6, (uint16_t)anims);
        size_t name_len = strlen(p.block);
        if (name_len > 23) name_len = 23;
        memcpy(header + 8, p.block, name_len);
        ok = append(&out, header, sizeof header);
        for (uint32_t i = 0; i < anims && ok; ++i) {
            uint8_t empty[32] = {0};
            ok = append(&out, empty, sizeof empty);
        }
        size_t dir = 32;
        int wide = 0;
        for (size_t i = 0; i < p.count && ok; ++i) {
            const animation *anim = &p.animations[i];
            if (!selected(anim, options)) continue;
            if (anim->count > UINT16_MAX || out.length > UINT32_MAX) {
                ok = 0;
                break;
            }
            size_t begin = out.length;
            uint8_t record[28] = {0};
            name_len = strlen(anim->name);
            if (name_len > 23) name_len = 23;
            memcpy(record, anim->name, name_len);
            put16(record + 24, (uint16_t)anim->count);
            ok = append(&out, record, sizeof record);
            for (size_t j = 0; j < anim->count && ok; ++j)
                ok = write_seq(&out, &anim->sequences[j], anim, j, options,
                               &wide, &input_keys, &output_keys);
            if (!ok || out.length - begin > UINT32_MAX) break;
            memcpy(out.data + dir, record, 24);
            put32(out.data + dir + 24, (uint32_t)begin);
            put32(out.data + dir + 28, (uint32_t)(out.length - begin));
            dir += 32;
        }
        if (ok && wide) put16(out.data + 4, 3);
    } else ok = 0;
    if (ok) {
        *output = out.data;
        *length = out.length;
        *keys_in = input_keys;
        *keys_out = output_keys;
    } else free(out.data);
    release(&p);
    return ok;
}
