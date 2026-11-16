#include "vehparse.h"
#include "vehpack.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *strip(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
    return text;
}

static int case_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return !*a && !*b;
}

static int copy_case(char *out, size_t capacity, const char *input, int upper) {
    size_t n = strlen(input);
    if (n >= capacity) return 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = (char)(upper ? toupper((unsigned char)input[i]) :
                                tolower((unsigned char)input[i]));
    out[n] = 0;
    return 1;
}

static int real(const char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*end;
}

static int integer(const char *text, int base, int32_t *value) {
    char *end;
    long parsed = strtol(text, &end, base);
    if (end == text || *end || parsed < INT32_MIN || parsed > INT32_MAX) return 0;
    *value = (int32_t)parsed;
    return 1;
}

static int unsigned_value(const char *text, uint32_t *value) {
    char *end;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (end == text || *end || parsed > UINT32_MAX) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

int vehparse_ide(const char *text, size_t length, vehparse_ide_entry **entries,
                 size_t *count) {
    if (!text || !entries || !count || length == SIZE_MAX) return 0;
    *entries = NULL;
    *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    vehparse_ide_entry *result = NULL;
    size_t used = 0, capacity = 0;
    const char *types[] = {"car", "mtruck", "quad", "heli", "plane", "boat",
                           "train", "f_heli", "f_plane", "bike", "bmx", "trailer"};
    int cars = 0, ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = strip(line);
        if (*start && !strchr(start, ',')) cars = case_equal(start, "cars");
        else if (cars && *start) {
            char *tokens[64];
            size_t n = 0;
            for (char *p = start; *p;) {
                while (*p && (*p == ',' || isspace((unsigned char)*p))) ++p;
                if (!*p) break;
                if (n == 64) break;
                tokens[n++] = p;
                while (*p && *p != ',' && !isspace((unsigned char)*p)) ++p;
                if (*p) *p++ = 0;
            }
            if (n >= 11) {
                vehparse_ide_entry v = {0};
                int32_t id;
                uint32_t flags;
                int valid = integer(tokens[0], 10, &id) &&
                            unsigned_value(tokens[9], &flags) &&
                            copy_case(v.model, sizeof v.model, tokens[1], 0) &&
                            copy_case(v.txd, sizeof v.txd, tokens[2], 0) &&
                            copy_case(v.handling, sizeof v.handling, tokens[4], 1) &&
                            strlen(tokens[5]) < sizeof v.gxt &&
                            copy_case(v.anims, sizeof v.anims, tokens[6], 0);
                if (valid) {
                    memcpy(v.gxt, tokens[5], strlen(tokens[5]) + 1);
                    v.id = id;
                    v.flags = (uint32_t)flags;
                    v.type = UINT8_MAX;
                    for (uint8_t i = 0; i < 12; ++i)
                        if (case_equal(tokens[3], types[i])) {
                            v.type = i;
                            break;
                        }
                    v.wheel_front = v.wheel_rear = 0.7;
                    if (v.type != 5 && n >= 14) {
                        valid = real(tokens[12], &v.wheel_front) &&
                                real(tokens[13], &v.wheel_rear);
                        if (valid && (v.type == 9 || v.type == 10))
                            valid = real(tokens[11], &v.steer);
                    }
                }
                if (valid) {
                    if (used == capacity) {
                        size_t next_size = capacity ? capacity * 2 : 256;
                        if (next_size > SIZE_MAX / sizeof *result) {
                            ok = 0;
                            break;
                        }
                        vehparse_ide_entry *next = realloc(result, next_size * sizeof *next);
                        if (!next) {
                            ok = 0;
                            break;
                        }
                        capacity = next_size;
                        result = next;
                    }
                    result[used++] = v;
                }
            }
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *entries = result;
        *count = used;
    } else free(result);
    return ok;
}

static int float_value(const char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && !*end;
}

static int flags_value(const char *text, uint64_t *value) {
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 16);
    if (end == text || *end || errno == ERANGE) return 0;
    *value = (uint64_t)parsed;
    return 1;
}

