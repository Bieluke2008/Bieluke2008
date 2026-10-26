#include "engine.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "quickjs.h"

#define MAX_CMDS 256
#define MAX_MSGS 64
#define MAX_STRINGS 256

// --- Shared state (guarded by the mutexes noted) ------------------------------

static SDL_Thread *thread;
static SDL_mutex *cmd_lock;  // cmds, speed, paused, quit
static SDL_cond *cmd_cond;
static Cmd cmds[MAX_CMDS];
static int ncmds;
static double speed = 1.0;
static bool paused;
static bool quit;

static SDL_mutex *snap_lock;  // snap, names, dirty, msgs, menu
static Snapshot snap;
static char names[MAX_PLAYERS][NAME_LEN];
static uint32_t *dirty;
static int ndirty, dirty_cap;
static int full_redraw;
static Msg msgs[MAX_MSGS];
static int msg_head, msg_count;
static MenuResult menu;
static int seq_counter;

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
    return (double)SDL_GetPerformanceCounter() * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
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

static void dump_exception(void) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    fprintf(stderr, "JS exception: %s\n%s\n", s ? s : "?", st ? st : "");
    SDL_LockMutex(snap_lock);
    snprintf(snap.error, sizeof snap.error, "%s", s ? s : "JS exception");
    SDL_UnlockMutex(snap_lock);
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

/** Calls OF[name](...args); returns the result (caller frees) or JS_EXCEPTION. */
static JSValue call(const char *name, int argc, JSValue *argv) {
    JSValue fn = JS_GetPropertyStr(ctx, of_obj, name);
    JSValue r = JS_Call(ctx, fn, of_obj, argc, argv);
    JS_FreeValue(ctx, fn);
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(r)) dump_exception();
    run_jobs();
    return r;
}

static void call_void(const char *name, int argc, JSValue *argv) {
    JS_FreeValue(ctx, call(name, argc, argv));
}

