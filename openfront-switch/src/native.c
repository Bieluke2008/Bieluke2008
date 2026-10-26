// Native versions of the engine's hottest small pieces, for QuickJS.
//
// QuickJS interprets: a one-line JS method such as GameMapImpl.isLand() costs
// a call frame plus a dozen bytecodes, and the engine calls these millions of
// times a second (every conquered tile probes its neighbours, queues them in
// a heap, adds them to tile sets). Done in C each is a single native call.
//
// Each piece mirrors the TypeScript it replaces exactly, so the simulation
// is unchanged: the same values, the same iteration orders, the same heap
// tie-breaking (priorities rounded to float32 as in its Float32Array).
//   - GameMapImpl accessors, bound onto the map objects (and onto GameImpl,
//     whose versions only delegate to its map): bindMap().
//   - FlatBinaryHeap (packages/engine/src/execution/utils/FlatBinaryHeap.ts)
//   - TileSet (packages/engine-lib/src/game/TileSet.ts)
// js/native/*.ts pick these up from globalThis.__native when present, and
// fall back to the TypeScript classes elsewhere (the Node smoke test).
#include "native.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- Helpers ----------------------------------------------------------------------

/** A tile ref or coordinate argument; non-integers come back as -1. */
static inline int64_t iarg(JSContext *ctx, JSValueConst v) {
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return JS_VALUE_GET_INT(v);
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return -1;
    if (!(d == floor(d)) || d < -4e9 || d > 4e9) return -1;
    return (int64_t)d;
}

static inline double darg(JSContext *ctx, JSValueConst v) {
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return JS_VALUE_GET_INT(v);
    double d = NAN;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

static inline JSValue num_(JSContext *ctx, int64_t v) {
    return v >= INT32_MIN && v <= INT32_MAX ? JS_NewInt32(ctx, (int32_t)v) : JS_NewFloat64(ctx, (double)v);
}
#define num(v) num_(ctx, (v))

// --- GameMapImpl ---------------------------------------------------------------------

#define MAX_MAPS 8

typedef struct {
    JSValue terrain_obj, state_obj;  // kept alive while bound
    const uint8_t *terrain;
    uint16_t *state;
    int64_t w, h, size;
} NMap;

static NMap maps[MAX_MAPS];
static int nmaps;

enum {
    M_IS_LAND, M_IS_IMPASSABLE, M_IS_OCEAN, M_IS_SHORELINE, M_MAGNITUDE, M_TERRAIN_BYTE, M_OWNER_ID, M_HAS_OWNER,
    M_HAS_FALLOUT, M_HAS_DEFENSE_BONUS, M_IS_BORDER, M_IS_WATER, M_IS_SHORE, M_COST, M_TERRAIN_TYPE, M_X, M_Y,
    M_IS_VALID_COORD, M_NEIGHBORS4, M_NEIGHBORS8, M_FOR_EACH_NEIGHBOR, M_FOR_EACH_NEIGHBOR_DIAG, M_MANHATTAN,
    M_EUCLIDEAN2, M_IS_OCEAN_SHORE, M_IS_ON_EDGE,
};

static const struct {
    const char *name;
    int argc;
} MAP_METHODS[] = {
    [M_IS_LAND] = {"isLand", 1}, [M_IS_IMPASSABLE] = {"isImpassable", 1}, [M_IS_OCEAN] = {"isOcean", 1},
    [M_IS_SHORELINE] = {"isShoreline", 1}, [M_MAGNITUDE] = {"magnitude", 1}, [M_TERRAIN_BYTE] = {"terrainByte", 1},
    [M_OWNER_ID] = {"ownerID", 1}, [M_HAS_OWNER] = {"hasOwner", 1}, [M_HAS_FALLOUT] = {"hasFallout", 1},
    [M_HAS_DEFENSE_BONUS] = {"hasDefenseBonus", 1}, [M_IS_BORDER] = {"isBorder", 1}, [M_IS_WATER] = {"isWater", 1},
    [M_IS_SHORE] = {"isShore", 1}, [M_COST] = {"cost", 1}, [M_TERRAIN_TYPE] = {"terrainType", 1}, [M_X] = {"x", 1},
    [M_Y] = {"y", 1}, [M_IS_VALID_COORD] = {"isValidCoord", 2}, [M_NEIGHBORS4] = {"neighbors4", 2},
    [M_NEIGHBORS8] = {"neighbors8", 2}, [M_FOR_EACH_NEIGHBOR] = {"forEachNeighbor", 2},
    [M_FOR_EACH_NEIGHBOR_DIAG] = {"forEachNeighborWithDiag", 2}, [M_MANHATTAN] = {"manhattanDist", 2},
    [M_EUCLIDEAN2] = {"euclideanDistSquared", 2}, [M_IS_OCEAN_SHORE] = {"isOceanShore", 1},
    [M_IS_ON_EDGE] = {"isOnEdgeOfMap", 1},
};
#define N_MAP_METHODS (int)(sizeof MAP_METHODS / sizeof MAP_METHODS[0])

// Out-of-range reads behave like the typed arrays' (undefined -> 0 in bit ops).
static inline unsigned T(const NMap *m, int64_t r) { return r >= 0 && r < m->size ? m->terrain[r] : 0; }
static inline unsigned S(const NMap *m, int64_t r) { return r >= 0 && r < m->size ? m->state[r] : 0; }
static inline bool is_land(const NMap *m, int64_t r) { return T(m, r) & 0x80; }
static inline bool is_impassable(const NMap *m, int64_t r) { return is_land(m, r) && (T(m, r) & 0x1f) == 31; }
static inline unsigned owner_id(const NMap *m, int64_t r) { return S(m, r) & 0xfff; }
static inline bool is_ocean(const NMap *m, int64_t r) { return T(m, r) & 0x20; }

/** JS `ref % w` and `(ref / w) | 0` for the refs the engine passes (>= 0). */
static inline int64_t xof(const NMap *m, int64_t r) { return r % m->w; }
static inline int64_t yof(const NMap *m, int64_t r) { return r / m->w; }

static JSValue call_cb(JSContext *ctx, JSValueConst cb, int64_t v) {
    if (!JS_IsFunction(ctx, cb)) return JS_ThrowTypeError(ctx, "callback is not a function");
    JSValue a = num(v);
    JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, 1, &a);
    if (JS_IsException(r)) return r;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}

