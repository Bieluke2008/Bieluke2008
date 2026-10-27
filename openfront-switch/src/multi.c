#include "multi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "app.h"
#include "gfx.h"
#include "home.h"
#include "json.h"
#include "lanhost.h"
#include "ldn.h"
#include "net.h"
#include "settings.h"
#include "solo.h"
#include "ui.h"

// The home page's palette.
#define CARD_BG 0x13061e
#define CARD_BORDER 0x2b1e34
#define ITEM_BG 0x201229
#define ITEM_BORDER 0x33263d
#define ORANGE 0xe06c12
#define LABEL 0xa99aa8
#define MUTED 0x8f8696

// The OpenFront dev server's port (vite, which also proxies the game workers).
#define DEFAULT_PORT 9000
// Workers a server may run; a lobby lives on one of them (simpleHash % n).
#define MAX_WORKERS 8

typedef enum { ST_MENU, ST_CREATING, ST_FINDING, ST_LOBBY } State;

static struct {
    State st;
    bool in_game;
    char server[128];
    char host[128];
    int port;
    char code[32];
    char error[256];
    int http_id;
    int probe;  // the worker asked about the lobby while joining
    int lobby_req;
    double lobby_at;
    cJSON *lobby;
    StartOptions sent;  // the config the server last got from us (host)
    bool sent_valid;
    double changed_at;
    char token[40];
    bool is_host;       // the lobby is ours
    bool lan_host;      // ...and this console is its server (lanhost.c)
    bool wireless;      // the console's local wireless (LDN) instead of Wi-Fi
    bool scanning;      // LDN station open and scanning
    char lan_hint[160]; // how other consoles join it
    int auto_start;     // tests: start once this many players are in
    int auto_lan;       // tests: 1 host on this console, 2 join the first lobby heard
    const char *romfs;
} M;

// --- Identity ---------------------------------------------------------------------------

/** The persistent id the server knows this console by (a UUID, like the web's localStorage one). */
static const char *token(void) {
    if (M.token[0]) return M.token;
    const char *saved = settings_str("persistent_id", NULL);
    if (saved && strlen(saved) == 36) {
        snprintf(M.token, sizeof M.token, "%s", saved);
        return M.token;
    }
    unsigned char b[16];
#ifdef __SWITCH__
    randomGet(b, sizeof b);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, sizeof b, f) != sizeof b)
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() ^ SDL_GetPerformanceCounter() >> (i % 8));
    if (f) fclose(f);
#endif
    b[6] = (b[6] & 0x0f) | 0x40;  // version 4
    b[8] = (b[8] & 0x3f) | 0x80;  // variant
    snprintf(M.token, sizeof M.token, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
             b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    settings_set_str("persistent_id", M.token);
    settings_save();
    return M.token;
}

// --- Text input ---------------------------------------------------------------------------

static bool ask_text(const char *initial, const char *guide, bool numbers, char *out, size_t n) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    bool ok = false;
    char buf[128] = {0};
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        if (numbers) {
            swkbdConfigSetType(&kbd, SwkbdType_NumPad);
            swkbdConfigSetLeftOptionalSymbolKey(&kbd, ".");
            swkbdConfigSetRightOptionalSymbolKey(&kbd, ":");
        }
        swkbdConfigSetInitialText(&kbd, initial);
        swkbdConfigSetGuideText(&kbd, guide);
        swkbdConfigSetStringLenMax(&kbd, 40);
        if (R_SUCCEEDED(swkbdShow(&kbd, buf, sizeof buf)) && buf[0]) {
            snprintf(out, n, "%s", buf);
            ok = true;
        }
        swkbdClose(&kbd);
    }
    return ok;
#else
    (void)initial, (void)guide, (void)numbers, (void)out, (void)n;
    return false;
#endif
}

// --- Flow -----------------------------------------------------------------------------------

static void set_error(const char *msg) {
    snprintf(M.error, sizeof M.error, "%s", msg);
    M.st = ST_MENU;
}

static bool use_server(void) {
    M.error[0] = 0;
    if (!net_init()) {
        set_error(T("port.mp_no_network"));
        return false;
    }
    if (!net_parse_addr(M.server, M.host, sizeof M.host, &M.port, DEFAULT_PORT)) {
        set_error(T("port.mp_bad_address"));
        return false;
    }
    settings_set_str("server", M.server);
    settings_save();
    return true;
}

