// Small helpers over cJSON for reading the adapter's models.
#pragma once

#include <SDL.h>
#include <stdbool.h>
#include <stdlib.h>

#include "cJSON.h"

static inline const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }

static inline const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

static inline double jnum(const cJSON *o, const char *k, double def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static inline bool jbool(const cJSON *o, const char *k) {
    const cJSON *v = jget(o, k);
    return cJSON_IsTrue(v);
}

static inline bool jnull(const cJSON *o, const char *k) {
    const cJSON *v = jget(o, k);
    return v == NULL || cJSON_IsNull(v);
}

static inline int jlen(const cJSON *a) { return cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0; }

static inline const cJSON *jat(const cJSON *a, int i) { return cJSON_IsArray(a) ? cJSON_GetArrayItem(a, i) : NULL; }

/** "#rrggbb" to a colour. */
static inline SDL_Color jcolor(const char *s) {
    unsigned v = 0x808080;
    if (s && s[0] == '#') v = (unsigned)strtoul(s + 1, NULL, 16);
    SDL_Color c = {(Uint8)(v >> 16), (Uint8)(v >> 8), (Uint8)v, 255};
    return c;
}