int vehparse_handling(const char *text, size_t length,
                      vehparse_handling_entry **entries, size_t *count) {
    if (!text || !entries || !count || length == SIZE_MAX) return 0;
    *entries = NULL;
    *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    vehparse_handling_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, ';');
        if (comment) *comment = 0;
        char *start = strip(line);
        if (*start && !strchr("!$%^", *start)) {
            char *tokens[64];
            size_t n = 0;
            for (char *p = start; *p;) {
                while (isspace((unsigned char)*p)) ++p;
                if (!*p || n == 64) break;
                tokens[n++] = p;
                while (*p && !isspace((unsigned char)*p)) ++p;
                if (*p) *p++ = 0;
            }
            if (n >= 36) {
                vehparse_handling_entry v = {0};
                int valid = copy_case(v.name, sizeof v.name, tokens[0], 1) &&
                            float_value(tokens[1], &v.mass) &&
                            float_value(tokens[2], &v.turn_mass) &&
                            float_value(tokens[3], &v.drag);
                for (size_t j = 0; j < 3 && valid; ++j)
                    valid = float_value(tokens[4 + j], &v.com[j]);
                if (valid) valid = integer(tokens[7], 10, &v.submerged) &&
                                   float_value(tokens[8], &v.traction_mult) &&
                                   float_value(tokens[9], &v.traction_loss) &&
                                   float_value(tokens[10], &v.traction_bias) &&
                                   integer(tokens[11], 10, &v.gears) &&
                                   float_value(tokens[12], &v.max_vel) &&
                                   float_value(tokens[13], &v.engine_accel) &&
                                   float_value(tokens[14], &v.engine_inertia);
                if (valid) {
                    v.drive = (uint8_t)toupper((unsigned char)tokens[15][0]);
                    v.engine = (uint8_t)toupper((unsigned char)tokens[16][0]);
                    valid = float_value(tokens[17], &v.brake_decel) &&
                            float_value(tokens[18], &v.brake_bias) &&
                            integer(tokens[19], 10, &v.abs) &&
                            float_value(tokens[20], &v.steering_lock);
                }
                for (size_t j = 0; j < 7 && valid; ++j)
                    valid = float_value(tokens[21 + j], &v.suspension[j]);
                if (valid) {
                    double value;
                    valid = float_value(tokens[28], &v.seat_offset) &&
                            float_value(tokens[29], &v.damage_mult) &&
                            float_value(tokens[30], &value) &&
                            flags_value(tokens[31], &v.model_flags) &&
                            flags_value(tokens[32], &v.handling_flags) &&
                            integer(tokens[33], 10, &v.front_lights) &&
                            integer(tokens[34], 10, &v.rear_lights) &&
                            integer(tokens[35], 10, &v.anim_group);
                    if (valid && value >= INT32_MIN && value <= INT32_MAX)
                        v.value = (int32_t)value;
                    else valid = 0;
                }
                if (valid) {
                    v.engine_accel *= 0.4;
                    vehpack_handling units = {v.mass, v.drag, v.engine_accel,
                                              v.max_vel, v.brake_decel,
                                              v.damage_mult, 0, 0,
                                              (uint32_t)v.handling_flags, v.drive};
                    valid = vehpack_convert_handling(&units);
                    if (valid) {
                        v.engine_accel = units.engine_accel;
                        v.max_vel = units.max_vel;
                        v.brake_decel = units.brake_decel;
                        v.damage_mult = units.damage_mult;
                        v.max_flat = units.max_flat;
                        v.max_reverse = units.max_reverse;
                    }
                }
                if (valid) {
                    if (used == capacity) {
                        size_t next_size = capacity ? capacity * 2 : 256;
                        if (next_size > SIZE_MAX / sizeof *result) {
                            ok = 0;
                            break;
                        }
                        vehparse_handling_entry *next = realloc(result, next_size * sizeof *next);
                        if (!next) {
                            ok = 0;
                            break;
                        }
                        capacity = next_size;
                        result = next;
                    }
                    result[used++] = v;
                }
            }
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *entries = result;
        *count = used;
    } else free(result);
    return ok;
}

