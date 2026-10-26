// OpenFront for Nintendo Switch: the solo menu, the game screen and the
// main loop. The game screen is the web client's in-game page: the map,
// the HUD around it, the radial menu and the modals, driven by the touch
// screen, a controller (with a virtual mouse) or, on PC, mouse and keyboard.
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "app.h"
#include "clocks.h"
#include "engine.h"
#include "gfx.h"
#include "hud.h"
#include "input.h"
#include "json.h"
#include "map.h"
#include "menus.h"
#include "radial.h"
#include "solo.h"
#include "ui.h"
#include "units.h"

// Every string the native UI shows; engine_t translates only these.
static const char *const STRINGS[] = {
    // HUD
    "events_display.retreating", "help_modal.ui_wilderness",
    // Panels and modals
    "player_panel.gold", "player_panel.troops", "player_panel.betrayals", "player_panel.trading", "player_panel.active",
    "player_panel.stopped", "player_panel.alliances", "player_panel.chat", "player_panel.emotes", "player_panel.target",
    "player_panel.send_troops", "player_panel.send_gold", "player_panel.stop_trade", "player_panel.start_trade",
    "player_panel.break_alliance", "player_panel.send_alliance", "player_panel.stop_trade_enemies",
    "player_panel.start_trade_enemies", "player_panel.flip_rocket_trajectory", "player_panel.arc_up",
    "player_panel.arc_down", "player_panel.alliance_time_remaining", "player_panel.traitor", "common.none",
    "send_troops_modal.title_with_name", "send_gold_modal.title_with_name", "common.available", "common.preset_max",
    "common.send", "common.summary_keep", "common.cancel", "common.cap_label", "send_troops_modal.capacity_note",
    "common.confirm", "common.back", "common.loading",
    "user_setting.game_menu_title", "user_setting.open_settings_label", "user_setting.open_settings_desc",
    "user_setting.toggle_terrain", "user_setting.toggle_view_desc", "user_setting.on", "user_setting.off",
    "user_setting.modifiers", "user_setting.exit_game_label", "user_setting.exit_game_info",
    "help_modal.exit_confirmation", "win_modal.exit", "win_modal.keep", "win_modal.spectate",
    // Solo menu
    "main.solo", "map.map", "difficulty.difficulty", "difficulty.easy", "difficulty.medium", "difficulty.hard",
    "difficulty.impossible", "game_settings.bots", "game_settings.nations", "game_settings.options",
    "game_settings.start", "game_settings.instant_build", "game_settings.infinite_gold",
    "game_settings.infinite_troops", "game_settings.compact_map", "map_component.search_maps", "common.disabled",
    // The port's own
    "port.title", "port.name", "port.speed", "port.loading", "port.controls", "port.controls_title", "port.quit",
    "port.default", "port.credits",
    NULL,
};

App app;

static struct {
    SDL_Renderer *ren;
    const char *romfs;
    enum { SCREEN_SOLO, SCREEN_GAME } screen;
    Input in;
    bool quit;
    StartOptions opts;
    // Game screen
    Snapshot snap;           // render-thread copy
    Label *labels;
    int labels_cap;
    UnitRec *units;
    int units_cap;
    int frame_version;
    cJSON *frame_json;
    bool press_on_map;       // the current press started on the map (drags pan)
    int ghost_req;
    cJSON *ghost;
    double ghost_at;
    int ghost_tx, ghost_ty;
    int place_req;
    int locate_req;
    int warship_req;
    bool controller_cursor;
    float cursor_speed;
    Uint32 last_frame;
} G;

static const double SPEEDS[4] = {0.5, 1, 2, 4};

// --- App callbacks --------------------------------------------------------------------

const char *T(const char *key) { return engine_t(key); }

double app_now_ms(void) { return (double)SDL_GetTicks(); }

void app_set_paused(bool paused) {
    app.paused = paused;
    engine_set_paused(paused);
    engine_call("pause", paused ? "{\"paused\":true}" : "{\"paused\":false}", false);
}

void app_set_speed(int idx) {
    app.speed_idx = idx;
    engine_set_speed(SPEEDS[idx]);
}

void app_set_ghost(const char *type) {
    snprintf(app.ghost, sizeof app.ghost, "%s", type ? type : "");
    char args[64];
    if (app.ghost[0]) snprintf(args, sizeof args, "{\"type\":\"%s\"}", app.ghost);
    else snprintf(args, sizeof args, "{\"type\":null}");
    engine_call("setGhost", args, false);
    cJSON_Delete(G.ghost);
    G.ghost = NULL;
    G.ghost_req = 0;
    G.ghost_tx = G.ghost_ty = -1;
}