static void create_lobby(void) {
    if (!use_server()) return;
    M.http_id = http_request("POST", M.host, M.port, "/api/create_game", token(), "{}");
    M.st = ST_CREATING;
}

static void probe_worker(void) {
    char path[128];
    snprintf(path, sizeof path, "/w%d/api/game/%s/exists", M.probe, M.code);
    M.http_id = http_request("GET", M.host, M.port, path, NULL, NULL);
}

static void join_lobby(const char *code) {
    if (!use_server()) return;
    snprintf(M.code, sizeof M.code, "%s", code);
    M.probe = 0;
    M.st = ST_FINDING;
    probe_worker();
}

static void connect_lobby(const char *worker_path, const char *name) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "host", M.host);
    cJSON_AddNumberToObject(a, "port", M.port);
    cJSON_AddStringToObject(a, "workerPath", worker_path);
    cJSON_AddStringToObject(a, "gameID", M.code);
    cJSON_AddStringToObject(a, "token", token());
    cJSON_AddStringToObject(a, "name", name);
    char *args = cJSON_PrintUnformatted(a);
    engine_call("mpConnect", args, false);
    free(args);
    cJSON_Delete(a);
    cJSON_Delete(M.lobby);
    M.lobby = NULL;
    M.lobby_req = 0;
    M.lobby_at = 0;
    M.sent_valid = false;
    M.st = ST_LOBBY;
}

/** An LDN failure, with what usually fixes it. */
static void ldn_error(const char *detail) {
    char msg[256];
    snprintf(msg, sizeof msg, "%s (%s)", ldn_supported() ? T("port.mp_ldn_failed") : T("port.mp_ldn_console_only"), detail);
    set_error(msg);
}

/** Hosts a lobby on this console: its own game server, joined like any other. */
static void host_here(const char *name) {
    M.error[0] = 0;
    if (!net_init()) {
        set_error(T("port.mp_no_network"));
        return;
    }
    if (M.wireless) {
        M.scanning = false;
        const char *e = ldn_host_open(name);
        if (e) {
            ldn_error(e);
            return;
        }
    }
    int port = lan_host_start(M.romfs, token(), M.code, sizeof M.code);
    if (!port) {
        ldn_close();
        set_error(T("port.mp_host_failed"));
        return;
    }
    M.lan_host = true;
    snprintf(M.host, sizeof M.host, "127.0.0.1");
    M.port = port;
    char ip[48];
    net_local_ip(ip, sizeof ip);
    char addr[64];
    snprintf(addr, sizeof addr, "%s:%d", ip[0] ? ip : "?", port);
    const char *f = T(M.wireless ? "port.mp_ldn_hint" : "port.mp_switch_hint");
    const char *at = strstr(f, "{ip}");
    if (at) snprintf(M.lan_hint, sizeof M.lan_hint, "%.*s%s%s", (int)(at - f), f, addr, at + 4);
    else snprintf(M.lan_hint, sizeof M.lan_hint, "%s", f);
    printf("lobby %s on lan %s\n", M.code, addr);
    fflush(stdout);
    connect_lobby("w0", name);
}

/** Joins a lobby found on the console's local wireless: connect to its network, then to its server. */
static void join_ldn(const LdnGame *g, const char *name) {
    M.error[0] = 0;
    if (strcmp(g->version, OPENFRONT_VERSION)) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s (%s / %s)", T("port.mp_version_diff"), g->version, OPENFRONT_VERSION);
        set_error(msg);
        return;
    }
    char ip[48];
    const char *e = ldn_join(g->index, ip, sizeof ip);
    M.scanning = false;
    if (e) {
        ldn_error(e);
        return;
    }
    snprintf(M.host, sizeof M.host, "%s", ip);
    M.port = g->port;
    snprintf(M.code, sizeof M.code, "%s", g->id);
    connect_lobby("w0", name);
}

/** Joins a lobby another console announced. */
static void join_lan(const LanGame *g, const char *name) {
    M.error[0] = 0;
    if (strcmp(g->version, OPENFRONT_VERSION)) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s (%s / %s)", T("port.mp_version_diff"), g->version, OPENFRONT_VERSION);
        set_error(msg);
        return;
    }
    snprintf(M.host, sizeof M.host, "%s", g->ip);
    M.port = g->port;
    snprintf(M.code, sizeof M.code, "%s", g->id);
    connect_lobby("w0", name);
}