int vehparse_water(const char *text, size_t length,
                   vehparse_water_entry **entries, size_t *count) {
    if (!text || !entries || !count || length == SIZE_MAX) return 0;
    *entries = NULL;
    *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    vehparse_water_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *tokens[32];
        size_t n = 0;
        for (char *p = line; *p;) {
            while (isspace((unsigned char)*p)) ++p;
            if (!*p || n == 32) break;
            tokens[n++] = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = 0;
        }
        if (n == 22 || n == 29) {
            vehparse_water_entry value = {0};
            value.count = n == 29 ? 4 : 3;
            double raw[28];
            int valid = 1;
            for (size_t i = 0; i < n - 1 && valid; ++i)
                valid = float_value(tokens[i], &raw[i]);
            if (valid) valid = integer(tokens[n - 1], 10, &value.flags);
            if (valid) {
                for (size_t i = 0; i < value.count; ++i)
                    value.vertices[i] = (vehpack_vec3){
                        raw[i * 7], raw[i * 7 + 1], raw[i * 7 + 2]};
                if (used == capacity) {
                    size_t next_size = capacity ? capacity * 2 : 256;
                    if (next_size > SIZE_MAX / sizeof *result) {
                        ok = 0;
                        break;
                    }
                    vehparse_water_entry *next = realloc(result, next_size * sizeof *next);
                    if (!next) {
                        ok = 0;
                        break;
                    }
                    result = next;
                    capacity = next_size;
                }
                result[used++] = value;
            }
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *entries = result;
        *count = used;
    } else free(result);
    return ok;
}

int vehparse_track(const char *text, size_t length,
                   vehpack_vec3 **nodes, size_t *count) {
    if (!text || !nodes || !count || length == SIZE_MAX) return 0;
    *nodes = NULL;
    *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    vehpack_vec3 *result = NULL;
    size_t used = 0, capacity = 0;
    int first = 1, ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        if (!first) {
            char *tokens[3];
            size_t n = 0;
            for (char *p = line; *p && n < 3;) {
                while (isspace((unsigned char)*p)) ++p;
                if (!*p) break;
                tokens[n++] = p;
                while (*p && !isspace((unsigned char)*p)) ++p;
                if (*p) *p++ = 0;
            }
            vehpack_vec3 value;
            if (n == 3 && float_value(tokens[0], &value.x) &&
                float_value(tokens[1], &value.y) &&
                float_value(tokens[2], &value.z)) {
                if (used == capacity) {
                    size_t next_size = capacity ? capacity * 2 : 256;
                    if (next_size > SIZE_MAX / sizeof *result) {
                        ok = 0;
                        break;
                    }
                    vehpack_vec3 *next = realloc(result, next_size * sizeof *next);
                    if (!next) {
                        ok = 0;
                        break;
                    }
                    result = next;
                    capacity = next_size;
                }
                result[used++] = value;
            }
        }
        first = 0;
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *nodes = result;
        *count = used;
    } else free(result);
    return ok;
}

int vehparse_special(const char *text, size_t length,
                     vehparse_special_entry **entries, size_t *count) {
    if (!text || !entries || !count || length == SIZE_MAX) return 0;
    *entries = NULL;
    *count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    vehparse_special_entry *result = NULL;
    size_t used = 0, capacity = 0;
    int ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, ';');
        if (comment) *comment = 0;
        char *start = strip(line);
        unsigned kind = *start == '!' ? 1 : *start == '%' ? 2 : *start == '$' ? 3 : 0;
        if (kind) {
            char *tokens[32];
            size_t n = 0;
            for (char *p = start + 1; *p;) {
                while (isspace((unsigned char)*p)) ++p;
                if (!*p || n == 32) break;
                tokens[n++] = p;
                while (*p && !isspace((unsigned char)*p)) ++p;
                if (*p) *p++ = 0;
            }
            size_t needed = kind == 1 ? 15 : kind == 2 ? 14 : 21;
            if (n >= needed + 1) {
                vehparse_special_entry value = {0};
                int valid = copy_case(value.name, sizeof value.name, tokens[0], 1);
                value.kind = (uint8_t)kind;
                for (size_t i = 0; i < needed && valid; ++i) {
                    char *after;
                    value.values[i] = strtod(tokens[i + 1], &after);
                    while (*after == 's' || *after == 'S') ++after;
                    valid = after != tokens[i + 1] && !*after;
                }
                if (valid) {
                    if (kind == 1) {
                        double radians = 3.141592653589793 / 180.0;
                        value.values[4] = sin(value.values[4] * radians);
                        value.values[5] *= radians;
                        value.values[10] = sin(value.values[10] * radians);
                        value.values[11] = sin(value.values[11] * radians);
                    }
                    if (used == capacity) {
                        size_t next_size = capacity ? capacity * 2 : 256;
                        if (next_size > SIZE_MAX / sizeof *result) {
                            ok = 0;
                            break;
                        }
                        vehparse_special_entry *next = realloc(result, next_size * sizeof *next);
                        if (!next) {
                            ok = 0;
                            break;
                        }
                        capacity = next_size;
                        result = next;
                    }
                    result[used++] = value;
                }
            }
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *entries = result;
        *count = used;
    } else free(result);
    return ok;
}