void app_open_modal(Modal m, int player) {
    app.modal = m;
    if (player >= 0) app.modal_player = player;
    if (m != MODAL_NONE) radial_close();
    menus_on_modal_opened();
}

void app_open_radial(float px, float py) {
    float tx, ty;
    cam_page_to_tile(&app.cam, px, py, &tx, &ty);
    if (tx < 0 || ty < 0 || tx >= (float)G.snap.width || ty >= (float)G.snap.height) return;
    radial_open((int)tx, (int)ty, px, py);
}

void app_focus_tile(float x, float y) {
    app.cam.cx = x;
    app.cam.cy = y;
    // GoToPlayerEvent zooms in a little when far out.
    if (app.cam.zoom < 1.5f) app.cam.zoom = 1.5f;
}

void app_focus_player(int sid) {
    if (sid < 0) return;
    char args[32];
    snprintf(args, sizeof args, "{\"sid\":%d}", sid);
    G.locate_req = engine_call("playerLocation", args, true);
}

void app_focus_unit(int id) {
    char args[32];
    snprintf(args, sizeof args, "{\"id\":%d}", id);
    G.locate_req = engine_call("unitLocation", args, true);
}

void app_toggle_controls_help(void) { app.help_visible = !app.help_visible; }

// --- Snapshot -------------------------------------------------------------------------------

/** Copies what the renderer reads from the engine's snapshot. */
static void take_snapshot(void) {
    const Snapshot *s = engine_lock();
    Label *labels = G.labels;
    UnitRec *units = G.units;
    G.snap = *s;
    if (s->nlabels > G.labels_cap) {
        G.labels_cap = s->nlabels * 2;
        labels = G.labels = realloc(G.labels, sizeof(Label) * (size_t)G.labels_cap);
    }
    if (s->nunits > G.units_cap) {
        G.units_cap = s->nunits * 2;
        units = G.units = realloc(G.units, sizeof(UnitRec) * (size_t)G.units_cap);
    }
    if (s->nlabels) memcpy(labels, s->labels, sizeof(Label) * (size_t)s->nlabels);
    if (s->nunits) memcpy(units, s->units, sizeof(UnitRec) * (size_t)s->nunits);
    G.snap.labels = labels;
    G.snap.units = units;
    char *frame = NULL;
    if (s->frame && s->frame_version != G.frame_version) {
        frame = strdup(s->frame);
        G.frame_version = s->frame_version;
    }
    G.snap.frame = NULL;
    engine_unlock();
    if (frame) {
        cJSON *f = cJSON_Parse(frame);
        free(frame);
        if (f) {
            cJSON_Delete(G.frame_json);
            G.frame_json = f;
        }
    }
    app.frame = G.frame_json;
    const cJSON *me = jget(app.frame, "me");
    app.my_sid = cJSON_IsObject(me) ? (int)jnum(me, "sid", 0) : 0;
    app.spawn_phase = jbool(app.frame, "spawn");
}

// --- Starting and leaving games ---------------------------------------------------------------

static void start_game(void) {
    engine_set_speed(SPEEDS[app.speed_idx]);
    engine_set_paused(false);
    map_reset();
    menus_reset();
    radial_close();
    cJSON_Delete(G.frame_json);
    G.frame_json = NULL;
    app.frame = NULL;
    G.frame_version = -1;
    engine_start(&G.opts);
    G.screen = SCREEN_GAME;
    app.cam_ready = false;
    app.paused = false;
    app.modal = MODAL_NONE;
    app.leaderboard_shown = false;
    app.leaderboard_auto = true;
    app.replay_panel = false;
    app.ghost[0] = 0;
    app.nwarships = 0;
    app.quit_to_menu = false;
    app.help_visible = false;
    app.cursor_x = PAGE_W / 2.0f;
    app.cursor_y = PAGE_H / 2.0f;
}

static void leave_game(void) {
    engine_stop();
    map_reset();
    menus_reset();
    radial_close();
    G.screen = SCREEN_SOLO;
}

// --- Input ----------------------------------------------------------------------------------------