static void stop_hosting(void) {
    if (M.lan_host) lan_host_stop();
    M.lan_host = false;
    ldn_close();
    M.scanning = false;
}

static void leave_lobby(void) {
    engine_call("mpLeave", "{}", false);
    stop_hosting();
    cJSON_Delete(M.lobby);
    M.lobby = NULL;
    M.st = ST_MENU;
}

/** The Solo modal's options as OF.mpConfig takes them (engine.c's do_start fields). */
static void send_config(const StartOptions *o) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddNumberToObject(a, "map", o->map);
    cJSON_AddNumberToObject(a, "difficulty", o->difficulty);
    cJSON_AddNumberToObject(a, "bots", o->bots);
    cJSON_AddNumberToObject(a, "nations", o->nations);
    cJSON_AddBoolToObject(a, "instantBuild", o->instant_build);
    cJSON_AddBoolToObject(a, "infiniteGold", o->infinite_gold);
    cJSON_AddBoolToObject(a, "infiniteTroops", o->infinite_troops);
    cJSON_AddBoolToObject(a, "randomSpawn", o->random_spawn);
    cJSON_AddBoolToObject(a, "waterNukes", o->water_nukes);
    cJSON_AddNumberToObject(a, "gameMode", o->game_mode);
    cJSON_AddNumberToObject(a, "teamCount", o->team_count);
    cJSON_AddNumberToObject(a, "doomsdaySpeed", o->doomsday ? o->doomsday_speed : -1);
    if (o->max_timer_on) cJSON_AddNumberToObject(a, "maxTimer", o->max_timer);
    if (o->gold_mult_on) cJSON_AddNumberToObject(a, "goldMultiplier", o->gold_mult);
    if (o->starting_gold_on) cJSON_AddNumberToObject(a, "startingGold", o->starting_gold);
    if (o->alliance_on) cJSON_AddNumberToObject(a, "allianceMinutes", o->alliance_minutes);
    if (o->overtime_on) cJSON_AddNumberToObject(a, "overtime", o->overtime);
    cJSON_AddNumberToObject(a, "disabledUnits", o->disabled_units);
    char *args = cJSON_PrintUnformatted(a);
    engine_call("mpConfig", args, false);
    free(args);
    cJSON_Delete(a);
    M.sent = *o;
    M.sent_valid = true;
}

/** HTTP replies: the new lobby, or which worker hosts the one being joined. */
static void poll_http(const StartOptions *o) {
    int status = 0;
    char *body = NULL;
    if (!M.http_id || !http_result(M.http_id, &status, &body)) return;
    M.http_id = 0;
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (M.st == ST_CREATING) {
        const char *id = jstr(j, "gameID", NULL);
        const char *wp = jstr(j, "workerPath", "w0");
        if (status == 200 && id) {
            snprintf(M.code, sizeof M.code, "%s", id);
            printf("lobby %s on %s\n", M.code, wp);
            fflush(stdout);
            connect_lobby(wp, o->name);
        } else {
            set_error(status == 0 ? T("port.mp_no_server") : T("private_lobby.error"));
        }
    } else if (M.st == ST_FINDING) {
        if (status == 0) {
            set_error(T("port.mp_no_server"));
        } else if (jbool(j, "exists") && jstr(j, "switch", NULL) && strcmp(jstr(j, "switch", ""), OPENFRONT_VERSION)) {
            char msg[256];
            snprintf(msg, sizeof msg, "%s (%s / %s)", T("port.mp_version_diff"), jstr(j, "switch", ""), OPENFRONT_VERSION);
            set_error(msg);
        } else if (jbool(j, "exists")) {
            char wp[8];
            snprintf(wp, sizeof wp, "w%d", M.probe);
            connect_lobby(wp, o->name);
        } else if (++M.probe < MAX_WORKERS && j) {
            probe_worker();
        } else {
            set_error(T("private_lobby.not_found"));
        }
    }
    cJSON_Delete(j);
}

static void poll_lobby(void) {
    if (M.lobby_req) {
        char *r = engine_result(M.lobby_req);
        if (!r) return;
        M.lobby_req = 0;
        cJSON *j = cJSON_Parse(r);
        free(r);
        if (j) {
            cJSON_Delete(M.lobby);
            M.lobby = j;
        }
    }
    double now = app_now_ms();
    if (now - M.lobby_at > 200) {
        M.lobby_at = now;
        M.lobby_req = engine_call("mpLobby", "{}", true);
    }
}