static JSValue map_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                          JSValueConst *data) {
    (void)this_val;
    int idx = JS_VALUE_GET_INT(data[0]);
    if (idx >= nmaps) return JS_ThrowTypeError(ctx, "map no longer bound");
    const NMap *m = &maps[idx];
    int64_t r = argc > 0 ? iarg(ctx, argv[0]) : -1;
    if (argc < MAP_METHODS[magic].argc) return JS_ThrowTypeError(ctx, "%s: missing arguments", MAP_METHODS[magic].name);
    switch (magic) {
        case M_IS_LAND: return JS_NewBool(ctx, is_land(m, r));
        case M_IS_IMPASSABLE: return JS_NewBool(ctx, is_impassable(m, r));
        case M_IS_OCEAN: return JS_NewBool(ctx, is_ocean(m, r));
        case M_IS_SHORELINE: return JS_NewBool(ctx, T(m, r) & 0x40);
        case M_MAGNITUDE: return JS_NewInt32(ctx, (int)(T(m, r) & 0x1f));
        case M_TERRAIN_BYTE: {
            // terrain[ref] on a typed array: undefined when out of range.
            if (r < 0 || r >= m->size) return JS_UNDEFINED;
            return JS_NewInt32(ctx, m->terrain[r]);
        }
        case M_OWNER_ID: return JS_NewInt32(ctx, (int)owner_id(m, r));
        case M_HAS_OWNER: return JS_NewBool(ctx, owner_id(m, r) != 0);
        case M_HAS_FALLOUT: return JS_NewBool(ctx, S(m, r) & (1 << 13));
        case M_HAS_DEFENSE_BONUS: return JS_NewBool(ctx, S(m, r) & (1 << 14));
        case M_IS_BORDER: {
            int64_t w = m->w, x = xof(m, r);
            unsigned o = owner_id(m, r);
            if (x != 0 && owner_id(m, r - 1) != o) return JS_TRUE;
            if (x != w - 1 && owner_id(m, r + 1) != o) return JS_TRUE;
            if (r >= w && owner_id(m, r - w) != o) return JS_TRUE;
            if (r < (m->h - 1) * w && owner_id(m, r + w) != o) return JS_TRUE;
            return JS_FALSE;
        }
        case M_IS_WATER: return JS_NewBool(ctx, !is_land(m, r));
        case M_IS_SHORE: return JS_NewBool(ctx, is_land(m, r) && (T(m, r) & 0x40));
        case M_COST: return JS_NewInt32(ctx, (T(m, r) & 0x1f) < 10 ? 2 : 1);
        case M_TERRAIN_TYPE: {
            if (!is_land(m, r)) return JS_NewInt32(ctx, 3);  // Ocean
            unsigned mag = T(m, r) & 0x1f;
            if (mag >= 31) return JS_NewInt32(ctx, 4);  // Impassable
            if (mag < 10) return JS_NewInt32(ctx, 0);
            if (mag < 20) return JS_NewInt32(ctx, 1);
            return JS_NewInt32(ctx, 2);
        }
        case M_X: return num(xof(m, r));
        case M_Y: return num(yof(m, r));
        case M_IS_VALID_COORD: {
            int64_t y = argc > 1 ? iarg(ctx, argv[1]) : -1;
            // iarg() maps non-integers to -1, which fails the range test.
            return JS_NewBool(ctx, r >= 0 && r < m->w && y >= 0 && y < m->h);
        }
        case M_NEIGHBORS4: {
            int64_t w = m->w, x = xof(m, r), out[4];
            int n = 0;
            if (r >= w) out[n++] = r - w;
            if (r < (m->h - 1) * w) out[n++] = r + w;
            if (x != 0) out[n++] = r - 1;
            if (x != w - 1) out[n++] = r + 1;
            for (int i = 0; i < n; i++) {
                if (JS_SetPropertyUint32(ctx, argv[1], (uint32_t)i, num(out[i])) < 0) return JS_EXCEPTION;
            }
            return JS_NewInt32(ctx, n);
        }
        case M_NEIGHBORS8: {
            int64_t w = m->w, x = xof(m, r), out[8];
            bool hasN = r >= w, hasS = r < (m->h - 1) * w;
            int n = 0;
            if (x != 0) {
                if (hasN) out[n++] = r - 1 - w;
                out[n++] = r - 1;
                if (hasS) out[n++] = r - 1 + w;
            }
            if (hasN) out[n++] = r - w;
            if (hasS) out[n++] = r + w;
            if (x != w - 1) {
                if (hasN) out[n++] = r + 1 - w;
                out[n++] = r + 1;
                if (hasS) out[n++] = r + 1 + w;
            }
            for (int i = 0; i < n; i++) {
                if (JS_SetPropertyUint32(ctx, argv[1], (uint32_t)i, num(out[i])) < 0) return JS_EXCEPTION;
            }
            return JS_NewInt32(ctx, n);
        }
        case M_FOR_EACH_NEIGHBOR: {
            int64_t w = m->w, x = xof(m, r);
            JSValueConst cb = argv[1];
            if (r >= w && JS_IsException(call_cb(ctx, cb, r - w))) return JS_EXCEPTION;
            if (r < (m->h - 1) * w && JS_IsException(call_cb(ctx, cb, r + w))) return JS_EXCEPTION;
            if (x != 0 && JS_IsException(call_cb(ctx, cb, r - 1))) return JS_EXCEPTION;
            if (x != w - 1 && JS_IsException(call_cb(ctx, cb, r + 1))) return JS_EXCEPTION;
            return JS_UNDEFINED;
        }
        case M_FOR_EACH_NEIGHBOR_DIAG: {
            int64_t w = m->w, x = xof(m, r), list[8];
            bool hasN = r >= w, hasS = r < (m->h - 1) * w;
            int n = 0;
            if (x != 0) {
                if (hasN) list[n++] = r - 1 - w;
                list[n++] = r - 1;
                if (hasS) list[n++] = r - 1 + w;
            }
            if (hasN) list[n++] = r - w;
            if (hasS) list[n++] = r + w;
            if (x != w - 1) {
                if (hasN) list[n++] = r + 1 - w;
                list[n++] = r + 1;
                if (hasS) list[n++] = r + 1 + w;
            }
            for (int i = 0; i < n; i++) {
                if (JS_IsException(call_cb(ctx, argv[1], list[i]))) return JS_EXCEPTION;
            }
            return JS_UNDEFINED;
        }
        case M_MANHATTAN: {
            int64_t r2 = iarg(ctx, argv[1]);
            return num(llabs(xof(m, r) - xof(m, r2)) + llabs(yof(m, r) - yof(m, r2)));
        }
        case M_EUCLIDEAN2: {
            int64_t r2 = iarg(ctx, argv[1]);
            int64_t dx = xof(m, r) - xof(m, r2), dy = yof(m, r) - yof(m, r2);
            return num(dx * dx + dy * dy);
        }
        case M_IS_OCEAN_SHORE: {
            if (!is_land(m, r)) return JS_FALSE;
            int64_t w = m->w, x = xof(m, r);
            if (x != 0 && is_ocean(m, r - 1)) return JS_TRUE;
            if (x != w - 1 && is_ocean(m, r + 1)) return JS_TRUE;
            if (r >= w && is_ocean(m, r - w)) return JS_TRUE;
            if (r < (m->h - 1) * w && is_ocean(m, r + w)) return JS_TRUE;
            return JS_FALSE;
        }
        case M_IS_ON_EDGE: {
            int64_t w = m->w, x = xof(m, r);
            if (x == 0 || x == w - 1 || r < w || r >= (m->h - 1) * w) return JS_TRUE;
            return JS_NewBool(ctx, is_impassable(m, r - 1) || is_impassable(m, r + 1) || is_impassable(m, r - w) ||
                                       is_impassable(m, r + w));
        }
    }
    return JS_UNDEFINED;
}

/** Typed array -> pointer to its first element. */
static void *typed_ptr(JSContext *ctx, JSValueConst arr, size_t *bytes) {
    size_t off = 0, len = 0, per = 0, size = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, arr, &off, &len, &per);
    if (JS_IsException(buf)) return NULL;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!p) return NULL;
    *bytes = len;
    return p + off;
}

/**
 * bindMap(target, terrain: Uint8Array, state: Uint16Array, width, height, names[])
 * defines native versions of the listed GameMapImpl methods as own
 * properties of `target`.
 */