/** The pointer the UI sees this frame, in page coordinates. */
static Pointer make_pointer(const Input *in) {
    float s = gfx_scale();
    Pointer p = {0};
    if (in->moved || in->pressed_primary || in->released_primary || in->secondary) {
        app.cursor_x = in->px / s;
        app.cursor_y = in->py / s;
        G.controller_cursor = false;
    }
    // The left stick drives a virtual mouse, faster the longer it is held.
    float mag = hypotf(in->lx, in->ly);
    if (mag > 0) {
        G.controller_cursor = true;
        G.cursor_speed = fminf(G.cursor_speed + 0.04f, 1);
        float v = (5 + 14 * G.cursor_speed) * mag;
        app.cursor_x = fminf(fmaxf(app.cursor_x + in->lx / mag * v, 0), PAGE_W - 1);
        app.cursor_y = fminf(fmaxf(app.cursor_y + in->ly / mag * v, 0), PAGE_H - 1);
    } else {
        G.cursor_speed = 0;
    }
    app.cursor_visible = G.controller_cursor;
    p.x = app.cursor_x;
    p.y = app.cursor_y;
    p.touch = in->touch;
    p.down = in->down || in->held[BTN_A];
    p.pressed = in->pressed_primary || in->pressed[BTN_A];
    p.released = in->released_primary;
    p.secondary = in->secondary || in->pressed[BTN_X];
    p.wheel = in->wheel;
    return p;
}

static bool a_was_held;

static void map_controls(const Input *in, Pointer *p) {
    Camera *cam = &app.cam;
    float s = gfx_scale();
    // Drag to pan, from a press that began on the map.
    if (p->pressed) G.press_on_map = !ui_pointer_over_ui();
    if (G.press_on_map && in->dragging && (in->drag_dx || in->drag_dy)) {
        cam->cx -= in->drag_dx / s / cam->zoom;
        cam->cy -= in->drag_dy / s / cam->zoom;
    }
    // Right stick pans; ZL / ZR and the wheel / pinch zoom.
    if (in->rx || in->ry) {
        cam->cx += in->rx * 14 / cam->zoom;
        cam->cy += in->ry * 14 / cam->zoom;
    }
    if (in->zoom_steps != 0 && (in->touch || !ui_pointer_over_ui())) {
        cam_zoom_at(cam, powf(1.15f, in->zoom_steps), in->zoom_x / s, in->zoom_y / s);
    }
    if (in->held[BTN_ZR]) cam_zoom_at(cam, 1.04f, app.cursor_x, app.cursor_y);
    if (in->held[BTN_ZL]) cam_zoom_at(cam, 1 / 1.04f, app.cursor_x, app.cursor_y);
    cam_clamp(cam, G.snap.width, G.snap.height);
}

static void center_on_me(void) {
    if (app.my_sid > 0) app_focus_player(app.my_sid);
}

/** The hotbar entries in order, for the d-pad and the digit keys. */
static void hotbar_select(int dir, int index) {
    const cJSON *bar = jget(app.frame, "hotbar");
    int n = jlen(bar);
    if (n == 0) return;
    int cur = -1;
    for (int i = 0; i < n; i++) {
        if (jbool(jat(bar, i), "sel")) cur = i;
    }
    int next = index >= 0 ? index : cur < 0 ? (dir > 0 ? 0 : n - 1) : cur + dir;
    if (next < 0 || next >= n) {
        app_set_ghost("");
        return;
    }
    const cJSON *it = jat(bar, next);
    if (index >= 0 && cur == next) app_set_ghost("");
    else app_set_ghost(jstr(it, "type", ""));
}

static void handle_shortcuts(const Input *in) {
    if (in->pressed[BTN_B]) {
        if (app.help_visible) app.help_visible = false;
        else if (radial_back()) {
        } else if (menus_back()) {
        } else if (app.ghost[0]) app_set_ghost("");
        else if (app.nwarships) app.nwarships = 0;
    }
    if (app.modal != MODAL_NONE || menus_win_open()) {
        if (in->pressed[BTN_PLUS] && app.modal == MODAL_SETTINGS) app_open_modal(MODAL_NONE, -1);
        return;
    }
    if (in->pressed[BTN_PLUS]) app_open_modal(MODAL_SETTINGS, -1);
    if (in->pressed[BTN_MINUS]) app_set_paused(!app.paused);
    if (in->pressed[BTN_L] || in->pressed[BTN_R]) {
        engine_call("ratioStep", in->pressed[BTN_L] ? "{\"delta\":-10}" : "{\"delta\":10}", false);
    }
    if (in->pressed[BTN_LEFT]) hotbar_select(-1, -1);
    if (in->pressed[BTN_RIGHT]) hotbar_select(1, -1);
    if (in->pressed[BTN_UP]) app.leaderboard_shown = !app.leaderboard_shown;
    if (in->pressed[BTN_DOWN]) app_open_modal(MODAL_EMOJI, app.my_sid);
    if (in->pressed[BTN_RSTICK]) center_on_me();
    if (in->key_digit >= 0) hotbar_select(0, in->key_digit == 0 ? 9 : in->key_digit - 1);
    if (in->pressed[BTN_Y]) {
        // Boat attack to the tile under the cursor (the web's "B" key).
        float tx, ty;
        cam_page_to_tile(&app.cam, app.cursor_x, app.cursor_y, &tx, &ty);
        char args[64];
        snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", (int)tx, (int)ty);
        engine_call("boat", args, false);
    }
}

