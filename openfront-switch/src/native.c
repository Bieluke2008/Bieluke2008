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
    const NMap *m = &maps[JS_VALUE_GET_INT(data[0])];
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
    // Keep the arrays alive: old closures may still be reachable until GC.
    // Their slots are only reused by a new bind of different arrays.
    (void)ctx;
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

static JSValue heap_dequeue(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc, (void)argv;
    Heap *h = JS_GetOpaque2(ctx, this_val, heap_class);
    if (!h) return JS_EXCEPTION;
    if (h->len == 0) return JS_ThrowPlainError(ctx, "heap empty");
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
    return num(top);
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