static JSValue js_bind_map(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 6 || nmaps == MAX_MAPS) return JS_ThrowTypeError(ctx, "bindMap: bad arguments");
    // Reuse the slot of the same arrays (the game and its map share one).
    int idx = -1;
    size_t tb = 0, sb = 0;
    const uint8_t *terrain = typed_ptr(ctx, argv[1], &tb);
    uint16_t *state = typed_ptr(ctx, argv[2], &sb);
    if (!terrain || !state) return JS_ThrowTypeError(ctx, "bindMap: arrays expected");
    for (int i = 0; i < nmaps; i++) {
        if (maps[i].terrain == terrain && maps[i].state == state) idx = i;
    }
    if (idx < 0) {
        idx = nmaps++;
        NMap *m = &maps[idx];
        m->terrain_obj = JS_DupValue(ctx, argv[1]);
        m->state_obj = JS_DupValue(ctx, argv[2]);
        m->terrain = terrain;
        m->state = state;
        m->w = iarg(ctx, argv[3]);
        m->h = iarg(ctx, argv[4]);
        m->size = (int64_t)tb < (int64_t)(sb / 2) ? (int64_t)tb : (int64_t)(sb / 2);
    }
    JSValue data = JS_NewInt32(ctx, idx);
    int64_t n = 0;
    JSValue len = JS_GetPropertyStr(ctx, argv[5], "length");
    n = iarg(ctx, len);
    JS_FreeValue(ctx, len);
    for (int64_t i = 0; i < n; i++) {
        JSValue name = JS_GetPropertyUint32(ctx, argv[5], (uint32_t)i);
        const char *s = JS_ToCString(ctx, name);
        JS_FreeValue(ctx, name);
        if (!s) return JS_EXCEPTION;
        int k = -1;
        for (int j = 0; j < N_MAP_METHODS; j++) {
            if (MAP_METHODS[j].name && !strcmp(MAP_METHODS[j].name, s)) k = j;
        }
        if (k < 0) {
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx, "bindMap: unknown method");
        }
        JSValue f = JS_NewCFunctionData(ctx, map_method, MAP_METHODS[k].argc, k, 1, &data);
        JS_DefinePropertyValueStr(ctx, argv[0], s, f, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

/** Forgets bound maps (a new game). The functions stay valid while referenced. */
static JSValue js_unbind_maps(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    // A new game: its main map becomes slot 0 again. Closures bound to the
    // old game's objects die with them.
    for (int i = 0; i < nmaps; i++) {
        JS_FreeValue(ctx, maps[i].terrain_obj);
        JS_FreeValue(ctx, maps[i].state_obj);
    }
    nmaps = 0;
    return JS_UNDEFINED;
}

// --- FlatBinaryHeap -------------------------------------------------------------------

static JSClassID heap_class;

typedef struct {
    float *pri;
    uint32_t *tiles;
    int len, cap;
} Heap;

static void heap_finalizer(JSRuntime *rt, JSValue val) {
    Heap *h = JS_GetOpaque(val, heap_class);
    (void)rt;
    if (!h) return;
    free(h->pri);
    free(h->tiles);
    free(h);
}

static Heap *heap_new(int cap) {
    if (cap < 1) cap = 1;
    Heap *h = calloc(1, sizeof *h);
    h->cap = cap;
    h->pri = calloc((size_t)cap, sizeof(float));
    h->tiles = calloc((size_t)cap, sizeof(uint32_t));
    return h;
}

static JSValue heap_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    int cap = argc > 0 && !JS_IsUndefined(argv[0]) ? (int)iarg(ctx, argv[0]) : 1024;
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, heap_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, heap_new(cap));
    return obj;
}

static JSValue heap_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    Heap *h = JS_GetOpaque2(ctx, this_val, heap_class);
    if (!h) return JS_EXCEPTION;
    h->len = 0;
    return JS_UNDEFINED;
}

static JSValue heap_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    Heap *h = JS_GetOpaque2(ctx, this_val, heap_class);
    if (!h) return JS_EXCEPTION;
    return JS_NewInt32(ctx, h->len);
}

static JSValue heap_enqueue(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    Heap *h = JS_GetOpaque2(ctx, this_val, heap_class);
    if (!h || argc < 2) return JS_EXCEPTION;
    uint32_t tile = (uint32_t)iarg(ctx, argv[0]);
    double priority = darg(ctx, argv[1]);
    if (h->len == h->cap) {
        int cap = h->cap << 1;
        h->pri = realloc(h->pri, sizeof(float) * (size_t)cap);
        h->tiles = realloc(h->tiles, sizeof(uint32_t) * (size_t)cap);
        h->cap = cap;
    }
    int i = h->len++;
    // As in the TS: the incoming double is compared with the stored float32s.
    while (i > 0) {
        int parent = (i - 1) >> 1;
        if (priority >= (double)h->pri[parent]) break;
        h->pri[i] = h->pri[parent];
        h->tiles[i] = h->tiles[parent];
        i = parent;
    }
    h->pri[i] = (float)priority;
    h->tiles[i] = tile;
    return JS_UNDEFINED;
}

static uint32_t heap_pop(Heap *h) {
    uint32_t top = h->tiles[0];
    float last_pri = h->pri[--h->len];
    uint32_t last_tile = h->tiles[h->len];
    int i = 0;
    for (;;) {
        int left = (i << 1) + 1;
        if (left >= h->len) break;
        int right = left + 1;
        int child = right < h->len && h->pri[right] < h->pri[left] ? right : left;
        if (last_pri <= h->pri[child]) break;
        h->pri[i] = h->pri[child];
        h->tiles[i] = h->tiles[child];
        i = child;
    }
    h->pri[i] = last_pri;
    h->tiles[i] = last_tile;
    return top;
}

static JSValue heap_dequeue(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    Heap *h = JS_GetOpaque2(ctx, this_val, heap_class);
    if (!h) return JS_EXCEPTION;
    if (h->len == 0) return JS_ThrowPlainError(ctx, "heap empty");
    return num(heap_pop(h));
}

static const JSCFunctionListEntry heap_proto[] = {
    JS_CFUNC_DEF("clear", 0, heap_clear),
    JS_CFUNC_DEF("size", 0, heap_size),
    JS_CFUNC_DEF("enqueue", 2, heap_enqueue),
    JS_CFUNC_DEF("dequeue", 0, heap_dequeue),
};

// --- TileSet --------------------------------------------------------------------------

static JSClassID set_class, iter_class;

#define TOMBSTONE 0xffffffffu
#define EMPTY (-1)
#define DELETED (-2)

typedef struct {
    uint32_t *dense;
    int dense_cap, dense_len, size;
    int32_t *table;
    int table_len, table_used;
    int iter_depth;
} TSet;

static inline uint32_t ts_hash(uint32_t v) {
    uint32_t h = v * 0x9e3779b1u;  // Math.imul
    return h ^ (h >> 15);
}

static int next_capacity(int n) {
    int cap = 16;
    while (cap < n) cap *= 2;
    return cap;
}

static void ts_init(TSet *s) {
    s->dense_cap = 16;
    s->dense = calloc(16, sizeof(uint32_t));
    s->dense_len = s->size = 0;
    s->table_len = 32;
    s->table = malloc(sizeof(int32_t) * 32);
    for (int i = 0; i < 32; i++) s->table[i] = EMPTY;
    s->table_used = 0;
}