/** What a click on the map does: place the ghost, move warships, or attack. */
static void map_click(void) {
    float tx, ty;
    cam_page_to_tile(&app.cam, app.cursor_x, app.cursor_y, &tx, &ty);
    int x = (int)floorf(tx), y = (int)floorf(ty);
    if (x < 0 || y < 0 || x >= G.snap.width || y >= G.snap.height) return;
    char args[160];
    if (radial_is_open()) {
        radial_close();
        return;
    }
    if (app.ghost[0]) {
        snprintf(args, sizeof args, "{\"type\":\"%s\",\"x\":%d,\"y\":%d}", app.ghost, x, y);
        G.place_req = engine_call("placeGhost", args, true);
        return;
    }
    bool water = G.snap.terrain && !(G.snap.terrain[y * G.snap.width + x] & 0x80);
    if (app.nwarships > 0) {
        if (water) {
            int len = snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"ids\":[", x, y);
            for (int i = 0; i < app.nwarships && len < (int)sizeof args - 16; i++) {
                len += snprintf(args + len, sizeof args - (size_t)len, "%s%d", i ? "," : "", app.warships[i]);
            }
            snprintf(args + len, sizeof args - (size_t)len, "]}");
            engine_call("moveWarships", args, false);
        }
        app.nwarships = 0;
        return;
    }
    if (water) {
        snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"r\":%d}", x, y, (int)fmaxf(6, 14 / app.cam.zoom));
        G.warship_req = engine_call("warshipsAt", args, true);
        return;
    }
    snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", x, y);
    engine_call("click", args, false);
}

static void poll_results(void) {
    char *r;
    if (G.locate_req && (r = engine_result(G.locate_req))) {
        G.locate_req = 0;
        cJSON *j = cJSON_Parse(r);
        free(r);
        if (cJSON_IsObject(j)) app_focus_tile((float)jnum(j, "x", 0), (float)jnum(j, "y", 0));
        cJSON_Delete(j);
    }
    if (G.place_req && (r = engine_result(G.place_req))) {
        G.place_req = 0;
        // Nukes stay selected for the next shot; structures are done.
        if (strcmp(r, "true") != 0) app_set_ghost("");
        free(r);
    }
    if (G.warship_req && (r = engine_result(G.warship_req))) {
        G.warship_req = 0;
        cJSON *j = cJSON_Parse(r);
        free(r);
        app.nwarships = 0;
        for (int i = 0; i < jlen(j) && app.nwarships < 64; i++) app.warships[app.nwarships++] = (int)jat(j, i)->valuedouble;
        cJSON_Delete(j);
    }
    if (G.ghost_req && (r = engine_result(G.ghost_req))) {
        G.ghost_req = 0;
        cJSON_Delete(G.ghost);
        G.ghost = cJSON_Parse(r);
        free(r);
        if (G.ghost && cJSON_IsNull(G.ghost)) {
            cJSON_Delete(G.ghost);
            G.ghost = NULL;
        }
    }
}

static void update_ghost(void) {
    if (!app.ghost[0]) return;
    float tx, ty;
    cam_page_to_tile(&app.cam, app.cursor_x, app.cursor_y, &tx, &ty);
    int x = (int)floorf(tx), y = (int)floorf(ty);
    if ((x != G.ghost_tx || y != G.ghost_ty || app_now_ms() - G.ghost_at > 250) && !G.ghost_req) {
        char args[96];
        snprintf(args, sizeof args, "{\"type\":\"%s\",\"x\":%d,\"y\":%d}", app.ghost, x, y);
        G.ghost_req = engine_call("ghost", args, true);
        G.ghost_tx = x, G.ghost_ty = y;
        G.ghost_at = app_now_ms();
    }
}

// --- Drawing ----------------------------------------------------------------------------------------

static void draw_cursor(void) {
    if (!app.cursor_visible) return;
    float x = app.cursor_x, y = app.cursor_y;
    // A mouse arrow, like the desktop pointer the web UI expects.
    gfx_fill_triangle(x - 1, y - 2, x - 1, y + 21, x + 15, y + 15, rgba(0, 0, 0, 255));
    gfx_fill_triangle(x + 1, y + 2, x + 1, y + 17, x + 12, y + 13, rgba(255, 255, 255, 255));
}