int vehparse_carcols(const char *text, size_t length,
                      vehparse_color **colors, size_t *color_count,
                      vehparse_car_color **cars, size_t *car_count) {
    if (!text || !colors || !color_count || !cars || !car_count ||
        length == SIZE_MAX) return 0;
    *colors = NULL; *color_count = 0;
    *cars = NULL; *car_count = 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = 0;
    vehparse_color *palette = NULL;
    vehparse_car_color *models = NULL;
    size_t npalette = 0, nmodels = 0, palette_capacity = 0, model_capacity = 0;
    int section = 0, ok = 1;
    for (char *line = copy; line < copy + length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *start = strip(line);
        if (case_equal(start, "col")) section = 1;
        else if (case_equal(start, "car")) section = 2;
        else if (case_equal(start, "car4")) section = 3;
        else if (case_equal(start, "end")) section = 0;
        else if (*start && section == 1) {
            int32_t values[3];
            size_t n = 0;
            for (char *p = start; *p && n < 3;) {
                while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '.')) ++p;
                if (!*p) break;
                char *token = p;
                while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '.') ++p;
                char delimiter = *p;
                *p = 0;
                if (!integer(token, 10, &values[n])) break;
                ++n;
                if (delimiter) ++p;
            }
            if (n == 3) {
                if (npalette == palette_capacity) {
                    size_t next_capacity = palette_capacity ? palette_capacity * 2 : 128;
                    if (next_capacity > SIZE_MAX / sizeof *palette) {
                        ok = 0;
                        break;
                    }
                    vehparse_color *next = realloc(palette, next_capacity * sizeof *next);
                    if (!next) {
                        ok = 0;
                        break;
                    }
                    palette = next;
                    palette_capacity = next_capacity;
                }
                palette[npalette++] = (vehparse_color){
                    values[0] > 255 ? 255 : values[0],
                    values[1] > 255 ? 255 : values[1],
                    values[2] > 255 ? 255 : values[2]};
            }
        } else if (*start && (section == 2 || section == 3)) {
            char *parts[128];
            size_t n = 0;
            for (char *p = start; *p && n < 128;) {
                parts[n++] = p;
                while (*p && *p != ',') ++p;
                if (*p) *p++ = 0;
            }
            vehparse_car_color value = {0};
            if (n && copy_case(value.name, sizeof value.name, strip(parts[0]), 0)) {
                int32_t numbers[32];
                size_t number_count = 0;
                for (size_t i = 1; i < n && number_count < 32; ++i) {
                    char *part = strip(parts[i]);
                    int digits = *part != 0;
                    for (char *p = part; *p; ++p)
                        if (!isdigit((unsigned char)*p)) digits = 0;
                    if (digits && integer(part, 10, &numbers[number_count]))
                        ++number_count;
                }
                size_t step = section == 2 ? 2 : 4;
                for (size_t i = 0; i + step <= number_count && value.count < 8; i += step) {
                    value.pairs[value.count][0] = numbers[i];
                    value.pairs[value.count][1] = numbers[i + 1];
                    ++value.count;
                }
                if (value.count) {
                    if (nmodels == model_capacity) {
                        size_t next_capacity = model_capacity ? model_capacity * 2 : 256;
                        if (next_capacity > SIZE_MAX / sizeof *models) {
                            ok = 0;
                            break;
                        }
                        vehparse_car_color *next = realloc(models, next_capacity * sizeof *next);
                        if (!next) {
                            ok = 0;
                            break;
                        }
                        models = next;
                        model_capacity = next_capacity;
                    }
                    models[nmodels++] = value;
                }
            }
        }
        line = end ? end + 1 : copy + length;
    }
    free(copy);
    if (ok) {
        *colors = palette;
        *color_count = npalette;
        *cars = models;
        *car_count = nmodels;
    } else {
        free(palette);
        free(models);
    }
    return ok;
}