static void ts_rehash(TSet *s, int table_len) {
    int32_t *table = malloc(sizeof(int32_t) * (size_t)table_len);
    for (int i = 0; i < table_len; i++) table[i] = EMPTY;
    int mask = table_len - 1;
    for (int di = 0; di < s->dense_len; di++) {
        if (s->dense[di] == TOMBSTONE) continue;
        int slot = (int)(ts_hash(s->dense[di]) & (uint32_t)mask);
        while (table[slot] != EMPTY) slot = (slot + 1) & mask;
        table[slot] = di;
    }
    free(s->table);
    s->table = table;
    s->table_len = table_len;
    s->table_used = s->size;
}

static void ts_compact(TSet *s, int capacity) {
    if (capacity < 16) capacity = 16;
    uint32_t *c = calloc((size_t)capacity, sizeof(uint32_t));
    int n = 0;
    for (int i = 0; i < s->dense_len; i++) {
        if (s->dense[i] != TOMBSTONE) c[n++] = s->dense[i];
    }
    free(s->dense);
    s->dense = c;
    s->dense_cap = capacity;
    s->dense_len = n;
    int t = next_capacity(n * 2);
    ts_rehash(s, t > 32 ? t : 32);
}

static bool ts_has(const TSet *s, uint32_t v) {
    int mask = s->table_len - 1;
    int slot = (int)(ts_hash(v) & (uint32_t)mask);
    for (;;) {
        int di = s->table[slot];
        if (di == EMPTY) return false;
        if (di != DELETED && s->dense[di] == v) return true;
        slot = (slot + 1) & mask;
    }
}

static void ts_add(TSet *s, uint32_t v) {
    {
        int mask = s->table_len - 1;
        int slot = (int)(ts_hash(v) & (uint32_t)mask);
        int insert_at = -1;
        for (;;) {
            int di = s->table[slot];
            if (di == EMPTY) {
                if (insert_at == -1) insert_at = slot;
                break;
            }
            if (di == DELETED) {
                if (insert_at == -1) insert_at = slot;
            } else if (s->dense[di] == v) {
                return;
            }
            slot = (slot + 1) & mask;
        }
        if (s->dense_len < s->dense_cap && (s->table_used + 1) * 4 <= s->table_len * 3) {
            int di = s->dense_len++;
            s->dense[di] = v;
            s->size++;
            if (s->table[insert_at] == EMPTY) s->table_used++;
            s->table[insert_at] = di;
            return;
        }
    }
    if (s->dense_len == s->dense_cap) {
        if (s->iter_depth == 0 && s->dense_len - s->size >= s->size) {
            ts_compact(s, s->dense_cap);
        } else {
            int cap = s->dense_cap * 2;
            s->dense = realloc(s->dense, sizeof(uint32_t) * (size_t)cap);
            memset(s->dense + s->dense_cap, 0, sizeof(uint32_t) * (size_t)(cap - s->dense_cap));
            s->dense_cap = cap;
        }
    }
    if ((s->table_used + 1) * 4 > s->table_len * 3) {
        ts_rehash(s, s->size * 4 > s->table_len ? s->table_len * 2 : s->table_len);
    }
    int di = s->dense_len++;
    s->dense[di] = v;
    s->size++;
    int mask = s->table_len - 1;
    int slot = (int)(ts_hash(v) & (uint32_t)mask);
    while (s->table[slot] >= 0) slot = (slot + 1) & mask;
    if (s->table[slot] == EMPTY) s->table_used++;
    s->table[slot] = di;
}

static bool ts_delete(TSet *s, uint32_t v) {
    int mask = s->table_len - 1;
    int slot = (int)(ts_hash(v) & (uint32_t)mask);
    for (;;) {
        int di = s->table[slot];
        if (di == EMPTY) return false;
        if (di != DELETED && s->dense[di] == v) {
            s->table[slot] = DELETED;
            s->dense[di] = TOMBSTONE;
            s->size--;
            if (s->iter_depth == 0 && s->dense_len >= 64 && s->dense_len - s->size > s->size * 2) {
                ts_compact(s, next_capacity(s->size));
            }
            return true;
        }
        slot = (slot + 1) & mask;
    }
}

static void ts_clear(TSet *s) {
    free(s->dense);
    free(s->table);
    int depth = s->iter_depth;
    ts_init(s);
    s->iter_depth = depth;
}

static void set_finalizer(JSRuntime *rt, JSValue val) {
    TSet *s = JS_GetOpaque(val, set_class);
    (void)rt;
    if (!s) return;
    free(s->dense);
    free(s->table);
    free(s);
}

/** A tile ref argument as the uint32 the TS stores (Uint32Array semantics). */
static inline uint32_t tile_arg(JSContext *ctx, JSValueConst v) {
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return (uint32_t)JS_VALUE_GET_INT(v);
    uint32_t u = 0;
    JS_ToUint32(ctx, &u, v);
    return u;
}

static JSValue set_add_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static JSValue set_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, set_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    TSet *s = calloc(1, sizeof *s);
    ts_init(s);
    JS_SetOpaque(obj, s);
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        // for (const v of values) this.add(v)
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
        JS_FreeValue(ctx, global);
        JSValue iter_sym = JS_GetPropertyStr(ctx, symbol, "iterator");
        JS_FreeValue(ctx, symbol);
        JSAtom a = JS_ValueToAtom(ctx, iter_sym);
        JS_FreeValue(ctx, iter_sym);
        JSValue fn = JS_GetProperty(ctx, argv[0], a);
        JS_FreeAtom(ctx, a);
        JSValue it = JS_Call(ctx, fn, argv[0], 0, NULL);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(it)) {
            JS_FreeValue(ctx, obj);
            return it;
        }
        JSValue next = JS_GetPropertyStr(ctx, it, "next");
        for (;;) {
            JSValue res = JS_Call(ctx, next, it, 0, NULL);
            if (JS_IsException(res)) {
                JS_FreeValue(ctx, next);
                JS_FreeValue(ctx, it);
                JS_FreeValue(ctx, obj);
                return res;
            }
            JSValue done = JS_GetPropertyStr(ctx, res, "done");
            bool d = JS_ToBool(ctx, done);
            JS_FreeValue(ctx, done);
            if (d) {
                JS_FreeValue(ctx, res);
                break;
            }
            JSValue v = JS_GetPropertyStr(ctx, res, "value");
            ts_add(s, tile_arg(ctx, v));
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, res);
        }
        JS_FreeValue(ctx, next);
        JS_FreeValue(ctx, it);
    }
    return obj;
}

static JSValue set_size(JSContext *ctx, JSValueConst this_val) {
    TSet *s = JS_GetOpaque2(ctx, this_val, set_class);
    if (!s) return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->size);
}

static JSValue set_has_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TSet *s = JS_GetOpaque2(ctx, this_val, set_class);
    if (!s) return JS_EXCEPTION;
    if (argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, ts_has(s, tile_arg(ctx, argv[0])));
}

static JSValue set_add_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TSet *s = JS_GetOpaque2(ctx, this_val, set_class);
    if (!s) return JS_EXCEPTION;
    ts_add(s, argc > 0 ? tile_arg(ctx, argv[0]) : 0);
    return JS_DupValue(ctx, this_val);
}

static JSValue set_delete_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TSet *s = JS_GetOpaque2(ctx, this_val, set_class);
    if (!s) return JS_EXCEPTION;
    if (argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, ts_delete(s, tile_arg(ctx, argv[0])));
}