static void draw_help(void) {
    if (!app.help_visible) return;
    Rect all = {0, 0, PAGE_W, PAGE_H};
    gfx_fill_rect(all, rgba(0, 0, 0, 153));
    if (ui_button(ui_id("help-bg", 0), all)) app.help_visible = false;
    const float W = 640, H = 470;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - H / 2, W, H};
    gfx_box(card, 8, hex(0x1d293d), 1, hex(TW_SLATE_600));
    ui_block(card);
    gfx_icon("SettingIconWhite", (Rect){card.x + 17, card.y + 19, 24, 24}, hex(TW_WHITE), ICON_NORMAL);
    gfx_text(FONT_600, 20, T("port.controls_title"), card.x + 49, card.y + 17, 28, hex(TW_WHITE), ALIGN_LEFT);
    gfx_fill_rect((Rect){card.x + 1, card.y + 61, W - 2, 1}, hex(TW_SLATE_600));
    // port.controls: "key|action" lines separated by \n.
    const char *text = T("port.controls");
    float y = card.y + 76;
    const char *line = text;
    while (*line && y < card.y + H - 24) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[256];
        snprintf(buf, sizeof buf, "%.*s", (int)len, line);
        char *bar = strchr(buf, '|');
        if (bar) {
            *bar = 0;
            gfx_text(FONT_600, 15, buf, card.x + 32, y, 24, hex(TW_SKY_300), ALIGN_LEFT);
            gfx_text(FONT_400, 15, bar + 1, card.x + 250, y, 24, hex(TW_WHITE), ALIGN_LEFT);
        } else {
            gfx_text(FONT_400, 15, buf, card.x + 32, y, 24, hex(TW_SLATE_400), ALIGN_LEFT);
        }
        y += 26;
        if (!end) break;
        line = end + 1;
    }
}

static void draw_loading(void) {
    SDL_SetRenderDrawColor(G.ren, 0x3c, 0x3c, 0x3c, 255);
    SDL_RenderClear(G.ren);
    const char *msg = G.snap.status == GAME_FAILED ? G.snap.error : T("port.loading");
    gfx_text(FONT_400, 20, msg, PAGE_W / 2.0f, PAGE_H / 2.0f - 14, 28, hex(TW_WHITE), ALIGN_CENTER);
}

static void game_frame(const Input *in) {
    take_snapshot();
    poll_results();
    radial_poll();
    menus_poll();
    if (G.snap.status != GAME_RUNNING) {
        Pointer p = make_pointer(in);
        ui_begin(&p);
        draw_loading();
        ui_end();
        if (G.snap.status == GAME_FAILED && (in->pressed[BTN_B] || in->pressed[BTN_A] || in->released_primary)) leave_game();
        return;
    }
    if (!app.cam_ready && G.snap.width > 0) {
        cam_fit(&app.cam, G.snap.width, G.snap.height);
        app.cam_ready = true;
    }

    Pointer p = make_pointer(in);
    // A pressed: track release for the controller's click.
    if (in->held[BTN_A]) a_was_held = true;
    else if (a_was_held) {
        a_was_held = false;
        p.released = true;
    }
    ui_begin(&p);
    map_controls(in, &p);

    // Hover: the territory under the cursor and its info card.
    float tx, ty;
    cam_page_to_tile(&app.cam, app.cursor_x, app.cursor_y, &tx, &ty);
    int hx = (int)floorf(tx), hy = (int)floorf(ty);
    bool on_map = hx >= 0 && hy >= 0 && hx < G.snap.width && hy < G.snap.height;
    int hover_owner = 0;
    if (on_map && !ui_pointer_over_ui() && G.snap.state && G.snap.terrain && (G.snap.terrain[hy * G.snap.width + hx] & 0x80)) {
        hover_owner = G.snap.state[hy * G.snap.width + hx] & 0x0fff;
    }
    menus_hover_tile(hx, hy, on_map && !ui_pointer_over_ui() && !radial_is_open());
    update_ghost();
    map_set_view(app.alt_view, app.my_sid);
    map_update(&G.snap, hover_owner);

    MapOverlay ov = {
        .my_sid = app.my_sid,
        .hover_owner = hover_owner,
        .spawn_phase = app.spawn_phase,
        .ghost = app.ghost[0] ? G.ghost : NULL,
        .ghost_type = app.ghost,
        .warships = app.warships,
        .nwarships = app.nwarships,
    };
    map_draw(&G.snap, &app.cam, &ov);
    hud_draw(app.frame);
    menus_draw_overlay();
    radial_draw();
    menus_draw_modals();
    draw_help();
    draw_cursor();

    // What the map gets: clicks and right clicks no widget took.
    if (ui_click_on_map() && app.modal == MODAL_NONE && !menus_win_open() && !app.help_visible) map_click();
    if (ui_secondary_on_map() && app.modal == MODAL_NONE && !menus_win_open() && !app.help_visible) {
        if (app.ghost[0]) app_set_ghost("");
        else app_open_radial(app.cursor_x, app.cursor_y);
    }
    ui_end();
    handle_shortcuts(in);
    if (app.quit_to_menu) leave_game();
}