// --- Drawing ----------------------------------------------------------------------------------

static void card_title(const char *text, float y) {
    char t[96];
    snprintf(t, sizeof t, "%s", text);
    gfx_upper(t);
    gfx_text(FONT_800, 26, t, PAGE_W / 2.0f, y, 32, hex(TW_WHITE), ALIGN_CENTER);
}

static void label(const char *text, float x, float y) {
    char l[96];
    snprintf(l, sizeof l, "%s", text);
    gfx_upper(l);
    gfx_text(FONT_700, 13, l, x, y, 18, hex(LABEL), ALIGN_LEFT);
}

/** Wraps `text` into lines of at most `w` and draws them; returns the height. */
static float paragraph(const char *text, float x, float y, float w, float size, SDL_Color c) {
    char line[256];
    const char *p = text;
    float h = 0;
    while (*p) {
        size_t best = 0, i = 0;
        while (p[i] && p[i] != '\n') {
            size_t j = i;
            while (p[j] && p[j] != ' ' && p[j] != '\n') j++;
            snprintf(line, sizeof line, "%.*s", (int)j, p);
            if (best && gfx_text_width(FONT_400, size, line) > w) break;
            best = j;
            i = p[j] == ' ' ? j + 1 : j;
        }
        if (!best) best = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)best, p);
        gfx_text(FONT_400, size, line, x, y + h, size * 1.4f, c, ALIGN_LEFT);
        h += size * 1.4f;
        p += best;
        while (*p == ' ' || *p == '\n') p++;
    }
    return h;
}

/** A lobby another console announced: host, map, players; joins on A / tap. */
static bool lan_row(int i, const LanGame *g, Rect r) {
    uint32_t id = ui_id("multi-lan", i);
    bool hover = ui_hover(id, r);
    bool other_version = strcmp(g->version, OPENFRONT_VERSION) != 0;
    gfx_box(r, 10, hex(hover ? 0x2a1834 : ITEM_BG), 1, hover ? alpha(hex(ORANGE), 0.6f) : hex(ITEM_BORDER));
    const MapList *ml = engine_maps();
    const char *map = g->map;
    for (int m = 0; m < ml->nmaps; m++)
        if (!strcmp(ml->map_folders[m], g->map)) map = ml->map_names[m];
    char line[256];
    snprintf(line, sizeof line, "%.63s · %.63s · %s: %d", g->host[0] ? g->host : g->ip, map, T("host_modal.players"), g->players);
    gfx_text(FONT_600, 16, line, r.x + 16, r.y + 8, 22, hex(TW_WHITE), ALIGN_LEFT);
    char sub[256];
    if (other_version) snprintf(sub, sizeof sub, "%s (%s)", T("port.mp_version_diff"), g->version);
    else if (g->started) snprintf(sub, sizeof sub, "%s", T("port.mp_in_progress"));
    else if (g->ip[0]) snprintf(sub, sizeof sub, "%.47s · %.15s", g->ip, g->id);
    else snprintf(sub, sizeof sub, "%.15s", g->id);
    gfx_text(FONT_400, 12, sub, r.x + 16, r.y + 30, 16, other_version ? hex(0xf87171) : hex(MUTED), ALIGN_LEFT);
    char join[48];
    snprintf(join, sizeof join, "%s", T("private_lobby.play"));
    gfx_upper(join);
    gfx_text(FONT_700, 15, join, r.x + r.w - 16, r.y + 15, 20, hex(ORANGE), ALIGN_RIGHT);
    return ui_button(id, r);
}

