#include "engine.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "quickjs.h"

#define MAX_CALLS 256
#define MAX_RESULTS 64
#define MAX_STRINGS 512

typedef struct {
    int id;  // 0 = result not wanted
    char method[32];
    char *args;
} Call;

typedef struct {
    int id;
    char *json;
} Result;

// --- Shared state (guarded by the mutexes noted) ------------------------------

static SDL_Thread *thread;
static SDL_mutex *cmd_lock;  // calls, start/stop requests, speed, paused, quit
static SDL_cond *cmd_cond;
static Call calls[MAX_CALLS];
static int ncalls;
static bool start_requested, stop_requested;
static StartOptions start_opts;
static double speed = 1.0;
static bool paused;
static bool quit;
static int next_call_id;

static SDL_mutex *snap_lock;  // snap, players, dirty, results
static Snapshot snap;
static PlayerInfo players[MAX_PLAYERS];
static uint32_t *dirty;
static int ndirty, dirty_cap;
static int full_redraw;
static Result results[MAX_RESULTS];
static int labels_cap, units_cap;

// Written once by the engine thread before engine_init returns.
static MapList maps;
static const char *const *string_keys;
static char *string_values[MAX_STRINGS];
static int nstrings;

static SDL_sem *ready_sem;
static bool init_ok;
static char romfs_root[256];
static char lang_code[16];

// Engine-thread only.
static JSRuntime *rt;
static JSContext *ctx;
static JSValue of_obj;
static JSValue game_buffers[3];  // terrain, state, colors: kept alive while in use
static bool running;

// --- Host functions -------------------------------------------------------------

static double now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/** Reads a whole file, or `path`.gz decompressed when only that exists. */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        fclose(f);
        *len = (size_t)n;
        return buf;
    }
    char gzpath[512];
    snprintf(gzpath, sizeof gzpath, "%s.gz", path);
    gzFile gz = gzopen(gzpath, "rb");
    if (!gz) return NULL;
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        int r = gzread(gz, buf + n, (unsigned)(cap - n));
        if (r <= 0) break;
        n += (size_t)r;
    }
    gzclose(gz);
    *len = n;
    return buf;
}

static void *realloc_buf(JSRuntime *r, void *opaque, void *ptr, size_t size) {
    (void)r;
    (void)opaque;
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static JSValue js_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) {
        const char *s = JS_ToCString(c, argv[0]);
        if (s) {
            printf("[js] %s\n", s);
            fflush(stdout);
            JS_FreeCString(c, s);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_now(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(c, now_ms());
}

static JSValue js_read_file(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *path = JS_ToCString(c, argv[0]);
    if (!path) return JS_EXCEPTION;
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    JS_FreeCString(c, path);
    if (!data) return JS_NULL;
    return JS_NewArrayBuffer(c, data, len, 0, realloc_buf, NULL, false);
}

// --- JS helpers -----------------------------------------------------------------

static void set_error(const char *msg) {
    SDL_LockMutex(snap_lock);
    snprintf(snap.error, sizeof snap.error, "%s", msg);
    SDL_UnlockMutex(snap_lock);
}

static void dump_exception(void) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    fprintf(stderr, "JS exception: %s\n%s\n", s ? s : "?", st ? st : "");
    set_error(s ? s : "JS exception");
    if (st) JS_FreeCString(ctx, st);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
}

static void run_jobs(void) {
    JSContext *c;
    while (JS_ExecutePendingJob(rt, &c) > 0) {
    }
}

/** Calls OF[name](...args) (args are freed); the caller frees the result. */
static JSValue call(const char *name, int argc, JSValue *argv) {
    JSValue fn = JS_GetPropertyStr(ctx, of_obj, name);
    JSValue r = JS_Call(ctx, fn, of_obj, argc, argv);
    JS_FreeValue(ctx, fn);
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(r)) dump_exception();
    run_jobs();
    return r;
}

static void call_void(const char *name, int argc, JSValue *argv) { JS_FreeValue(ctx, call(name, argc, argv)); }