// --- Platform ------------------------------------------------------------------------------------------

static const char *system_language(void) {
#ifdef __SWITCH__
    static const struct {
        SetLanguage l;
        const char *code;
    } langs[] = {
        {SetLanguage_ENUS, "en"}, {SetLanguage_ENGB, "en"}, {SetLanguage_ES, "es"}, {SetLanguage_ES419, "es"},
        {SetLanguage_FR, "fr"},   {SetLanguage_FRCA, "fr"}, {SetLanguage_DE, "de"}, {SetLanguage_IT, "it"},
        {SetLanguage_JA, "ja"},   {SetLanguage_KO, "ko"},   {SetLanguage_NL, "nl"}, {SetLanguage_PT, "pt-PT"},
        {SetLanguage_PTBR, "pt-BR"}, {SetLanguage_RU, "ru"}, {SetLanguage_ZHCN, "zh-CN"}, {SetLanguage_ZHHANS, "zh-CN"},
        {SetLanguage_ZHTW, "zh-TW"}, {SetLanguage_ZHHANT, "zh-TW"},
    };
    u64 code = 0;
    SetLanguage lang;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang))) {
            for (size_t i = 0; i < sizeof langs / sizeof langs[0]; i++) {
                if (langs[i].l == lang) {
                    setExit();
                    return langs[i].code;
                }
            }
        }
        setExit();
    }
    return "en";
#else
    const char *env = getenv("OPENFRONT_LANG");
    if (env) return env;
    env = getenv("LANG");
    if (env && strncmp(env, "es", 2) == 0) return "es";
    return "en";
#endif
}

static void default_name(char *out, size_t n) {
    snprintf(out, n, "Player");
#ifdef __SWITCH__
    AccountUid uid;
    AccountProfile profile;
    AccountProfileBase base;
    if (R_SUCCEEDED(accountInitialize(AccountServiceType_Application))) {
        if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
            if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base)) && base.nickname[0]) {
                snprintf(out, n, "%s", base.nickname);
            }
            accountProfileClose(&profile);
        }
        accountExit();
    }
#endif
}

// --- Desktop test mode ------------------------------------------------------------------------------------
// --autoplay MAP --frames N --screenshot FILE [--scene NAME]
// plays the map (spawns, attacks around), and before the screenshot opens
// the scene to check: radial, build, hover, panel, emoji, settings, exit,
// send, chat, ghost, help, front (attack troop labels), bars (structure
// progress bars).

static struct {
    const char *map;
    int frames;
    const char *shot;
    const char *scene;
    int last_tick;
    int scene_req;
} test;

static bool find_label(int sid, float *x, float *y) {
    for (int i = 0; i < G.snap.nlabels; i++) {
        if (G.snap.labels[i].id == sid) {
            *x = G.snap.labels[i].x, *y = G.snap.labels[i].y;
            return true;
        }
    }
    return false;
}

static int nearest_enemy(float *x, float *y) {
    float mx, my;
    if (!find_label(app.my_sid, &mx, &my)) return 0;
    float best = 1e9f;
    int sid = 0;
    for (int i = 0; i < G.snap.nlabels; i++) {
        const Label *l = &G.snap.labels[i];
        if (l->id == app.my_sid) continue;
        float d = hypotf(l->x - mx, l->y - my);
        if (d < best) best = d, sid = l->id, *x = l->x, *y = l->y;
    }
    return sid;
}