int vehparse_surfaces(const char *limits, size_t limits_length,
                       const char *info, size_t info_length, double adhesion[36],
                       vehparse_surface **surfaces, size_t *count) {
    if (!limits || !info || !adhesion || !surfaces || !count ||
        limits_length == SIZE_MAX || info_length == SIZE_MAX) return 0;
    *surfaces = NULL;
    *count = 0;
    memset(adhesion, 0, 36 * sizeof *adhesion);
    char *text = malloc(limits_length + 1);
    if (!text) return 0;
    memcpy(text, limits, limits_length);
    text[limits_length] = 0;
    size_t row = 0;
    int ok = 1;
    for (char *line = text; line < text + limits_length && row < 6 && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *start = strip(line);
        if (*start && *start != ';') {
            char *tokens[8];
            size_t n = 0;
            for (char *p = start; *p && n < 8;) {
                while (isspace((unsigned char)*p)) ++p;
                if (!*p) break;
                tokens[n++] = p;
                while (*p && !isspace((unsigned char)*p)) ++p;
                if (*p) *p++ = 0;
            }
            for (size_t col = 0; col <= row; ++col) {
                double value = 0;
                if (col + 1 < n && strcmp(tokens[col + 1], "-"))
                    ok = float_value(tokens[col + 1], &value);
                adhesion[row * 6 + col] = adhesion[col * 6 + row] = value;
            }
            ++row;
        }
        line = end ? end + 1 : text + limits_length;
    }
    free(text);
    if (!ok) return 0;
    text = malloc(info_length + 1);
    if (!text) return 0;
    memcpy(text, info, info_length);
    text[info_length] = 0;
    vehparse_surface *result = NULL;
    size_t used = 0, capacity = 0;
    const char *groups[] = {"RUBBER", "HARD", "ROAD", "LOOSE", "SAND", "WET"};
    const char *skidmarks[] = {"DEFAULT", "SANDY", "MUDDY"};
    for (char *line = text; line < text + info_length && ok;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *tokens[5];
        size_t n = 0;
        for (char *p = strip(line); *p && n < 5;) {
            while (isspace((unsigned char)*p)) ++p;
            if (!*p) break;
            tokens[n++] = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = 0;
        }
        if (n == 5) {
            vehparse_surface value = {2, 0, 0};
            for (unsigned i = 0; i < 6; ++i)
                if (case_equal(tokens[1], groups[i])) value.group = (int32_t)i;
            double wet;
            if (float_value(tokens[3], &wet)) {
                double whole;
                double frac = modf(wet * 100.0, &whole);
                if (frac > 0.5 || (frac == 0.5 && fmod(whole, 2))) ++whole;
                if (frac < -0.5 || (frac == -0.5 && fmod(whole, 2))) --whole;
                value.wet = whole < -128 ? -128 : whole > 127 ? 127 : (int32_t)whole;
            }
            for (unsigned i = 0; i < 3; ++i)
                if (case_equal(tokens[4], skidmarks[i])) value.skid = (int32_t)i;
            if (used == capacity) {
                size_t next_capacity = capacity ? capacity * 2 : 256;
                if (next_capacity > SIZE_MAX / sizeof *result) {
                    ok = 0;
                    break;
                }
                vehparse_surface *next = realloc(result, next_capacity * sizeof *next);
                if (!next) {
                    ok = 0;
                    break;
                }
                result = next;
                capacity = next_capacity;
            }
            result[used++] = value;
        }
        line = end ? end + 1 : text + info_length;
    }
    free(text);
    if (ok) {
        *surfaces = result;
        *count = used;
    } else free(result);
    return ok;
}