static char *to_cstr(JSValueConst v) {
    if (JS_IsNull(v) || JS_IsUndefined(v) || JS_IsException(v)) return NULL;
    const char *s = JS_ToCString(ctx, v);
    char *out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(ctx, s);
    return out;
}

static void to_buf(JSValueConst v, char *out, size_t n) {
    out[0] = 0;
    char *s = to_cstr(v);
    if (s) {
        snprintf(out, n, "%s", s);
        free(s);
    }
}

/** Data pointer and element count of a typed array result. */
static const void *typed_data(JSValueConst ta, size_t *count, size_t elem) {
    size_t off = 0, len = 0, bpe = 0, size = 0;
    *count = 0;
    if (JS_IsException(ta)) return NULL;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, ta, &off, &len, &bpe);
    if (JS_IsException(ab)) {
        dump_exception();
        return NULL;
    }
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, ab);
    JS_FreeValue(ctx, ab);  // `ta` keeps it alive
    *count = len / elem;
    return p ? p + off : NULL;
}

static int64_t array_len(JSValueConst arr) {
    int64_t n = 0;
    JS_GetLength(ctx, arr, &n);
    return n;
}

/** OF.call(method, args) -> JSON string (malloc'd). */
static char *rpc(const char *method, const char *args) {
    JSValue argv[2] = {JS_NewString(ctx, method), JS_NewString(ctx, args ? args : "")};
    JSValue r = call("call", 2, argv);
    char *s = to_cstr(r);
    JS_FreeValue(ctx, r);
    return s ? s : strdup("null");
}

// --- Game -----------------------------------------------------------------------

static void release_game(void) {
    for (int i = 0; i < 3; i++) {
        JS_FreeValue(ctx, game_buffers[i]);
        game_buffers[i] = JS_UNDEFINED;
    }
}

static const void *buffer_of(const char *fn, int slot) {
    size_t size = 0;
    JSValue ab = call(fn, 0, NULL);
    game_buffers[slot] = ab;
    return JS_GetArrayBuffer(ctx, &size, ab);
}

static int get_int(const char *fn) {
    JSValue v = call(fn, 0, NULL);
    int n = 0;
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/** Called once the game exists: hands the render thread the tile arrays. */
static void attach_game(void) {
    int width = get_int("width"), height = get_int("height");
    const uint8_t *terrain = buffer_of("terrainBuffer", 0);
    const uint16_t *state = buffer_of("stateBuffer", 1);
    const uint8_t *colors = buffer_of("colors", 2);
    SDL_LockMutex(snap_lock);
    snap.width = width;
    snap.height = height;
    snap.terrain = terrain;
    snap.state = state;
    snap.colors = colors;
    snap.status = GAME_RUNNING;
    snap.tick = 0;
    full_redraw = 1;
    ndirty = 0;
    memset(players, 0, sizeof players);
    SDL_UnlockMutex(snap_lock);
}

/** Copies a JSON string value for `key` out of a flat object. */
static void json_str(const char *json, const char *key, char *out, size_t n) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    out[0] = 0;
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    size_t k = 0;
    while (*p && *p != '"' && k < n - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                // \uXXXX: re-encode as UTF-8 (BMP only).
                unsigned cp = (unsigned)strtol((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
                p += 5;
                if (cp < 0x80 && k + 1 < n) out[k++] = (char)cp;
                else if (cp < 0x800 && k + 2 < n) {
                    out[k++] = (char)(0xc0 | (cp >> 6));
                    out[k++] = (char)(0x80 | (cp & 63));
                } else if (k + 3 < n) {
                    out[k++] = (char)(0xe0 | (cp >> 12));
                    out[k++] = (char)(0x80 | ((cp >> 6) & 63));
                    out[k++] = (char)(0x80 | (cp & 63));
                }
                continue;
            }
        }
        out[k++] = *p++;
    }
    out[k] = 0;
}