static void autoplay(int frame) {
    if (test.scene && !strcmp(test.scene, "menu")) return;
    if (frame == 2) {
        const MapList *ml = engine_maps();
        for (int i = 0; i < ml->nmaps; i++) {
            if (strcmp(ml->map_folders[i], test.map) == 0) G.opts.map = i;
        }
        G.opts.instant_build = G.opts.infinite_gold = test.scene && (!strcmp(test.scene, "build") || !strcmp(test.scene, "ghost"));
        if (getenv("OPENFRONT_BOTS")) G.opts.bots = atoi(getenv("OPENFRONT_BOTS"));
        if (getenv("OPENFRONT_NATIONS")) G.opts.nations = atoi(getenv("OPENFRONT_NATIONS"));
        start_game();
        // Benchmarks: run ticks back to back instead of 10 per second.
        if (getenv("OPENFRONT_TEST_SPEED")) engine_set_speed(atof(getenv("OPENFRONT_TEST_SPEED")));
    }
    if (G.snap.status != GAME_RUNNING || !app.frame) return;
    const cJSON *me = jget(app.frame, "me");
    // Benchmarks leave the human out, so the seeded game is fully repeatable.
    if (app.spawn_phase && !getenv("OPENFRONT_BENCH") && !jbool(me, "spawned") && G.snap.tick > 2 && G.snap.tick != test.last_tick) {
        test.last_tick = G.snap.tick;
        // Spawn on land near the map centre.
        for (int r = 0; r < G.snap.width; r += 2) {
            int x = G.snap.width / 2 + r, y = G.snap.height / 2;
            if (x < G.snap.width && (G.snap.terrain[y * G.snap.width + x] & 0x80) && !(G.snap.state[y * G.snap.width + x] & 0xfff)) {
                char args[64];
                snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", x, y);
                engine_call("click", args, false);
                break;
            }
        }
    }
    int scene_at = test.frames - 90;
    // Scenes that go one ring deeper: the build or attack submenu.
    if (test.scene && frame == scene_at + 40 && (!strcmp(test.scene, "build") || !strcmp(test.scene, "attack"))) {
        radial_pick(3);
    }
    if (test.scene && frame >= scene_at - 30) {
        float mx, my, ex = 0, ey = 0;
        if (frame == scene_at - 30 && find_label(app.my_sid, &mx, &my)) {
            app.cam.cx = mx, app.cam.cy = my, app.cam.zoom = 4;
        }
        // These follow what they show until the screenshot.
        if (!strcmp(test.scene, "front") && G.snap.nattacks > 0) {
            app.cam.cx = G.snap.attacks[0].x[0], app.cam.cy = G.snap.attacks[0].y[0], app.cam.zoom = 3;
        } else if (!strcmp(test.scene, "bars")) {
            for (int i = 0; i < G.snap.nunits; i++) {
                if (G.snap.units[i].progress >= 0) {
                    app.cam.cx = G.snap.units[i].x, app.cam.cy = G.snap.units[i].y, app.cam.zoom = 6;
                    break;
                }
            }
        }
        if (frame != scene_at) return;
        const char *s = test.scene;
        int enemy = nearest_enemy(&ex, &ey);
        if (!strcmp(s, "radial") || !strcmp(s, "build") || !strcmp(s, "hover") || !strcmp(s, "attack")) {
            bool own = !strcmp(s, "build");
            if (own) find_label(app.my_sid, &ex, &ey);
            if (enemy || own) {
                app.cam.cx = ex, app.cam.cy = ey;
                float px, py;
                cam_tile_to_page(&app.cam, ex, ey, &px, &py);
                app.cursor_x = px, app.cursor_y = py;
                if (strcmp(s, "hover") != 0) app_open_radial(px, py);
            }
        } else if (!strcmp(s, "panel") && enemy) app_open_modal(MODAL_PANEL, enemy);
        else if (!strcmp(s, "self")) app_open_modal(MODAL_PANEL, app.my_sid);
        else if (!strcmp(s, "emoji")) app_open_modal(MODAL_EMOJI, enemy);
        else if (!strcmp(s, "chat") && enemy) app_open_modal(MODAL_CHAT, enemy);
        else if (!strcmp(s, "settings")) app_open_modal(MODAL_SETTINGS, -1);
        else if (!strcmp(s, "exit")) app_open_modal(MODAL_EXIT, -1);
        else if (!strcmp(s, "help")) app.help_visible = true;

        else if (!strcmp(s, "send") && enemy) {
            app.send_troops = true;
            app_open_modal(MODAL_SEND, enemy);
        } else if (!strcmp(s, "ghost")) {
            app_set_ghost("City");
            app.cursor_x = PAGE_W / 2.0f + 40, app.cursor_y = PAGE_H / 2.0f;
        }
        return;
    }
    if (getenv("OPENFRONT_BENCH")) return;  // benchmarks: spawn only, the AI plays
    if (!app.spawn_phase && G.snap.tick % 15 == 0 && G.snap.tick != test.last_tick) {
        test.last_tick = G.snap.tick;
        float mx, my;
        if (find_label(app.my_sid, &mx, &my)) {
            float a = (float)G.snap.tick * 0.7f;
            char args[64];
            snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", (int)(mx + cosf(a) * 30), (int)(my + sinf(a) * 30));
            engine_call("click", args, false);
            app.cam.cx = mx, app.cam.cy = my, app.cam.zoom = 2.5f;
        }
    }
}