static JSValue set_clear_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    TSet *s = JS_GetOpaque2(ctx, this_val, set_class);
    if (!s) return JS_EXCEPTION;
    ts_clear(s);
    return JS_UNDEFINED;
}

static JSValue set_for_each(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TSet *s = JS_GetOpaque2(ctx, this_val, set_class);
    if (!s) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "forEach: callback expected");
    s->iter_depth++;
    // dense_len and dense are re-read each step: entries appended during
    // the walk are visited, and an append can swap in a grown buffer.
    for (int i = 0; i < s->dense_len; i++) {
        uint32_t v = s->dense[i];
        if (v == TOMBSTONE) continue;
        JSValue args[3] = {num(v), num(v), (JSValue)this_val};
        JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 3, args);
        if (JS_IsException(r)) {
            s->iter_depth--;
            return r;
        }
        JS_FreeValue(ctx, r);
    }
    s->iter_depth--;
    return JS_UNDEFINED;
}

// values(): a generator in the TS. Like one, it holds the set's iteration
// depth from the first next() until it finishes or is returned.
typedef struct {
    JSValue set;
    int index;
    bool started, finished;
} TIter;

static void iter_finalizer(JSRuntime *rt, JSValue val) {
    TIter *it = JS_GetOpaque(val, iter_class);
    if (!it) return;
    JS_FreeValueRT(rt, it->set);
    free(it);
}

static void iter_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    TIter *it = JS_GetOpaque(val, iter_class);
    if (it) JS_MarkValue(rt, it->set, mark_func);
}

static JSValue iter_result(JSContext *ctx, JSValue value, bool done) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "value", value);
    JS_SetPropertyStr(ctx, r, "done", JS_NewBool(ctx, done));
    return r;
}

static void iter_finish(TIter *it) {
    if (it->started && !it->finished) {
        TSet *s = JS_GetOpaque(it->set, set_class);
        if (s) s->iter_depth--;
    }
    it->finished = true;
}

static JSValue iter_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    TIter *it = JS_GetOpaque2(ctx, this_val, iter_class);
    if (!it) return JS_EXCEPTION;
    if (it->finished) return iter_result(ctx, JS_UNDEFINED, true);
    TSet *s = JS_GetOpaque(it->set, set_class);
    if (!it->started) {
        it->started = true;
        s->iter_depth++;
    }
    while (it->index < s->dense_len) {
        uint32_t v = s->dense[it->index++];
        if (v != TOMBSTONE) return iter_result(ctx, num(v), false);
    }
    iter_finish(it);
    return iter_result(ctx, JS_UNDEFINED, true);
}

static JSValue iter_return(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TIter *it = JS_GetOpaque2(ctx, this_val, iter_class);
    if (!it) return JS_EXCEPTION;
    iter_finish(it);
    return iter_result(ctx, argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED, true);
}

static JSValue iter_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)argc, (void)argv;
    return JS_DupValue(ctx, this_val);
}

static JSValue set_values(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    if (!JS_GetOpaque2(ctx, this_val, set_class)) return JS_EXCEPTION;
    JSValue obj = JS_NewObjectClass(ctx, (int)iter_class);
    if (JS_IsException(obj)) return obj;
    TIter *it = calloc(1, sizeof *it);
    it->set = JS_DupValue(ctx, this_val);
    JS_SetOpaque(obj, it);
    return obj;
}

static const JSCFunctionListEntry set_proto[] = {
    JS_CGETSET_DEF("size", set_size, NULL),
    JS_CFUNC_DEF("has", 1, set_has_js),
    JS_CFUNC_DEF("add", 1, set_add_js),
    JS_CFUNC_DEF("delete", 1, set_delete_js),
    JS_CFUNC_DEF("clear", 0, set_clear_js),
    JS_CFUNC_DEF("forEach", 1, set_for_each),
    JS_CFUNC_DEF("values", 0, set_values),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, set_values),
};

static const JSCFunctionListEntry iter_proto[] = {
    JS_CFUNC_DEF("next", 0, iter_next),
    JS_CFUNC_DEF("return", 1, iter_return),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, iter_self),
};


// --- Kernels: whole engine methods whose loops run per tile ------------------------------

static JSAtom atom_s[4], atom_add_border, atom_remove_border, atom_border_tiles;

/** PseudoRandom.next() on the object's sfc32 state words s0..s3. */
typedef struct {
    int32_t s0, s1, s2, s3;
} Sfc32;

static void sfc_load(JSContext *ctx, JSValueConst obj, Sfc32 *r) {
    int32_t *w[4] = {&r->s0, &r->s1, &r->s2, &r->s3};
    for (int i = 0; i < 4; i++) {
        JSValue v = JS_GetProperty(ctx, obj, atom_s[i]);
        JS_ToInt32(ctx, w[i], v);
        JS_FreeValue(ctx, v);
    }
}

static void sfc_store(JSContext *ctx, JSValueConst obj, const Sfc32 *r) {
    int32_t w[4] = {r->s0, r->s1, r->s2, r->s3};
    for (int i = 0; i < 4; i++) JS_SetProperty(ctx, obj, atom_s[i], JS_NewInt32(ctx, w[i]));
}

static double sfc_next(Sfc32 *r) {
    uint32_t s0 = (uint32_t)r->s0, s1 = (uint32_t)r->s1, s2 = (uint32_t)r->s2, s3 = (uint32_t)r->s3;
    uint32_t t = s0 + s1 + s3;
    s3 = s3 + 1;
    s0 = s1 ^ (s1 >> 9);
    s1 = s2 + (s2 << 3);
    s2 = (s2 << 21) | (s2 >> 11);
    s2 = s2 + t;
    r->s0 = (int32_t)s0, r->s1 = (int32_t)s1, r->s2 = (int32_t)s2, r->s3 = (int32_t)s3;
    return (double)t / 4294967296.0;
}

/**
 * AttackExecution.addNeighbors(tile) (packages/engine/src/execution/
 * AttackExecution.ts), with mg.ticks(), the small ids, the PseudoRandom, the
 * native heap and the attack passed in:
 * addNeighbors(tile, tickNow, targetSmallID, ownerSmallID, random, heap, attack)
 */