/** Fetches names and flags of players the renderer has not seen yet. */
static void learn_players(const double *ld, size_t nl) {
    for (size_t i = 0; ld && i + 6 < nl; i += 7) {
        int id = (int)ld[i];
        if (id < 0 || id >= MAX_PLAYERS || players[id].known) continue;
        char args[32];
        snprintf(args, sizeof args, "{\"sid\":%d}", id);
        char *json = rpc("playerInfo", args);
        PlayerInfo pi = {0};
        pi.known = true;
        json_str(json, "name", pi.name, sizeof pi.name);
        json_str(json, "flag", pi.flag, sizeof pi.flag);
        pi.type = strstr(json, "\"NATION\"") ? 1 : strstr(json, "\"BOT\"") ? 2 : 0;
        free(json);
        SDL_LockMutex(snap_lock);
        players[id] = pi;
        SDL_UnlockMutex(snap_lock);
    }
}

static void refresh_relations(void) {
    static uint8_t *rel;
    char *json = rpc("relations", "{}");
    uint8_t *fresh = calloc(MAX_REL * MAX_REL, 1);
    // Flat [a, b, r, a, b, r, ...]
    const char *p = json;
    int v[3], k = 0;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            char *end;
            v[k++] = (int)strtol(p, &end, 10);
            p = end;
            if (k == 3) {
                if (v[0] < MAX_REL && v[1] < MAX_REL) {
                    uint8_t *a = &fresh[v[0] * MAX_REL + v[1]], *b = &fresh[v[1] * MAX_REL + v[0]];
                    // Embargo overrides friendly, both ways (RelationMatrix.ts).
                    if (v[2] > *a) *a = (uint8_t)v[2];
                    if (v[2] > *b) *b = (uint8_t)v[2];
                }
                k = 0;
            }
            continue;
        }
        p++;
    }
    free(json);
    SDL_LockMutex(snap_lock);
    bool changed = !rel || memcmp(rel, fresh, MAX_REL * MAX_REL) != 0;
    if (changed) {
        free(rel);
        rel = fresh;
        snap.relations = rel;
        snap.relations_version++;
    }
    SDL_UnlockMutex(snap_lock);
    if (!changed) free(fresh);
}

static void refresh_snapshot(double tick_ms) {
    JSValue labels = call("labels", 0, NULL);
    JSValue units = call("units", 0, NULL);
    JSValue tiles = call("tileUpdates", 0, NULL);
    JSValue frame = call("frame", 0, NULL);
    char *frame_json = to_cstr(frame);

    size_t nl = 0;
    const double *ld = typed_data(labels, &nl, sizeof(double));
    learn_players(ld, nl);

    int tick = 0;
    if (frame_json) {
        const char *t = strstr(frame_json, "\"tick\":");
        if (t) tick = atoi(t + 7);
    }
    if (tick % 10 == 1) refresh_relations();

    size_t nu = 0;
    const int32_t *ud = typed_data(units, &nu, sizeof(int32_t));
    size_t tsize = 0;
    const uint8_t *tp = JS_IsException(tiles) ? NULL : JS_GetArrayBuffer(ctx, &tsize, tiles);
    size_t ntu = tp ? tsize / 8 : 0;

    SDL_LockMutex(snap_lock);
    snap.tick = tick;
    snap.tick_ms = tick_ms;
    int count = (int)(nl / 7);
    if (count > labels_cap) {
        labels_cap = count * 2 + 16;
        snap.labels = realloc(snap.labels, sizeof(Label) * (size_t)labels_cap);
    }
    snap.nlabels = ld ? count : 0;
    for (int i = 0; i < snap.nlabels; i++) {
        const double *r = ld + i * 7;
        snap.labels[i] = (Label){(int)r[0], (float)r[1], (float)r[2], (float)r[3], r[4], (int)r[5], (int)r[6]};
    }
    count = (int)(nu / 9);
    if (count > units_cap) {
        units_cap = count * 2 + 16;
        snap.units = realloc(snap.units, sizeof(UnitRec) * (size_t)units_cap);
    }
    snap.nunits = ud ? count : 0;
    if (ud && snap.nunits) memcpy(snap.units, ud, sizeof(UnitRec) * (size_t)snap.nunits);
    if (ndirty + (int)ntu > dirty_cap) {
        dirty_cap = (ndirty + (int)ntu) * 2 + 1024;
        dirty = realloc(dirty, sizeof(uint32_t) * (size_t)dirty_cap);
    }
    for (size_t i = 0; i < ntu; i++) memcpy(&dirty[ndirty++], tp + i * 8, 4);
    if (frame_json) {
        free(snap.frame);
        snap.frame = frame_json;
        snap.frame_version++;
    }
    SDL_UnlockMutex(snap_lock);

    JS_FreeValue(ctx, labels);
    JS_FreeValue(ctx, units);
    JS_FreeValue(ctx, tiles);
    JS_FreeValue(ctx, frame);
}