static void screenshot(const char *path) {
    int w, h;
    SDL_GetRendererOutputSize(G.ren, &w, &h);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(G.ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    printf("screenshot: %s\n", path);
}

int main(int argc, char **argv) {
#ifdef __SWITCH__
    romfsInit();
    plInitialize(PlServiceType_User);
    clocks_boost();
    G.romfs = "romfs:/";
    int win_w = 1920, win_h = 1080;
#else
    G.romfs = "romfs/";
    int win_w = 1280, win_h = 720;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--romfs") && i + 1 < argc) G.romfs = argv[++i];
        else if (!strcmp(argv[i], "--autoplay") && i + 1 < argc) test.map = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) test.frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) test.shot = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) test.scene = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) win_w = atoi(argv[++i]), win_h = atoi(argv[++i]);
    }

#ifdef __SWITCH__
    // Buttons and sticks come from libnx's pad API (input.c), not SDL.
    Uint32 subsystems = SDL_INIT_VIDEO | SDL_INIT_TIMER;
#else
    Uint32 subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER;
#endif
    if (SDL_Init(subsystems) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_Window *win = SDL_CreateWindow("OpenFront", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, 0);
    G.ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!G.ren) G.ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);

    if (!gfx_init(G.ren, G.romfs)) {
        fprintf(stderr, "gfx_init failed (romfs at %s?)\n", G.romfs);
        return 1;
    }
    input_init();

    // Splash while engine.js loads.
    gfx_begin_frame();
    SDL_SetRenderDrawColor(G.ren, 0x0a, 0x06, 0x0f, 255);
    SDL_RenderClear(G.ren);
    gfx_text(FONT_800, 56, "OPENFRONT", PAGE_W / 2.0f, PAGE_H / 2.0f - 50, 64, hex(0xff3d3d), ALIGN_CENTER);
    SDL_RenderPresent(G.ren);

    if (!engine_init(G.romfs, system_language(), STRINGS)) {
        fprintf(stderr, "engine failed to load\n");
        return 1;
    }
    map_init(G.romfs);
    // The emoji table feeds both the names on the map and the emoji modal.
    int emoji_req = engine_call("emojis", "{}", true);
    G.opts.map = 0;
    G.opts.difficulty = 1;
    G.opts.bots = 100;
    G.opts.nations = -1;
    app.speed_idx = 1;
    default_name(G.opts.name, sizeof G.opts.name);
    const MapList *ml = engine_maps();
    for (int i = 0; i < ml->nmaps; i++) {
        if (strcmp(ml->map_folders[i], "britannia") == 0) G.opts.map = i;
    }
    solo_init(G.romfs);

    G.last_frame = SDL_GetTicks();
    for (int frame = 0; !G.quit; frame++) {
        gfx_begin_frame();
        if (frame % 120 == 0) clocks_poll();
        int out_w, out_h;
        SDL_GetRendererOutputSize(G.ren, &out_w, &out_h);
        input_poll(&G.in, out_w, out_h);
        if (G.in.quit) break;
        if (emoji_req) {
            char *r = engine_result(emoji_req);
            if (r) {
                emoji_req = 0;
                cJSON *j = cJSON_Parse(r);
                map_set_emojis(jget(j, "table"));
                cJSON_Delete(j);
                free(r);
            }
        }
        if (test.map) {
            take_snapshot();
            autoplay(frame);
        }
        if (G.screen == SCREEN_SOLO) {
            Pointer p = make_pointer(&G.in);
            if (G.in.held[BTN_A]) a_was_held = true;
            else if (a_was_held) a_was_held = false, p.released = true;
            ui_begin(&p);
            SoloResult r = solo_draw(&G.opts, &app.speed_idx, &G.in);
            draw_cursor();
            ui_end();
            if (r == SOLO_START) start_game();
            if (r == SOLO_QUIT) G.quit = true;
        } else {
            game_frame(&G.in);
        }
        if (test.frames && frame == test.frames - 1) {
            if (test.shot) screenshot(test.shot);
            break;
        }
        SDL_RenderPresent(G.ren);
        if (test.map) SDL_Delay(16);
    }

    engine_shutdown();
    input_shutdown();
    map_shutdown();
    solo_shutdown();
    gfx_shutdown();
    SDL_DestroyRenderer(G.ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
#ifdef __SWITCH__
    clocks_restore();
    plExit();
    romfsExit();
#endif
    return 0;
}