/** The Wi-Fi / local wireless switch above the menu. */
static void mode_tabs(float x, float y, float w, const char *name) {
    const char *labels[2] = {T("port.mp_wifi"), T("port.mp_local_wireless")};
    float tw = (w - 8) / 2;
    gfx_fill_rrect((Rect){x, y, w, 40}, 10, hex(ITEM_BG));
    for (int i = 0; i < 2; i++) {
        Rect r = {x + 4 + (tw)*(float)i, y + 4, tw, 32};
        uint32_t id = ui_id("multi-mode", i);
        bool on = M.wireless == (i == 1);
        bool hover = ui_hover(id, r);
        if (on) gfx_fill_rrect(r, 8, hex(ORANGE));
        else if (hover) gfx_fill_rrect(r, 8, hex(0x2a1834));
        char t[64];
        snprintf(t, sizeof t, "%s", labels[i]);
        gfx_upper(t);
        gfx_text(FONT_700, 14, t, r.x + r.w / 2, r.y + 7, 18, hex(TW_WHITE), ALIGN_CENTER);
        if (ui_button(id, r) && !on) {
            M.wireless = i == 1;
            M.error[0] = 0;
            if (!M.wireless) {
                ldn_close();
                M.scanning = false;
            }
        }
    }
    // The local wireless list needs a scanning station.
    if (M.wireless && !M.scanning && M.st == ST_MENU && !M.error[0]) {
        const char *e = ldn_scan_start(name);
        if (e) ldn_error(e);
        else M.scanning = true;
    }
}

static MultiResult draw_menu(const StartOptions *o, const Input *in) {
    MultiResult result = MULTI_NONE;
    home_backdrop();
    const float W = 600, X = PAGE_W / 2.0f - W / 2;
    gfx_box((Rect){X - 24, 100, W + 48, 528}, 14, alpha(hex(CARD_BG), 0.96f), 1, hex(CARD_BORDER));
    card_title(T("port.multiplayer_title"), 112);
    bool busy = M.st != ST_MENU;
    mode_tabs(X, 156, W, o->name);
    if (in->pressed[BTN_L] || in->pressed[BTN_R]) {
        M.wireless = !M.wireless;
        M.error[0] = 0;
        if (!M.wireless) ldn_close(), M.scanning = false;
    }

    // Host here: this console is the server.
    if ((home_button("multi-host", (Rect){X, 210, W, 56}, T("port.mp_host_here"), true, 20) || in->pressed[BTN_PLUS]) && !busy)
        host_here(o->name);

    // Lobbies other consoles announce (Wi-Fi broadcast or LDN scan).
    label(T("port.mp_lan_games"), X, 282);
    int max_rows = M.wireless ? 3 : 2;
    LanGame games[3];
    LdnGame lgames[3];
    int ng = 0;
    if (M.wireless) {
        ng = ldn_scan_list(lgames, max_rows);
        for (int i = 0; i < ng; i++) {
            LanGame g = {.port = lgames[i].port, .players = lgames[i].players, .started = lgames[i].started};
            snprintf(g.id, sizeof g.id, "%s", lgames[i].id);
            snprintf(g.host, sizeof g.host, "%s", lgames[i].host);
            snprintf(g.map, sizeof g.map, "%s", lgames[i].map);
            snprintf(g.version, sizeof g.version, "%s", lgames[i].version);
            games[i] = g;
        }
    } else {
        ng = lan_discover_list(games, max_rows);
    }
    float y = 304;
    for (int i = 0; i < ng; i++) {
        if (lan_row(i, &games[i], (Rect){X, y, W, 52}) && !busy) {
            if (M.wireless) join_ldn(&lgames[i], o->name);
            else join_lan(&games[i], o->name);
        }
        y += 58;
    }
    if (ng == 0) {
        gfx_box((Rect){X, y, W, 52}, 10, alpha(hex(ITEM_BG), 0.5f), 1, hex(ITEM_BORDER));
        gfx_text(FONT_400, 14, T(M.wireless ? "port.mp_searching_ldn" : "port.mp_searching"), PAGE_W / 2.0f, y + 16, 20,
                 hex(MUTED), ALIGN_CENTER);
        y += 58;
    }
    // Tests (multi_auto with "lan"): host, or join the first lobby heard.
    if (M.auto_lan == 1 && !busy) M.auto_lan = 0, host_here(o->name);
    if (M.auto_lan == 2 && ng > 0 && !busy && !M.wireless) M.auto_lan = 0, join_lan(&games[0], o->name);
    paragraph(T(M.wireless ? "port.mp_ldn_help" : "port.mp_same_wifi"), X, y + 2, W, 13, hex(MUTED));

    if (!M.wireless) {
        // A PC running OpenFront's server (with browsers).
        label(T("port.mp_pc_server"), X, 452);
        const float bw = 112;
        Rect sr = {X, 476, W - 2 * (bw + 12), 48};
        uint32_t sid = ui_id("multi-server", 0);
        bool sh = ui_hover(sid, sr);
        gfx_box(sr, 10, hex(sh ? 0x2a1834 : ITEM_BG), 1, sh ? alpha(hex(ORANGE), 0.6f) : hex(ITEM_BORDER));
        gfx_text(FONT_600, 18, M.server[0] ? M.server : "192.168.1.?:9000", sr.x + 16, sr.y + 12, 24,
                 M.server[0] ? hex(TW_WHITE) : hex(MUTED), ALIGN_LEFT);
        if (ui_button(sid, sr) || in->pressed[BTN_Y]) ask_text(M.server, T("port.mp_server"), true, M.server, sizeof M.server);
        if (home_button("multi-create", (Rect){sr.x + sr.w + 12, 476, bw, 48}, T("port.mp_create_short"), false, 15) && !busy)
            create_lobby();
        if (home_button("multi-join", (Rect){sr.x + sr.w + 24 + bw, 476, bw, 48}, T("port.mp_join_short"), false, 15) &&
            !busy) {
            char code[32] = "";
            if (ask_text("", T("private_lobby.enter_id"), false, code, sizeof code)) join_lobby(code);
        }
    }

    if (busy) gfx_text(FONT_600, 15, T("port.mp_connecting"), PAGE_W / 2.0f, 534, 20, hex(ORANGE), ALIGN_CENTER);
    else if (M.error[0]) paragraph(M.error, X, 530, W, 13, hex(0xf87171));
    if (home_button("multi-back", (Rect){X, 572, W, 44}, T("common.back"), false, 15)) result = MULTI_BACK;
    if (in->pressed[BTN_B] && !busy) result = MULTI_BACK;
    if (result == MULTI_BACK) {
        lan_discover_stop();
        ldn_close();
        M.scanning = false;
    }
    return result;
}