static void do_start(const StartOptions *o) {
    SDL_LockMutex(snap_lock);
    snap.status = GAME_LOADING;
    snap.error[0] = 0;
    snap.terrain = NULL;
    snap.state = NULL;
    snap.colors = NULL;
    snap.nlabels = snap.nunits = 0;
    free(snap.frame);
    snap.frame = NULL;
    SDL_UnlockMutex(snap_lock);
    // Only now that the render thread can no longer see them.
    release_game();

    JSValue opts = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, opts, "map", JS_NewInt32(ctx, o->map));
    JS_SetPropertyStr(ctx, opts, "difficulty", JS_NewInt32(ctx, o->difficulty));
    JS_SetPropertyStr(ctx, opts, "bots", JS_NewInt32(ctx, o->bots));
    JS_SetPropertyStr(ctx, opts, "nations", JS_NewInt32(ctx, o->nations));
    JS_SetPropertyStr(ctx, opts, "name", JS_NewString(ctx, o->name));
    JS_SetPropertyStr(ctx, opts, "instantBuild", JS_NewBool(ctx, o->instant_build));
    JS_SetPropertyStr(ctx, opts, "infiniteGold", JS_NewBool(ctx, o->infinite_gold));
    JS_SetPropertyStr(ctx, opts, "infiniteTroops", JS_NewBool(ctx, o->infinite_troops));
    call_void("start", 1, &opts);
    JSValue ok = call("ready", 0, NULL);
    bool ready = JS_ToBool(ctx, ok);
    JS_FreeValue(ctx, ok);
    if (ready) {
        attach_game();
        refresh_relations();
        refresh_snapshot(0);
        running = true;
    } else {
        JSValue err = call("error", 0, NULL);
        char msg[256];
        to_buf(err, msg, sizeof msg);
        JS_FreeValue(ctx, err);
        SDL_LockMutex(snap_lock);
        snap.status = GAME_FAILED;
        snprintf(snap.error, sizeof snap.error, "%s", msg);
        SDL_UnlockMutex(snap_lock);
        running = false;
    }
}

static void do_stop(void) {
    running = false;
    SDL_LockMutex(snap_lock);
    snap.status = GAME_IDLE;
    snap.terrain = NULL;
    snap.state = NULL;
    snap.colors = NULL;
    SDL_UnlockMutex(snap_lock);
    release_game();
}

static void store_result(int id, char *json) {
    SDL_LockMutex(snap_lock);
    Result *slot = &results[id % MAX_RESULTS];
    free(slot->json);
    slot->id = id;
    slot->json = json;
    SDL_UnlockMutex(snap_lock);
}