static JSValue js_add_neighbors(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 7 || nmaps == 0) return JS_ThrowTypeError(ctx, "addNeighbors: bad arguments");
    const NMap *m = &maps[0];
    int64_t tile = iarg(ctx, argv[0]);
    double tick_now = darg(ctx, argv[1]);
    unsigned target = (unsigned)iarg(ctx, argv[2]), owner = (unsigned)iarg(ctx, argv[3]);
    Heap *h = JS_GetOpaque2(ctx, argv[5], heap_class);
    if (!h) return JS_EXCEPTION;
    int64_t w = m->w, x = xof(m, tile), nb[4];
    int n = 0;
    if (tile >= w) nb[n++] = tile - w;
    if (tile < (m->h - 1) * w) nb[n++] = tile + w;
    if (x != 0) nb[n++] = tile - 1;
    if (x != w - 1) nb[n++] = tile + 1;
    Sfc32 rng;
    bool rng_loaded = false;
    JSValue add_border = JS_UNDEFINED;
    JSValue ret = JS_UNDEFINED;
    for (int i = 0; i < n; i++) {
        int64_t nbr = nb[i];
        if (!is_land(m, nbr) || is_impassable(m, nbr) || owner_id(m, nbr) != target) continue;
        if (JS_IsUndefined(add_border)) add_border = JS_GetProperty(ctx, argv[6], atom_add_border);
        JSValue a = num(nbr);
        JSValue r = JS_Call(ctx, add_border, argv[6], 1, &a);
        if (JS_IsException(r)) {
            ret = r;
            break;
        }
        JS_FreeValue(ctx, r);
        int owned = 0;
        int64_t nx = xof(m, nbr);
        if (nbr >= w && owner_id(m, nbr - w) == owner) owned++;
        if (nbr < (m->h - 1) * w && owner_id(m, nbr + w) == owner) owned++;
        if (nx != 0 && owner_id(m, nbr - 1) == owner) owned++;
        if (nx != w - 1 && owner_id(m, nbr + 1) == owner) owned++;
        double mag;
        unsigned t = T(m, nbr) & 0x1f;
        // terrainType(): land here (water skipped), so Plains/Highland/Mountain/Impassable.
        if (t >= 31) mag = 0;
        else if (t < 10) mag = 1;
        else if (t < 20) mag = 1.5;
        else mag = 2;
        if (!rng_loaded) {
            sfc_load(ctx, argv[4], &rng);
            rng_loaded = true;
        }
        double ri = floor(sfc_next(&rng) * 7);  // nextInt(0, 7)
        double priority = (ri + 10) * (1 - owned * 0.5 + mag / 2) + tick_now;
        // FlatBinaryHeap.enqueue
        if (h->len == h->cap) {
            int cap = h->cap << 1;
            h->pri = realloc(h->pri, sizeof(float) * (size_t)cap);
            h->tiles = realloc(h->tiles, sizeof(uint32_t) * (size_t)cap);
            h->cap = cap;
        }
        int k = h->len++;
        while (k > 0) {
            int parent = (k - 1) >> 1;
            if (priority >= (double)h->pri[parent]) break;
            h->pri[k] = h->pri[parent];
            h->tiles[k] = h->tiles[parent];
            k = parent;
        }
        h->pri[k] = (float)priority;
        h->tiles[k] = (uint32_t)nbr;
    }
    if (rng_loaded) sfc_store(ctx, argv[4], &rng);
    JS_FreeValue(ctx, add_border);
    return ret;
}

/** `visited[t]` with the typed array's out-of-range read (undefined, never equal to a gen). */
static inline uint32_t vis_at(const uint32_t *visited, int64_t len, int64_t t) {
    return t >= 0 && t < len ? visited[t] : 0;
}

/**
 * PlayerExecution.calculateClusters() after its bookkeeping:
 * clusters(borderTiles: TileSet, visited: Uint32Array, borderGen, currentGen)
 * -> { clusters: TileRef[][], boxes: Int32Array } exactly as floodFillWithGen
 * builds them (8-neighbour DFS, mark order, [minX, minY, maxX, maxY] boxes).
 */
static JSValue js_clusters(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 4 || nmaps == 0) return JS_ThrowTypeError(ctx, "clusters: bad arguments");
    const NMap *m = &maps[0];
    TSet *set = JS_GetOpaque2(ctx, argv[0], set_class);
    if (!set) return JS_EXCEPTION;
    size_t vbytes = 0;
    uint32_t *visited = typed_ptr(ctx, argv[1], &vbytes);
    if (!visited) return JS_ThrowTypeError(ctx, "clusters: Uint32Array expected");
    int64_t vlen = (int64_t)(vbytes / 4);
    uint32_t border_gen = (uint32_t)iarg(ctx, argv[2]), cur_gen = (uint32_t)iarg(ctx, argv[3]);
#define VIS(t) vis_at(visited, vlen, (t))
    for (int i = 0; i < set->dense_len; i++) {
        uint32_t v = set->dense[i];
        if (v != TOMBSTONE && v < vlen) visited[v] = border_gen;
    }
    int boxes_len = 64, nclusters = 0;
    int32_t *boxes = malloc(sizeof(int32_t) * (size_t)boxes_len);
    JSValue clusters = JS_NewArray(ctx);
    int64_t *stack = NULL, scap = 0, sp = 0;
    JSValue *vals = NULL;
    int64_t vcap = 0;
    int64_t w = m->w;
    for (int i = 0; i < set->dense_len; i++) {
        uint32_t start = set->dense[i];
        if (start == TOMBSTONE) continue;
        if (VIS((int64_t)start) == cur_gen) continue;
        if (nclusters * 4 >= boxes_len) {
            boxes_len *= 2;
            boxes = realloc(boxes, sizeof(int32_t) * (size_t)boxes_len);
        }
        int64_t count = 0;
        double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
#define MARK(t)                                                                       \
    do {                                                                              \
        int64_t t_ = (t);                                                             \
        if (t_ >= 0 && t_ < vlen) visited[t_] = cur_gen;                              \
        if (count == vcap) {                                                          \
            vcap = vcap ? vcap * 2 : 256;                                             \
            vals = realloc(vals, sizeof(JSValue) * (size_t)vcap);                     \
        }                                                                             \
        vals[count++] = num(t_);                                                      \
        if (sp == scap) {                                                             \
            scap = scap ? scap * 2 : 256;                                             \
            stack = realloc(stack, sizeof(int64_t) * (size_t)scap);                   \
        }                                                                             \
        stack[sp++] = t_;                                                             \
        double x_ = (double)xof(m, t_), y_ = (double)yof(m, t_);                      \
        if (x_ < minX) minX = x_;                                                     \
        if (y_ < minY) minY = y_;                                                     \
        if (x_ > maxX) maxX = x_;                                                     \
        if (y_ > maxY) maxY = y_;                                                     \
    } while (0)
        // includeFn(start): visited[start] === borderGen
        if (VIS((int64_t)start) == border_gen) MARK((int64_t)start);
        while (sp > 0) {
            int64_t tile = stack[--sp];
            int64_t x = xof(m, tile), nb[8];
            bool hasN = tile >= w, hasS = tile < (m->h - 1) * w;
            int n = 0;
            if (x != 0) {
                if (hasN) nb[n++] = tile - 1 - w;
                nb[n++] = tile - 1;
                if (hasS) nb[n++] = tile - 1 + w;
            }
            if (hasN) nb[n++] = tile - w;
            if (hasS) nb[n++] = tile + w;
            if (x != w - 1) {
                if (hasN) nb[n++] = tile + 1 - w;
                nb[n++] = tile + 1;
                if (hasS) nb[n++] = tile + 1 + w;
            }
            for (int k = 0; k < n; k++) {
                int64_t nbr = nb[k];
                if (VIS(nbr) == cur_gen) continue;
                if (VIS(nbr) != border_gen) continue;
                MARK(nbr);
            }
        }
#undef MARK
        // JS stores the doubles into an Int32Array (ToInt32).
        double bx[4] = {minX, minY, maxX, maxY};
        for (int k = 0; k < 4; k++) {
            int32_t iv;
            JSValue tmp = JS_NewFloat64(ctx, bx[k]);
            JS_ToInt32(ctx, &iv, tmp);
            boxes[nclusters * 4 + k] = iv;
        }
        JSValue arr = JS_NewArrayFrom(ctx, (int)count, vals);
        JS_SetPropertyUint32(ctx, clusters, (uint32_t)nclusters, arr);
        nclusters++;
    }