/** JoinLobbyModal after joining: the lobby's players and settings, waiting for the host. */
static void draw_waiting(const Input *in) {
    home_backdrop();
    const float W = 600, X = PAGE_W / 2.0f - W / 2;
    gfx_box((Rect){X - 24, 112, W + 48, 508}, 14, alpha(hex(CARD_BG), 0.96f), 1, hex(CARD_BORDER));
    card_title(T("private_lobby.title"), 132);
    const cJSON *l = M.lobby;
    const char *phase = jstr(l, "phase", "connecting");

    label(T("port.mp_code"), X, 190);
    gfx_text(FONT_800, 24, M.code, X + W, 184, 30, hex(ORANGE), ALIGN_RIGHT);
    float y = 228;
    if (!strcmp(phase, "connecting")) {
        gfx_text(FONT_600, 16, T("port.mp_connecting"), PAGE_W / 2.0f, y, 22, hex(ORANGE), ALIGN_CENTER);
    } else {
        // Map, mode, difficulty, tribes.
        char info[256], bots[64];
        const char *mode = jstr(l, "mode", "");
        snprintf(bots, sizeof bots, "%s", T("game_settings.bots"));
        size_t bl = strlen(bots);
        if (bl && bots[bl - 1] == ':') bots[bl - 1] = 0;  // "Tribus:" in some languages
        snprintf(info, sizeof info, "%s · %s · %s · %s: %d", jstr(l, "map", ""),
                 !strcmp(mode, "Team") ? T("game_mode.teams") : T("game_mode.ffa"), jstr(l, "difficulty", ""), bots,
                 (int)jnum(l, "bots", 0));
        gfx_box((Rect){X, y, W, 44}, 10, hex(ITEM_BG), 1, hex(ITEM_BORDER));
        gfx_text(FONT_600, 15, info, X + 16, y + 12, 20, hex(TW_WHITE), ALIGN_LEFT);
        y += 60;
        const cJSON *players = jget(l, "players");
        char title[96];
        snprintf(title, sizeof title, "%s (%d)", T("host_modal.players"), jlen(players));
        label(title, X, y);
        y += 26;
        float cx = X;
        for (int i = 0; i < jlen(players); i++) {
            const cJSON *pl = jat(players, i);
            char name[96];
            if (jbool(pl, "host")) snprintf(name, sizeof name, "%s · %s", jstr(pl, "name", "?"), T("host_modal.host_badge"));
            else snprintf(name, sizeof name, "%s", jstr(pl, "name", "?"));
            float pw = gfx_text_width(FONT_600, 14, name) + 28;
            if (cx > X && cx + pw > X + W) cx = X, y += 40;
            bool host = jbool(pl, "host");
            gfx_box((Rect){cx, y, pw, 32}, 8, hex(host ? 0x31151c : ITEM_BG), 1, hex(host ? ORANGE : ITEM_BORDER));
            gfx_text(FONT_600, 14, name, cx + 14, y + 7, 18, hex(TW_WHITE), ALIGN_LEFT);
            cx += pw + 10;
        }
        y += 56;
        const char *status = jbool(l, "starting") ? T("game_starting_modal.title") : T("private_lobby.joined_waiting");
        gfx_text(FONT_600, 16, status, PAGE_W / 2.0f, y, 22, hex(0x4ade80), ALIGN_CENTER);
        y += 34;
        if (!jbool(l, "compact")) y += paragraph(T("port.mp_need_compact"), X, y, W, 15, hex(0xfbbf24));
    }
    if (home_button("multi-leave", (Rect){X, 548, W, 52}, T("port.mp_leave"), false, 16) || in->pressed[BTN_B]) leave_lobby();
}