static bool load_engine(void) {
    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 3 * 1024 * 1024);
    ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue host = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, host, "log", JS_NewCFunction(ctx, js_log, "log", 1));
    JS_SetPropertyStr(ctx, host, "now", JS_NewCFunction(ctx, js_now, "now", 0));
    JS_SetPropertyStr(ctx, host, "readFile", JS_NewCFunction(ctx, js_read_file, "readFile", 1));
    JS_SetPropertyStr(ctx, global, "host", host);
    JS_SetPropertyStr(ctx, global, "ROMFS_ROOT", JS_NewString(ctx, romfs_root));
    for (int i = 0; i < 3; i++) game_buffers[i] = JS_UNDEFINED;

    char path[512];
    snprintf(path, sizeof path, "%sengine.js", romfs_root);
    size_t len = 0;
    char *src = (char *)read_file(path, &len);
    if (!src) {
        fprintf(stderr, "cannot read %s\n", path);
        JS_FreeValue(ctx, global);
        return false;
    }
    src = realloc(src, len + 1);
    src[len] = 0;
    double t0 = now_ms();
    JSValue r = JS_Eval(ctx, src, len, "engine.js", JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(r)) {
        dump_exception();
        JS_FreeValue(ctx, global);
        return false;
    }
    JS_FreeValue(ctx, r);
    printf("engine.js loaded in %.0f ms\n", now_ms() - t0);
    of_obj = JS_GetPropertyStr(ctx, global, "OF");
    JS_FreeValue(ctx, global);

    JSValue arg = JS_NewString(ctx, lang_code);
    call_void("init", 1, &arg);

    JSValue list = call("mapNames", 0, NULL);
    maps.nmaps = (int)array_len(list);
    maps.map_names = calloc((size_t)maps.nmaps + 1, NAME_LEN);
    maps.map_folders = calloc((size_t)maps.nmaps + 1, NAME_LEN);
    for (int i = 0; i < maps.nmaps; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
        to_buf(v, maps.map_names[i], NAME_LEN);
        JS_FreeValue(ctx, v);
        JSValue idx = JS_NewInt32(ctx, i);
        JSValue f = call("mapFolder", 1, &idx);
        to_buf(f, maps.map_folders[i], NAME_LEN);
        JS_FreeValue(ctx, f);
    }
    JS_FreeValue(ctx, list);

    for (nstrings = 0; string_keys && string_keys[nstrings] && nstrings < MAX_STRINGS; nstrings++) {
        JSValue k = JS_NewString(ctx, string_keys[nstrings]);
        JSValue v = call("t", 1, &k);
        char *s = to_cstr(v);
        string_values[nstrings] = s ? s : strdup(string_keys[nstrings]);
        JS_FreeValue(ctx, v);
    }
    return true;
}

static int engine_main(void *unused) {
    (void)unused;
    init_ok = load_engine();
    SDL_SemPost(ready_sem);
    if (!init_ok) return 1;

    double next_tick = now_ms();
    Call local[MAX_CALLS];
    for (;;) {
        SDL_LockMutex(cmd_lock);
        int n = ncalls;
        memcpy(local, calls, sizeof(Call) * (size_t)n);
        ncalls = 0;
        bool q = quit, p = paused;
        bool do_s = start_requested, do_x = stop_requested;
        StartOptions opts = start_opts;
        start_requested = stop_requested = false;
        double interval = 100.0 / speed;
        SDL_UnlockMutex(cmd_lock);
        if (q) break;

        if (do_x) do_stop();
        if (do_s) do_start(&opts);
        for (int i = 0; i < n; i++) {
            if (running) {
                char *json = rpc(local[i].method, local[i].args);
                if (local[i].id) store_result(local[i].id, json);
                else free(json);
            } else if (local[i].id) {
                store_result(local[i].id, strdup("null"));
            }
            free(local[i].args);
        }

        double now = now_ms();
        if (running && !p && now >= next_tick) {
            double t0 = now_ms();
            JS_FreeValue(ctx, call("tick", 0, NULL));
            double dt = now_ms() - t0;
            refresh_snapshot(dt);
            next_tick += interval;
            // Running behind (slow CPU): don't try to catch up in a burst.
            if (next_tick < now_ms()) next_tick = now_ms();
            continue;
        }
        // Paused: still show what the calls changed (ratio, ghost...).
        if (running && p && n > 0) refresh_snapshot(snap.tick_ms);
        if (!running || p) next_tick = now;

        SDL_LockMutex(cmd_lock);
        if (ncalls == 0 && !quit && !start_requested && !stop_requested) {
            double wait = running && !p ? next_tick - now_ms() : 50;
            if (wait > 0) SDL_CondWaitTimeout(cmd_cond, cmd_lock, (Uint32)wait + 1);
        }
        SDL_UnlockMutex(cmd_lock);
    }
    release_game();
    JS_FreeValue(ctx, of_obj);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}

// --- Public API -----------------------------------------------------------------