#undef VIS
    free(stack);
    free(vals);
    JSValue len = JS_NewInt32(ctx, boxes_len);
    JSValue boxes_arr = JS_NewTypedArray(ctx, 1, &len, JS_TYPED_ARRAY_INT32);
    size_t bb = 0;
    int32_t *bp = typed_ptr(ctx, boxes_arr, &bb);
    if (bp) memcpy(bp, boxes, sizeof(int32_t) * (size_t)(nclusters * 4));
    free(boxes);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "clusters", clusters);
    JS_SetPropertyStr(ctx, out, "boxes", boxes_arr);
    return out;
}

/**
 * NameBoxCalculator's createGrid() + findLargestInscribedRectangle():
 * nameRect(smallID, minX, minY, maxX, maxY, scalingFactor) -> [x, y, w, h]
 * in scaled cells.
 */
static JSValue js_name_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 6 || nmaps == 0) return JS_ThrowTypeError(ctx, "nameRect: bad arguments");
    const NMap *m = &maps[0];
    unsigned sid = (unsigned)iarg(ctx, argv[0]);
    double sf = darg(ctx, argv[5]);
    int64_t sminx = (int64_t)floor(darg(ctx, argv[1]) / sf), sminy = (int64_t)floor(darg(ctx, argv[2]) / sf);
    int64_t smaxx = (int64_t)floor(darg(ctx, argv[3]) / sf), smaxy = (int64_t)floor(darg(ctx, argv[4]) / sf);
    int64_t cols = smaxx - sminx + 1, rows = smaxy - sminy + 1;
    if (cols <= 0 || rows <= 0) return JS_ThrowRangeError(ctx, "nameRect: empty box");
    uint8_t *grid = calloc((size_t)(cols * rows), 1);  // grid[col * rows + row]
    int64_t s = (int64_t)sf;
    for (int64_t gx = sminx; gx <= smaxx; gx++) {
        for (int64_t gy = sminy; gy <= smaxy; gy++) {
            int64_t cx = gx * s, cy = gy * s;
            if (cx < 0 || cx >= m->w || cy < 0 || cy >= m->h) continue;
            int64_t t = cy * m->w + cx;
            unsigned tb = T(m, t);
            bool v = ((tb & 0x80) && (tb & 0x40)) || ((tb & 0x20) && (tb & 0x1f) < 10) ||
                     (owner_id(m, t) == sid && sid != 0) || (S(m, t) & (1 << 13));
            grid[(gx - sminx) * rows + (gy - sminy)] = v;
        }
    }
    int64_t *heights = calloc((size_t)cols, sizeof(int64_t));
    int64_t *stack = malloc(sizeof(int64_t) * (size_t)(cols + 1));
    double bx = 0, by = 0, bw = 0, bh = 0;
    for (int64_t row = 0; row < rows; row++) {
        for (int64_t col = 0; col < cols; col++) heights[col] = grid[col * rows + row] ? heights[col] + 1 : 0;
        // largestRectangleInHistogram(heights)
        int64_t sp = 0;
        double max_area = 0, rx = 0, rw = 0, rh = 0;
        for (int64_t i = 0; i <= cols; i++) {
            int64_t hh = i == cols ? 0 : heights[i];
            while (sp > 0 && hh < heights[stack[sp - 1]]) {
                int64_t height = heights[stack[--sp]];
                int64_t width = sp == 0 ? i : i - stack[sp - 1] - 1;
                if ((double)height * (double)width > max_area) {
                    max_area = (double)height * (double)width;
                    rx = sp == 0 ? 0 : (double)(stack[sp - 1] + 1);
                    rw = (double)width;
                    rh = (double)height;
                }
            }
            stack[sp++] = i;
        }
        if (rw * rh > bw * bh) {
            bx = rx;
            by = (double)row - rh + 1;
            bw = rw;
            bh = rh;
        }
    }
    free(grid);
    free(heights);
    free(stack);
    JSValue vals[4] = {JS_NewFloat64(ctx, bx), JS_NewFloat64(ctx, by), JS_NewFloat64(ctx, bw), JS_NewFloat64(ctx, bh)};
    return JS_NewArrayFrom(ctx, 4, vals);
}

/**
 * The skip path of AttackExecution.tick()'s conquer loop: dequeues tiles
 * (dropping each from the attack's border) until one the attacker can take
 * now, i.e. still the target's, touching the attacker, passable land.
 * nextConquerTile(heap, attack, ownerSmallID, targetSmallID) -> tile, or -1
 * once the heap is empty.
 */
static JSValue js_next_conquer_tile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 4 || nmaps == 0) return JS_ThrowTypeError(ctx, "nextConquerTile: bad arguments");
    const NMap *m = &maps[0];
    Heap *h = JS_GetOpaque2(ctx, argv[0], heap_class);
    if (!h) return JS_EXCEPTION;
    unsigned owner = (unsigned)iarg(ctx, argv[2]), target = (unsigned)iarg(ctx, argv[3]);
    JSValue remove = JS_GetProperty(ctx, argv[1], atom_remove_border);
    int64_t w = m->w, found = -1;
    while (h->len > 0) {
        int64_t tile = heap_pop(h);
        JSValue a = num(tile);
        JSValue r = JS_Call(ctx, remove, argv[1], 1, &a);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, remove);
            return r;
        }
        JS_FreeValue(ctx, r);
        int64_t x = xof(m, tile);
        bool on_border = (tile >= w && owner_id(m, tile - w) == owner) ||
                         (tile < (m->h - 1) * w && owner_id(m, tile + w) == owner) ||
                         (x != 0 && owner_id(m, tile - 1) == owner) || (x != w - 1 && owner_id(m, tile + 1) == owner);
        if (owner_id(m, tile) != target || !on_border) continue;
        if (!is_land(m, tile) || is_impassable(m, tile)) continue;
        found = tile;
        break;
    }
    JS_FreeValue(ctx, remove);
    return num(found);
}

/** GameImpl.updateBorderStatus(t): files t under its owner's border tiles or not. */
static int border_status(JSContext *ctx, const NMap *m, JSValueConst players, int64_t t) {
    unsigned sid = owner_id(m, t);
    if (sid == 0) return 0;
    JSValue p = JS_GetPropertyUint32(ctx, players, sid - 1);
    JSValue bt = JS_GetProperty(ctx, p, atom_border_tiles);
    JS_FreeValue(ctx, p);
    TSet *set = JS_GetOpaque2(ctx, bt, set_class);
    JS_FreeValue(ctx, bt);  // the player keeps it alive
    if (!set) return -1;
    int64_t w = m->w, x = xof(m, t);
    bool border = (x != 0 && owner_id(m, t - 1) != sid) || (x != w - 1 && owner_id(m, t + 1) != sid) ||
                  (t >= w && owner_id(m, t - w) != sid) || (t < (m->h - 1) * w && owner_id(m, t + w) != sid);
    if (border) ts_add(set, (uint32_t)t);
    else ts_delete(set, (uint32_t)t);
    return 0;
}