MultiResult multi_draw(StartOptions *o, const Input *in) {
    poll_http(o);
    if (M.st != ST_LOBBY) return draw_menu(o, in);

    poll_lobby();
    const char *phase = jstr(M.lobby, "phase", "connecting");
    if (!strcmp(phase, "closed")) {
        set_error(jstr(M.lobby, "error", T("port.mp_closed")));
        engine_call("mpLeave", "{}", false);
        stop_hosting();
        return draw_menu(o, in);
    }
    if (!strcmp(phase, "loading") || !strcmp(phase, "game")) {
        M.in_game = true;
        lan_discover_stop();
        return MULTI_GAME;
    }
    M.is_host = jbool(M.lobby, "isHost");
    if (!M.is_host) {
        draw_waiting(in);
        return MULTI_NONE;
    }

    // The host: the Solo screen's settings, sent to the server as they change.
    if (!strcmp(phase, "lobby")) {
        double now = app_now_ms();
        if (!M.sent_valid) send_config(o);
        else if (memcmp(&M.sent, o, sizeof *o)) {
            if (M.changed_at == 0) M.changed_at = now;
            if (now - M.changed_at > 250) send_config(o), M.changed_at = 0;
        }
    }
    SoloLobby lobby = {.code = M.code, .hint = M.lan_host ? M.lan_hint : NULL, .starting = jbool(M.lobby, "starting")};
    const cJSON *players = jget(M.lobby, "players");
    for (int i = 0; i < jlen(players) && lobby.nplayers < 64; i++) {
        const cJSON *pl = jat(players, i);
        if (jbool(pl, "host")) {
            memmove(&lobby.players[1], &lobby.players[0], sizeof(char *) * (size_t)lobby.nplayers);
            lobby.players[0] = jstr(pl, "name", "?");
        } else {
            lobby.players[lobby.nplayers] = jstr(pl, "name", "?");
        }
        lobby.nplayers++;
    }
    int speed = 1;
    SoloResult r = solo_draw(o, &speed, in, &lobby);
    if (M.auto_start && lobby.nplayers >= M.auto_start && !strcmp(phase, "lobby")) r = SOLO_START, M.auto_start = 0;
    if (r == SOLO_START && !lobby.starting) {
        send_config(o);
        engine_call("mpStartTimer", "{}", false);
    }
    if (r == SOLO_QUIT || in->pressed[BTN_B]) leave_lobby();
    return MULTI_NONE;
}

void multi_init(const char *romfs) { M.romfs = romfs; }

void multi_open(void) {
    if (!M.server[0]) snprintf(M.server, sizeof M.server, "%s", settings_str("server", ""));
    if (net_init()) lan_discover_start();
    M.st = ST_MENU;
    M.error[0] = 0;
}

bool multi_in_game(void) { return M.in_game; }

bool multi_is_host(void) { return M.is_host; }

void multi_game_over(void) {
    M.in_game = false;
    stop_hosting();
    if (net_init()) lan_discover_start();
    cJSON_Delete(M.lobby);
    M.lobby = NULL;
    M.st = ST_MENU;
}

void multi_auto(const char *server, const char *code, int start_players) {
    multi_open();
    M.auto_start = start_players;
    if (!strcmp(server, "lan")) {
        M.auto_lan = code ? 2 : 1;
        return;
    }
    snprintf(M.server, sizeof M.server, "%s", server);
    if (code) join_lobby(code);
    else create_lobby();
}