bool engine_init(const char *romfs, const char *lang, const char *const *keys) {
    snprintf(romfs_root, sizeof romfs_root, "%s", romfs);
    snprintf(lang_code, sizeof lang_code, "%s", lang);
    string_keys = keys;
    cmd_lock = SDL_CreateMutex();
    cmd_cond = SDL_CreateCond();
    snap_lock = SDL_CreateMutex();
    ready_sem = SDL_CreateSemaphore(0);
    thread = SDL_CreateThreadWithStackSize(engine_main, "engine", 4 * 1024 * 1024, NULL);
    SDL_SemWait(ready_sem);
    if (!init_ok) {
        SDL_WaitThread(thread, NULL);
        thread = NULL;
    }
    return init_ok;
}

void engine_shutdown(void) {
    if (!thread) return;
    SDL_LockMutex(cmd_lock);
    quit = true;
    SDL_CondSignal(cmd_cond);
    SDL_UnlockMutex(cmd_lock);
    SDL_WaitThread(thread, NULL);
    thread = NULL;
}

void engine_start(const StartOptions *opts) {
    SDL_LockMutex(snap_lock);
    snap.status = GAME_LOADING;
    SDL_UnlockMutex(snap_lock);
    SDL_LockMutex(cmd_lock);
    start_opts = *opts;
    start_requested = true;
    SDL_CondSignal(cmd_cond);
    SDL_UnlockMutex(cmd_lock);
}

void engine_stop(void) {
    SDL_LockMutex(cmd_lock);
    stop_requested = true;
    SDL_CondSignal(cmd_cond);
    SDL_UnlockMutex(cmd_lock);
}

void engine_set_speed(double s) {
    SDL_LockMutex(cmd_lock);
    speed = s;
    SDL_CondSignal(cmd_cond);
    SDL_UnlockMutex(cmd_lock);
}

void engine_set_paused(bool p) {
    SDL_LockMutex(cmd_lock);
    paused = p;
    SDL_CondSignal(cmd_cond);
    SDL_UnlockMutex(cmd_lock);
}

int engine_call(const char *method, const char *args_json, bool want_result) {
    SDL_LockMutex(cmd_lock);
    int id = 0;
    if (ncalls < MAX_CALLS) {
        Call *c = &calls[ncalls++];
        if (want_result) {
            if (++next_call_id > 1000000000) next_call_id = 1;
            id = next_call_id;
        }
        c->id = id;
        snprintf(c->method, sizeof c->method, "%s", method);
        c->args = strdup(args_json ? args_json : "{}");
    }
    SDL_CondSignal(cmd_cond);
    SDL_UnlockMutex(cmd_lock);
    return id;
}

char *engine_result(int id) {
    if (id <= 0) return NULL;
    SDL_LockMutex(snap_lock);
    Result *slot = &results[id % MAX_RESULTS];
    char *out = NULL;
    if (slot->id == id && slot->json) {
        out = slot->json;
        slot->json = NULL;
        slot->id = 0;
    }
    SDL_UnlockMutex(snap_lock);
    return out;
}

const Snapshot *engine_lock(void) {
    SDL_LockMutex(snap_lock);
    return &snap;
}

void engine_unlock(void) { SDL_UnlockMutex(snap_lock); }

const PlayerInfo *engine_player(int id) {
    static const PlayerInfo none = {{0}, {0}, 0, false};
    if (id < 0 || id >= MAX_PLAYERS) return &none;
    return &players[id];
}

int engine_take_dirty(uint32_t **out, int *full) {
    SDL_LockMutex(snap_lock);
    int n = ndirty;
    *out = NULL;
    if (n > 0) {
        *out = malloc(sizeof(uint32_t) * (size_t)n);
        memcpy(*out, dirty, sizeof(uint32_t) * (size_t)n);
    }
    ndirty = 0;
    *full = full_redraw;
    full_redraw = 0;
    SDL_UnlockMutex(snap_lock);
    return n;
}

const MapList *engine_maps(void) { return &maps; }

const char *engine_t(const char *key) {
    for (int i = 0; i < nstrings; i++) {
        if (strcmp(string_keys[i], key) == 0) return string_values[i];
    }
    return key;
}