static double get_num(JSValueConst obj, const char *prop) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static bool get_bool(JSValueConst obj, const char *prop) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static void get_str(JSValueConst obj, const char *prop, char *out, size_t n) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    out[0] = 0;
    if (!JS_IsNull(v) && !JS_IsUndefined(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            snprintf(out, n, "%s", s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
}

static void to_str(JSValueConst v, char *out, size_t n) {
    out[0] = 0;
    const char *s = JS_ToCString(ctx, v);
    if (s) {
        snprintf(out, n, "%s", s);
        JS_FreeCString(ctx, s);
    }
}

/** Data pointer and element count of a typed array result. */
static const void *typed_data(JSValueConst ta, size_t *count, size_t elem) {
    size_t off = 0, len = 0, bpe = 0, size = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, ta, &off, &len, &bpe);
    if (JS_IsException(ab)) {
        dump_exception();
        *count = 0;
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

// --- Game -----------------------------------------------------------------------

static void push_msg(const char *text, int kind) {
    int i = (msg_head + msg_count) % MAX_MSGS;
    if (msg_count == MAX_MSGS) {
        msg_head = (msg_head + 1) % MAX_MSGS;
        msg_count--;
    }
    snprintf(msgs[i].text, sizeof msgs[i].text, "%s", text);
    msgs[i].kind = kind;
    msg_count++;
}

static void release_game(void) {
    for (int i = 0; i < 3; i++) {
        JS_FreeValue(ctx, game_buffers[i]);
        game_buffers[i] = JS_UNDEFINED;
    }
}

static const void *buffer_of(const char *fn, int slot, size_t *size) {
    JSValue ab = call(fn, 0, NULL);
    game_buffers[slot] = ab;
    return JS_GetArrayBuffer(ctx, size, ab);
}

/** Called once the game exists: hands the render thread the tile arrays. */
static void attach_game(void) {
    size_t tsize = 0, ssize = 0, csize = 0;
    JSValue w = call("width", 0, NULL), h = call("height", 0, NULL);
    int width = 0, height = 0;
    JS_ToInt32(ctx, &width, w);
    JS_ToInt32(ctx, &height, h);
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, h);
    const uint8_t *terrain = buffer_of("terrainBuffer", 0, &tsize);
    const uint16_t *state = buffer_of("stateBuffer", 1, &ssize);
    const uint8_t *colors = buffer_of("colors", 2, &csize);

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
    memset(names, 0, sizeof names);
    SDL_UnlockMutex(snap_lock);
}

static void refresh_snapshot(double tick_ms) {
    JSValue hud = call("hud", 0, NULL);
    JSValue labels = call("labels", 0, NULL);
    JSValue units = call("units", 0, NULL);
    JSValue tiles = call("tileUpdates", 0, NULL);
    JSValue messages = call("messages", 0, NULL);

    // Names for new labels: they never change, so fetch each only once.
    size_t nl = 0;
    const double *ld = typed_data(labels, &nl, sizeof(double));
    for (size_t i = 0; ld && i + 4 < nl; i += 5) {
        int id = (int)ld[i];
        if (id >= 0 && id < MAX_PLAYERS && names[id][0] == 0) {
            JSValue arg = JS_NewInt32(ctx, id);
            JSValue r = call("playerName", 1, &arg);
            char tmp[NAME_LEN];
            to_str(r, tmp, sizeof tmp);
            JS_FreeValue(ctx, r);
            SDL_LockMutex(snap_lock);
            memcpy(names[id], tmp, NAME_LEN);
            SDL_UnlockMutex(snap_lock);
        }
    }
    JSValue leaders = JS_GetPropertyStr(ctx, hud, "leaders");
    int64_t nlead = array_len(leaders);
    int64_t nmsg = array_len(messages);

    SDL_LockMutex(snap_lock);
    snap.tick = (int)get_num(hud, "tick");
    snap.seconds = get_num(hud, "seconds");
    snap.spawn_phase = get_bool(hud, "spawn");
    snap.spawned = get_bool(hud, "spawned");
    snap.alive = get_bool(hud, "alive");
    snap.me = (int)get_num(hud, "me");
    snap.troops = get_num(hud, "troops");
    snap.max_troops = get_num(hud, "maxTroops");
    snap.gold = get_num(hud, "gold");
    snap.tiles = (int)get_num(hud, "tiles");
    snap.pct = get_num(hud, "pct");
    snap.attacks = (int)get_num(hud, "attacks");
    snap.ratio = get_num(hud, "ratio");
    snap.pending_alliances = (int)get_num(hud, "pendingAlliances");
    get_str(hud, "winner", snap.winner, sizeof snap.winner);
    snap.tick_ms = tick_ms;
    snap.nleaders = 0;
    for (int64_t i = 0; i < nlead && i < MAX_LEADERS; i++) {
        JSValue l = JS_GetPropertyUint32(ctx, leaders, (uint32_t)i);
        Leader *ld2 = &snap.leaders[snap.nleaders++];
        ld2->id = (int)get_num(l, "id");
        ld2->pct = get_num(l, "pct");
        get_str(l, "name", ld2->name, sizeof ld2->name);
        JS_FreeValue(ctx, l);
    }

    snap.nlabels = 0;
    for (size_t i = 0; ld && i + 4 < nl && snap.nlabels < MAX_PLAYERS; i += 5) {
        Label *l = &snap.labels[snap.nlabels++];
        l->id = (int)ld[i];
        l->x = (float)ld[i + 1];
        l->y = (float)ld[i + 2];
        l->size = (float)ld[i + 3];
        l->troops = ld[i + 4];
    }

    size_t nu = 0;
    const int32_t *ud = typed_data(units, &nu, sizeof(int32_t));
    int count = (int)(nu / 5);
    if (count > snap.units_cap) {
        snap.units_cap = count * 2;
        snap.units = realloc(snap.units, sizeof(UnitRec) * (size_t)snap.units_cap);
    }
    snap.nunits = ud ? count : 0;
    if (ud) memcpy(snap.units, ud, sizeof(UnitRec) * (size_t)snap.nunits);

    size_t ntu = 0, tsize = 0;
    const uint8_t *tp = JS_GetArrayBuffer(ctx, &tsize, tiles);
    ntu = tp ? tsize / 8 : 0;
    if (ndirty + (int)ntu > dirty_cap) {
        dirty_cap = (ndirty + (int)ntu) * 2 + 1024;
        dirty = realloc(dirty, sizeof(uint32_t) * (size_t)dirty_cap);
    }
    for (size_t i = 0; i < ntu; i++) {
        uint32_t ref;
        memcpy(&ref, tp + i * 8, 4);
        dirty[ndirty++] = ref;
    }

    for (int64_t i = 0; i < nmsg; i++) {
        JSValue m = JS_GetPropertyUint32(ctx, messages, (uint32_t)i);
        char text[160];
        get_str(m, "text", text, sizeof text);
        push_msg(text, (int)get_num(m, "kind"));
        JS_FreeValue(ctx, m);
    }
    SDL_UnlockMutex(snap_lock);

    JS_FreeValue(ctx, leaders);
    JS_FreeValue(ctx, hud);
    JS_FreeValue(ctx, labels);
    JS_FreeValue(ctx, units);
    JS_FreeValue(ctx, tiles);
    JS_FreeValue(ctx, messages);
}

static void menu_from_build(JSValueConst list, int seq, int x, int y) {
    MenuResult r;
    memset(&r, 0, sizeof r);
    r.kind = MENU_BUILD;
    r.seq = seq;
    r.x = x;
    r.y = y;
    int64_t n = array_len(list);
    for (int64_t i = 0; i < n && r.count < MAX_MENU; i++) {
        JSValue it = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
        MenuItem *m = &r.items[r.count++];
        m->kind = (int)get_num(it, "kind");
        m->cost = get_num(it, "cost");
        m->enabled = get_bool(it, "canBuild");
        get_str(it, "name", m->name, sizeof m->name);
        JS_FreeValue(ctx, it);
    }
    SDL_LockMutex(snap_lock);
    menu = r;
    SDL_UnlockMutex(snap_lock);
}

static void menu_from_player(JSValueConst p, int seq, int x, int y) {
    MenuResult r;
    memset(&r, 0, sizeof r);
    r.kind = MENU_PLAYER;
    r.seq = seq;
    r.x = x;
    r.y = y;
    if (JS_IsNull(p) || JS_IsException(p)) {
        r.player = -1;
    } else {
        r.player = (int)get_num(p, "id");
        get_str(p, "name", r.player_name, sizeof r.player_name);
        r.player_troops = get_num(p, "troops");
        r.ally = get_bool(p, "ally");
        r.can_ally = get_bool(p, "canAlly");
        r.can_break = get_bool(p, "canBreak");
        r.can_target = get_bool(p, "canTarget");
        r.can_embargo = get_bool(p, "canEmbargo");
        r.can_donate = get_bool(p, "canDonate");
    }
    SDL_LockMutex(snap_lock);
    menu = r;
    SDL_UnlockMutex(snap_lock);
}

static JSValue int2(int a, int b, JSValue *argv) {
    argv[0] = JS_NewInt32(ctx, a);
    argv[1] = JS_NewInt32(ctx, b);
    return JS_UNDEFINED;
}

static void handle(const Cmd *c) {
    JSValue argv[3];
    switch (c->type) {
        case CMD_START: {
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "map", JS_NewInt32(ctx, c->start.map));
            JS_SetPropertyStr(ctx, o, "difficulty", JS_NewInt32(ctx, c->start.difficulty));
            JS_SetPropertyStr(ctx, o, "bots", JS_NewInt32(ctx, c->start.bots));
            JS_SetPropertyStr(ctx, o, "nations", JS_NewInt32(ctx, c->start.nations));
            JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, c->start.name));
            SDL_LockMutex(snap_lock);
            snap.status = GAME_LOADING;
            snap.error[0] = 0;
            snap.terrain = NULL;
            snap.state = NULL;
            snap.colors = NULL;
            snap.nlabels = snap.nunits = 0;
            snap.winner[0] = 0;
            msg_count = 0;
            menu.kind = MENU_NONE;
            SDL_UnlockMutex(snap_lock);
            // Only now that the render thread can no longer see them.
            release_game();
            argv[0] = o;
            call_void("start", 1, argv);
            JSValue ok = call("ready", 0, NULL);
            bool ready = JS_ToBool(ctx, ok);
            JS_FreeValue(ctx, ok);
            if (ready) {
                attach_game();
                refresh_snapshot(0);
                running = true;
            } else {
                JSValue err = call("error", 0, NULL);
                SDL_LockMutex(snap_lock);
                snap.status = GAME_FAILED;
                to_str(err, snap.error, sizeof snap.error);
                SDL_UnlockMutex(snap_lock);
                JS_FreeValue(ctx, err);
                running = false;
            }
            break;
        }
        case CMD_STOP:
            running = false;
            SDL_LockMutex(snap_lock);
            snap.status = GAME_IDLE;
            snap.terrain = NULL;
            snap.state = NULL;
            snap.colors = NULL;
            SDL_UnlockMutex(snap_lock);
            release_game();
            break;
        default:
            if (!running) break;
            switch (c->type) {
                case CMD_PRIMARY:
                    int2(c->a, c->b, argv);
                    call_void("primary", 2, argv);
                    break;
                case CMD_BOAT:
                    int2(c->a, c->b, argv);
                    call_void("boat", 2, argv);
                    break;
                case CMD_BUILD:
                    argv[0] = JS_NewInt32(ctx, c->a);
                    int2(c->b, c->c, argv + 1);
                    call_void("build", 3, argv);
                    break;
                case CMD_BUILD_MENU: {
                    int2(c->a, c->b, argv);
                    JSValue r = call("buildMenu", 2, argv);
                    if (!JS_IsException(r)) menu_from_build(r, c->c, c->a, c->b);
                    JS_FreeValue(ctx, r);
                    break;
                }
                case CMD_PLAYER_MENU: {
                    int2(c->a, c->b, argv);
                    JSValue r = call("playerMenu", 2, argv);
                    menu_from_player(r, c->c, c->a, c->b);
                    JS_FreeValue(ctx, r);
                    break;
                }
                case CMD_PLAYER_ACTION:
                    int2(c->a, c->b, argv);
                    call_void("playerAction", 2, argv);
                    break;
                case CMD_ALLIANCES:
                    argv[0] = JS_NewBool(ctx, c->a != 0);
                    call_void("answerAlliances", 1, argv);
                    break;
                case CMD_RATIO:
                    argv[0] = JS_NewFloat64(ctx, c->f);
                    call_void("setAttackRatio", 1, argv);
                    break;
                case CMD_CANCEL_ATTACK:
                    call_void("cancelLastAttack", 0, NULL);
                    break;
                default:
                    break;
            }
    }
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
        to_str(v, maps.map_names[i], NAME_LEN);
        JS_FreeValue(ctx, v);
        JSValue idx = JS_NewInt32(ctx, i);
        JSValue f = call("mapFolder", 1, &idx);
        to_str(f, maps.map_folders[i], NAME_LEN);
        JS_FreeValue(ctx, f);
    }
    JS_FreeValue(ctx, list);

    for (nstrings = 0; string_keys && string_keys[nstrings] && nstrings < MAX_STRINGS; nstrings++) {
        JSValue k = JS_NewString(ctx, string_keys[nstrings]);
        JSValue v = call("t", 1, &k);
        char tmp[256];
        to_str(v, tmp, sizeof tmp);
        string_values[nstrings] = strdup(tmp);
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
    Cmd local[MAX_CMDS];
    for (;;) {
        SDL_LockMutex(cmd_lock);
        int n = ncmds;
        memcpy(local, cmds, sizeof(Cmd) * (size_t)n);
        ncmds = 0;
        bool q = quit, p = paused;
        double interval = 100.0 / speed;
        SDL_UnlockMutex(cmd_lock);
        if (q) break;

        for (int i = 0; i < n; i++) handle(&local[i]);

        double now = now_ms();
        if (running && !p && now >= next_tick) {
            double t0 = now_ms();
            JSValue r = call("tick", 0, NULL);
            JS_FreeValue(ctx, r);
            double dt = now_ms() - t0;
            refresh_snapshot(dt);
            next_tick += interval;
            // Running behind (slow CPU): don't try to catch up in a burst.
            if (next_tick < now_ms()) next_tick = now_ms();
            continue;
        }
        if (!running || p) next_tick = now;

        SDL_LockMutex(cmd_lock);
        if (ncmds == 0 && !quit) {
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

void engine_send(const Cmd *cmd) {
    SDL_LockMutex(cmd_lock);
    if (ncmds < MAX_CMDS) cmds[ncmds++] = *cmd;
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

const Snapshot *engine_lock(void) {
    SDL_LockMutex(snap_lock);
    return &snap;
}

void engine_unlock(void) { SDL_UnlockMutex(snap_lock); }

const char *engine_player_name(int id) {
    if (id < 0 || id >= MAX_PLAYERS) return "";
    return names[id];
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

bool engine_pop_message(Msg *out) {
    SDL_LockMutex(snap_lock);
    bool ok = msg_count > 0;
    if (ok) {
        *out = msgs[msg_head];
        msg_head = (msg_head + 1) % MAX_MSGS;
        msg_count--;
    }
    SDL_UnlockMutex(snap_lock);
    return ok;
}

int engine_menu(MenuResult *out) {
    SDL_LockMutex(snap_lock);
    *out = menu;
    SDL_UnlockMutex(snap_lock);
    return out->seq;
}

int engine_next_seq(void) { return ++seq_counter; }

const MapList *engine_maps(void) { return &maps; }

const char *engine_t(const char *key) {
    for (int i = 0; i < nstrings; i++) {
        if (strcmp(string_keys[i], key) == 0) return string_values[i];
    }
    return key;
}