/** GameImpl.updateBorders(tile): updateBorders(tile, playersBySmallID). */
static JSValue js_update_borders(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || nmaps == 0) return JS_ThrowTypeError(ctx, "updateBorders: bad arguments");
    const NMap *m = &maps[0];
    int64_t tile = iarg(ctx, argv[0]), w = m->w, x = xof(m, tile), nb[5];
    int n = 0;
    nb[n++] = tile;
    if (tile >= w) nb[n++] = tile - w;
    if (tile < (m->h - 1) * w) nb[n++] = tile + w;
    if (x != 0) nb[n++] = tile - 1;
    if (x != w - 1) nb[n++] = tile + 1;
    for (int i = 0; i < n; i++) {
        if (border_status(ctx, m, argv[1], nb[i]) < 0) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/**
 * SpatialQuery.bfsNearest() with closestShore's predicate (land shore owned
 * by ownerSmallID) tested in C; `extra`, when a function, is the rest of the
 * predicate and runs on the tiles that pass, in the same order as in JS.
 * shoreNearest(from, maxDist, ownerSmallID, visited, gen, extra) -> tile | null
 */
static JSValue js_shore_nearest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 6 || nmaps == 0) return JS_ThrowTypeError(ctx, "shoreNearest: bad arguments");
    const NMap *m = &maps[0];
    int64_t from = iarg(ctx, argv[0]);
    double max_dist = darg(ctx, argv[1]);
    unsigned owner = (unsigned)iarg(ctx, argv[2]);
    size_t vbytes = 0;
    uint32_t *visited = typed_ptr(ctx, argv[3], &vbytes);
    if (!visited) return JS_ThrowTypeError(ctx, "shoreNearest: Uint32Array expected");
    int64_t vlen = (int64_t)(vbytes / 4);
    uint32_t gen = (uint32_t)iarg(ctx, argv[4]);
    bool has_extra = JS_IsFunction(ctx, argv[5]);
    if (from < 0 || from >= m->size || from >= vlen) return JS_NULL;
    int64_t w = m->w, fx = xof(m, from), fy = yof(m, from);
    int64_t best = -1;
    double best_dist = INFINITY;
    int64_t *stack = NULL, scap = 0, sp = 0;
    JSValue ret = JS_UNDEFINED;
#define DIST(t) ((double)(llabs(xof(m, t) - fx) + llabs(yof(m, t) - fy)))
#define MARK(t)                                                                                       \
    do {                                                                                              \
        int64_t t_ = (t);                                                                             \
        visited[t_] = gen;                                                                            \
        if (sp == scap) {                                                                             \
            scap = scap ? scap * 2 : 1024;                                                            \
            stack = realloc(stack, sizeof(int64_t) * (size_t)scap);                                   \
        }                                                                                             \
        stack[sp++] = t_;                                                                             \
        unsigned tb_ = T(m, t_);                                                                      \
        if ((tb_ & 0x80) && (tb_ & 0x40) && owner_id(m, t_) == owner) {                               \
            bool ok_ = true;                                                                          \
            if (has_extra) {                                                                          \
                JSValue a_ = num(t_);                                                                 \
                JSValue r_ = JS_Call(ctx, argv[5], JS_UNDEFINED, 1, &a_);                             \
                if (JS_IsException(r_)) {                                                             \
                    ret = r_;                                                                         \
                    goto out;                                                                         \
                }                                                                                     \
                ok_ = JS_ToBool(ctx, r_);                                                             \
                JS_FreeValue(ctx, r_);                                                                \
            }                                                                                         \
            if (ok_) {                                                                                \
                double d_ = DIST(t_);                                                                 \
                if (d_ < best_dist) best = t_, best_dist = d_;                                        \
            }                                                                                         \
        }                                                                                             \
    } while (0)
    if (max_dist >= 0) MARK(from);
    while (sp > 0) {
        int64_t cur = stack[--sp], x = xof(m, cur), nb[4];
        int n = 0;
        if (cur >= w) nb[n++] = cur - w;
        if (cur < (m->h - 1) * w) nb[n++] = cur + w;
        if (x != 0) nb[n++] = cur - 1;
        if (x != w - 1) nb[n++] = cur + 1;
        for (int i = 0; i < n; i++) {
            int64_t t = nb[i];
            if (t >= vlen) continue;
            if (visited[t] != gen && DIST(t) <= max_dist) MARK(t);
        }
    }
#undef MARK
#undef DIST
out:
    free(stack);
    if (JS_IsException(ret)) return ret;
    return best < 0 ? JS_NULL : num(best);
}

// --- Install ----------------------------------------------------------------------------

static JSValue make_class(JSContext *ctx, JSClassID *id, const char *name, JSClassFinalizer *fin, JSClassGCMark *mark,
                          const JSCFunctionListEntry *funcs, int nfuncs, JSCFunction *ctor_fn) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (*id == 0) JS_NewClassID(rt, id);
    JSClassDef def = {.class_name = name, .finalizer = fin, .gc_mark = mark};
    if (!JS_IsRegisteredClass(rt, *id)) JS_NewClass(rt, *id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, funcs, nfuncs);
    JS_SetClassProto(ctx, *id, JS_DupValue(ctx, proto));
    if (!ctor_fn) {
        JS_FreeValue(ctx, proto);
        return JS_UNDEFINED;
    }
    JSValue ctor = JS_NewCFunction2(ctx, ctor_fn, name, 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    return ctor;
}

void native_install(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue n = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, n, "bindMap", JS_NewCFunction(ctx, js_bind_map, "bindMap", 6));
    JS_SetPropertyStr(ctx, n, "unbindMaps", JS_NewCFunction(ctx, js_unbind_maps, "unbindMaps", 0));
    JS_SetPropertyStr(ctx, n, "addNeighbors", JS_NewCFunction(ctx, js_add_neighbors, "addNeighbors", 7));
    JS_SetPropertyStr(ctx, n, "clusters", JS_NewCFunction(ctx, js_clusters, "clusters", 4));
    JS_SetPropertyStr(ctx, n, "nameRect", JS_NewCFunction(ctx, js_name_rect, "nameRect", 6));
    JS_SetPropertyStr(ctx, n, "nextConquerTile", JS_NewCFunction(ctx, js_next_conquer_tile, "nextConquerTile", 4));
    JS_SetPropertyStr(ctx, n, "shoreNearest", JS_NewCFunction(ctx, js_shore_nearest, "shoreNearest", 6));
    JS_SetPropertyStr(ctx, n, "updateBorders", JS_NewCFunction(ctx, js_update_borders, "updateBorders", 2));
    static const char *snames[4] = {"s0", "s1", "s2", "s3"};
    for (int i = 0; i < 4; i++) atom_s[i] = JS_NewAtom(ctx, snames[i]);
    atom_add_border = JS_NewAtom(ctx, "addBorderTile");
    atom_remove_border = JS_NewAtom(ctx, "removeBorderTile");
    atom_border_tiles = JS_NewAtom(ctx, "_borderTiles");
    JS_SetPropertyStr(ctx, n, "FlatBinaryHeap",
                      make_class(ctx, &heap_class, "FlatBinaryHeap", heap_finalizer, NULL, heap_proto,
                                 (int)(sizeof heap_proto / sizeof heap_proto[0]), (JSCFunction *)heap_ctor));
    JS_SetPropertyStr(ctx, n, "TileSet",
                      make_class(ctx, &set_class, "TileSet", set_finalizer, NULL, set_proto,
                                 (int)(sizeof set_proto / sizeof set_proto[0]), (JSCFunction *)set_ctor));
    make_class(ctx, &iter_class, "TileSetIterator", iter_finalizer, iter_mark, iter_proto,
               (int)(sizeof iter_proto / sizeof iter_proto[0]), NULL);
    JS_SetPropertyStr(ctx, global, "__native", n);
    JS_FreeValue(ctx, global);
}
