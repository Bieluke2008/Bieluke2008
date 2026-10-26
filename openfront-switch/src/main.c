// OpenFront for Nintendo Switch: menus, HUD and the main loop.
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "engine.h"
#include "input.h"
#include "render.h"
#include "text.h"
#include "units.h"

#define SW 1280
#define SH 720

static const SDL_Color WHITE = {255, 255, 255, 255};
static const SDL_Color GREY = {150, 150, 160, 255};
static const SDL_Color DARK = {15, 18, 28, 255};
static const SDL_Color ACCENT = {80, 160, 255, 255};
static const SDL_Color GOOD = {110, 220, 120, 255};
static const SDL_Color BAD = {255, 110, 100, 255};
static const SDL_Color GOLD = {250, 210, 80, 255};

// Every string the UI shows; engine_t translates only these.
static const char *const STRINGS[] = {
    "port.title", "port.map", "port.speed", "port.name", "port.start", "port.quit",
    "port.loading", "port.paused", "port.resume", "port.restart", "port.main_menu",
    "port.spawn_hint", "port.controls", "port.menu_controls", "port.alliance_hint",
    "port.attack_ratio", "port.spectating", "port.accept_all", "port.reject_all",
    "port.no_target", "port.engine_slow", "port.embargo", "port.credits", "port.tribes",
    "port.nations", "port.default", "port.build",
    "difficulty.difficulty", "difficulty.easy", "difficulty.medium", "difficulty.hard",
    "difficulty.impossible",
    "leaderboard.troops", "leaderboard.gold", "leaderboard.owned",
    "player_panel.send_alliance", "player_panel.break_alliance", "player_panel.target",
    "player_panel.send_troops", "player_panel.troops",
    "build_menu.not_enough_money", "common.loading", "common.back",
    NULL,
};

typedef enum { SCREEN_MENU, SCREEN_GAME } Screen;
typedef enum { OVERLAY_NONE, OVERLAY_PAUSE, OVERLAY_BUILD, OVERLAY_PLAYER } Overlay;

typedef struct {
    char text[160];
    int kind;
    Uint32 at;
} LogLine;

#define LOG_LINES 6

static struct {
    SDL_Renderer *ren;
    const char *romfs;
    Screen screen;
    Overlay overlay;
    Input in;
    bool quit;

    // Menu
    int menu_row;
    StartOptions opts;
    int speed_idx;
    SDL_Texture *preview;
    int preview_w, preview_h, preview_map;
    float repeat_t;

    // Game
    Camera cam;
    bool cam_ready;
    float cur_x, cur_y;  // cursor on screen
    int overlay_row;
    int menu_seq;
    MenuResult menu;
    LogLine log[LOG_LINES];
    bool paused;
    Uint32 last_frame;
    float fps;
} app;

static const double SPEEDS[] = {0.5, 1.0, 1.5, 2.0};
static const int NATION_STEPS[] = {-1, 0, 5, 10, 20, 40, 80};
#define N_NATION_STEPS 7

// --- Small drawing helpers ----------------------------------------------------

static void fill(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(app.ren, c.r, c.g, c.b, c.a);
    SDL_FRect r = {x, y, w, h};
    SDL_RenderFillRectF(app.ren, &r);
}

static void panel(float x, float y, float w, float h) {
    fill(x, y, w, h, (SDL_Color){10, 14, 24, 205});
    SDL_SetRenderDrawColor(app.ren, 60, 80, 120, 255);
    SDL_FRect r = {x, y, w, h};
    SDL_RenderDrawRectF(app.ren, &r);
}

static const char *T(const char *k) { return engine_t(k); }

// --- Main menu ----------------------------------------------------------------

static void load_preview(void) {
    const MapList *ml = engine_maps();
    if (app.preview_map == app.opts.map && app.preview) return;
    if (app.preview) SDL_DestroyTexture(app.preview);
    app.preview = NULL;
    if (ml->nmaps == 0) return;
    app.preview = render_map_preview(app.romfs, ml->map_folders[app.opts.map], &app.preview_w,
                                     &app.preview_h);
    app.preview_map = app.opts.map;
}

static void edit_name(void) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    char out[NAME_LEN] = {0};
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, app.opts.name);
        swkbdConfigSetStringLenMax(&kbd, 20);
        if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out)) && out[0]) {
            snprintf(app.opts.name, sizeof app.opts.name, "%s", out);
        }
        swkbdClose(&kbd);
    }
#endif
}

static void start_game(void) {
    Cmd c = {.type = CMD_START, .start = app.opts};
    engine_set_speed(SPEEDS[app.speed_idx]);
    engine_set_paused(false);
    render_reset();
    engine_send(&c);
    app.screen = SCREEN_GAME;
    app.overlay = OVERLAY_NONE;
    app.cam_ready = false;
    app.paused = false;
    memset(app.log, 0, sizeof app.log);
    app.cur_x = SW / 2;
    app.cur_y = SH / 2;
}

enum { ROW_MAP, ROW_DIFF, ROW_NATIONS, ROW_TRIBES, ROW_SPEED, ROW_NAME, ROW_START, ROW_QUIT, ROW_COUNT };

static int nation_idx(void) {
    for (int i = 0; i < N_NATION_STEPS; i++) {
        if (NATION_STEPS[i] == app.opts.nations) return i;
    }
    return 0;
}

/** Left/right on a menu row; held directions repeat. */
static int menu_dir(const Input *in, float dt) {
    int d = in->held[BTN_RIGHT] || in->lx > 0.6f ? 1 : in->held[BTN_LEFT] || in->lx < -0.6f ? -1 : 0;
    if (in->pressed[BTN_RIGHT]) return 1;
    if (in->pressed[BTN_LEFT]) return -1;
    if (d == 0) {
        app.repeat_t = 0;
        return 0;
    }
    app.repeat_t += dt;
    if (app.repeat_t > 0.35f) {
        app.repeat_t -= 0.08f;
        return d;
    }
    return 0;
}

static void menu_update(float dt) {
    const Input *in = &app.in;
    const MapList *ml = engine_maps();
    // The stick moves one row per push, like the d-pad.
    static float prev_ly;
    bool down = in->pressed[BTN_DOWN] || (in->ly > 0.6f && prev_ly <= 0.6f);
    bool up = in->pressed[BTN_UP] || (in->ly < -0.6f && prev_ly >= -0.6f);
    prev_ly = in->ly;
    if (down) app.menu_row = (app.menu_row + 1) % ROW_COUNT;
    if (up) app.menu_row = (app.menu_row + ROW_COUNT - 1) % ROW_COUNT;
    int d = menu_dir(in, dt);
    if (in->held[BTN_L] || in->held[BTN_R]) d *= 10;  // big steps
    switch (app.menu_row) {
        case ROW_MAP:
            if (d && ml->nmaps) app.opts.map = ((app.opts.map + d) % ml->nmaps + ml->nmaps) % ml->nmaps;
            break;
        case ROW_DIFF:
            if (d) app.opts.difficulty = (app.opts.difficulty + (d > 0 ? 1 : 3)) % 4;
            break;
        case ROW_NATIONS:
            if (d) {
                int i = nation_idx() + (d > 0 ? 1 : -1);
                if (i < 0) i = 0;
                if (i >= N_NATION_STEPS) i = N_NATION_STEPS - 1;
                app.opts.nations = NATION_STEPS[i];
            }
            break;
        case ROW_TRIBES:
            app.opts.bots += d > 0 ? 10 : d < 0 ? -10 : 0;
            if (app.opts.bots < 0) app.opts.bots = 0;
            if (app.opts.bots > 400) app.opts.bots = 400;
            break;
        case ROW_SPEED:
            if (d) app.speed_idx = (app.speed_idx + (d > 0 ? 1 : 3)) % 4;
            break;
        default:
            break;
    }
    if (in->pressed[BTN_A]) {
        if (app.menu_row == ROW_START) start_game();
        else if (app.menu_row == ROW_QUIT) app.quit = true;
        else if (app.menu_row == ROW_NAME) edit_name();
        else app.menu_row = ROW_START;
    }
    if (in->pressed[BTN_PLUS]) app.quit = true;
    // Touch: tap a row to select it, tap again to activate.
    if (in->tap) {
        int row = (int)((in->py - 170) / 52);
        if (in->px < 640 && row >= 0 && row < ROW_COUNT) {
            if (row == app.menu_row) {
                if (row == ROW_START) start_game();
                else if (row == ROW_QUIT) app.quit = true;
                else if (row == ROW_NAME) edit_name();
            }
            app.menu_row = row;
        }
    }
    load_preview();
}

static void menu_draw(void) {
    const MapList *ml = engine_maps();
    fill(0, 0, SW, SH, (SDL_Color){18, 24, 38, 255});
    text_draw("OpenFront", 60, 40, 64, WHITE, ALIGN_LEFT);
    text_draw(T("port.title"), 64, 110, 26, ACCENT, ALIGN_LEFT);

    static const char *diff_keys[] = {"difficulty.easy", "difficulty.medium", "difficulty.hard", "difficulty.impossible"};
    char vals[ROW_COUNT][100];
    snprintf(vals[ROW_MAP], 100, "%s", ml->nmaps ? ml->map_names[app.opts.map] : "-");
    snprintf(vals[ROW_DIFF], 100, "%s", T(diff_keys[app.opts.difficulty]));
    if (app.opts.nations < 0) snprintf(vals[ROW_NATIONS], 100, "%s", T("port.default"));
    else snprintf(vals[ROW_NATIONS], 100, "%d", app.opts.nations);
    snprintf(vals[ROW_TRIBES], 100, "%d", app.opts.bots);
    snprintf(vals[ROW_SPEED], 100, "%.1fx", SPEEDS[app.speed_idx]);
    snprintf(vals[ROW_NAME], 100, "%s", app.opts.name);
    vals[ROW_START][0] = vals[ROW_QUIT][0] = 0;
    const char *labels[ROW_COUNT] = {
        T("port.map"), T("difficulty.difficulty"), T("port.nations"), T("port.tribes"),
        T("port.speed"), T("port.name"), T("port.start"), T("port.quit"),
    };
    for (int i = 0; i < ROW_COUNT; i++) {
        float y = 170 + (float)i * 52;
        bool sel = i == app.menu_row;
        if (sel) fill(48, y - 4, 560, 46, (SDL_Color){40, 70, 120, 255});
        text_draw(labels[i], 64, y + 4, 28, sel ? WHITE : GREY, ALIGN_LEFT);
        if (vals[i][0]) {
            char v[128];
            if (sel && i != ROW_NAME) snprintf(v, sizeof v, "< %.100s >", vals[i]);
            else snprintf(v, sizeof v, "%.100s", vals[i]);
            text_draw(v, 592, y + 4, 28, sel ? GOLD : WHITE, ALIGN_RIGHT);
        }
    }
    // Map preview
    float bx = 660, by = 170, bw = 570, bh = 400;
    panel(bx, by, bw, bh);
    if (app.preview) {
        float s = fminf((bw - 20) / (float)app.preview_w, (bh - 20) / (float)app.preview_h);
        float w = (float)app.preview_w * s, h = (float)app.preview_h * s;
        SDL_FRect dst = {bx + (bw - w) / 2, by + (bh - h) / 2, w, h};
        SDL_RenderCopyF(app.ren, app.preview, NULL, &dst);
    }
    text_draw(T("port.menu_controls"), SW / 2, SH - 92, 22, GREY, ALIGN_CENTER);
    text_draw(T("port.credits"), SW / 2, SH - 52, 18, GREY, ALIGN_CENTER);
}

// --- Game ---------------------------------------------------------------------

static void log_push(const char *text, int kind) {
    memmove(&app.log[0], &app.log[1], sizeof(LogLine) * (LOG_LINES - 1));
    LogLine *l = &app.log[LOG_LINES - 1];
    snprintf(l->text, sizeof l->text, "%s", text);
    l->kind = kind;
    l->at = SDL_GetTicks();
}

static void cursor_tile(int *x, int *y) {
    float tx, ty;
    cam_screen_to_tile(&app.cam, SW, SH, app.cur_x, app.cur_y, &tx, &ty);
    *x = (int)floorf(tx);
    *y = (int)floorf(ty);
}

static void send_xy(CmdType t, int x, int y) {
    Cmd c = {.type = t, .a = x, .b = y};
    engine_send(&c);
}

static void open_menu(CmdType t, Overlay o) {
    int x, y;
    cursor_tile(&x, &y);
    app.menu_seq = engine_next_seq();
    Cmd c = {.type = t, .a = x, .b = y, .c = app.menu_seq};
    engine_send(&c);
    app.overlay = o;
    app.overlay_row = 0;
    memset(&app.menu, 0, sizeof app.menu);
}

static void zoom_at(float factor, float sx, float sy) {
    float tx, ty;
    cam_screen_to_tile(&app.cam, SW, SH, sx, sy, &tx, &ty);
    app.cam.zoom *= factor;
    // Keep the tile under (sx, sy) in place.
    app.cam.cx = tx - (sx - SW / 2) / app.cam.zoom;
    app.cam.cy = ty - (sy - SH / 2) / app.cam.zoom;
}

/** Player menu rows: which actions apply. Returns the count. */
static int player_rows(const MenuResult *m, int *actions, const char **labels) {
    int n = 0;
    if (m->can_ally) actions[n] = 0, labels[n++] = T("player_panel.send_alliance");
    if (m->can_break) actions[n] = 1, labels[n++] = T("player_panel.break_alliance");
    if (m->can_target) actions[n] = 2, labels[n++] = T("player_panel.target");
    if (m->can_embargo) actions[n] = 3, labels[n++] = T("port.embargo");
    if (m->can_donate) actions[n] = 4, labels[n++] = T("player_panel.send_troops");
    return n;
}

static void overlay_update(const Snapshot *s) {
    const Input *in = &app.in;
    int rows = 0;
    if (app.overlay == OVERLAY_PAUSE) rows = 5;
    if (app.overlay == OVERLAY_BUILD || app.overlay == OVERLAY_PLAYER) {
        engine_menu(&app.menu);
        if (app.menu.seq != app.menu_seq) return;  // still waiting
    }
    int actions[8];
    const char *labels[8];
    if (app.overlay == OVERLAY_BUILD) rows = app.menu.count;
    if (app.overlay == OVERLAY_PLAYER) rows = app.menu.player >= 0 ? player_rows(&app.menu, actions, labels) : 0;
    if (rows > 0) {
        if (in->pressed[BTN_DOWN]) app.overlay_row = (app.overlay_row + 1) % rows;
        if (in->pressed[BTN_UP]) app.overlay_row = (app.overlay_row + rows - 1) % rows;
    }
    if (in->pressed[BTN_B] || (app.overlay != OVERLAY_PAUSE && in->pressed[BTN_PLUS])) {
        app.overlay = OVERLAY_NONE;
        return;
    }
    if (!in->pressed[BTN_A] || rows == 0) {
        if (in->pressed[BTN_A]) app.overlay = OVERLAY_NONE;
        return;
    }
    int r = app.overlay_row;
    switch (app.overlay) {
        case OVERLAY_PAUSE: {
            Cmd c = {0};
            if (r == 0) {
                app.paused = false;
            } else if (r == 1 || r == 2) {
                c.type = CMD_ALLIANCES;
                c.a = r == 1;
                engine_send(&c);
                app.paused = false;
            } else if (r == 3) {
                start_game();
                return;
            } else {
                c.type = CMD_STOP;
                engine_send(&c);
                render_reset();
                app.screen = SCREEN_MENU;
            }
            engine_set_paused(app.paused);
            break;
        }
        case OVERLAY_BUILD: {
            MenuItem *it = &app.menu.items[r];
            if (!it->enabled) return;
            Cmd c = {.type = CMD_BUILD, .a = it->kind, .b = app.menu.x, .c = app.menu.y};
            engine_send(&c);
            break;
        }
        case OVERLAY_PLAYER: {
            Cmd c = {.type = CMD_PLAYER_ACTION, .a = app.menu.player, .b = actions[r]};
            engine_send(&c);
            break;
        }
        default:
            break;
    }
    (void)s;
    app.overlay = OVERLAY_NONE;
}

static void game_update(const Snapshot *s, float dt) {
    Input *in = &app.in;
    if (s->status != GAME_RUNNING) {
        if (s->status == GAME_FAILED && (in->pressed[BTN_A] || in->pressed[BTN_B] || in->pressed[BTN_PLUS])) {
            app.screen = SCREEN_MENU;
        }
        return;
    }
    if (!app.cam_ready && s->width > 0) {
        app.cam.cx = (float)s->width / 2;
        app.cam.cy = (float)s->height / 2;
        app.cam.zoom = fminf((float)SW / (float)s->width, (float)SH / (float)s->height);
        app.cam_ready = true;
    }

    if (app.overlay != OVERLAY_NONE) {
        overlay_update(s);
        return;
    }

    // Cursor: left stick, mouse, or the last tap.
    float speed = 700 * dt;
    app.cur_x += in->lx * fabsf(in->lx) * speed;
    app.cur_y += in->ly * fabsf(in->ly) * speed;
    if (in->pointer_moved || in->tap || in->secondary_tap) {
        app.cur_x = in->px;
        app.cur_y = in->py;
    }
    // Pushing the cursor against the screen edge pans.
    float edge = 40, pan = 600 * dt / app.cam.zoom;
    if (app.cur_x < edge && in->lx < 0) app.cam.cx -= pan * -in->lx;
    if (app.cur_x > SW - edge && in->lx > 0) app.cam.cx += pan * in->lx;
    if (app.cur_y < edge && in->ly < 0) app.cam.cy -= pan * -in->ly;
    if (app.cur_y > SH - edge && in->ly > 0) app.cam.cy += pan * in->ly;
    if (app.cur_x < 0) app.cur_x = 0;
    if (app.cur_y < 0) app.cur_y = 0;
    if (app.cur_x > SW - 1) app.cur_x = SW - 1;
    if (app.cur_y > SH - 1) app.cur_y = SH - 1;

    app.cam.cx += in->rx * 900 * dt / app.cam.zoom;
    app.cam.cy += in->ry * 900 * dt / app.cam.zoom;
    app.cam.cx -= in->pan_dx / app.cam.zoom;
    app.cam.cy -= in->pan_dy / app.cam.zoom;
    if (in->held[BTN_ZR]) zoom_at(powf(2.5f, dt), app.cur_x, app.cur_y);
    if (in->held[BTN_ZL]) zoom_at(powf(2.5f, -dt), app.cur_x, app.cur_y);
    if (in->zoom_steps != 0) zoom_at(powf(1.25f, in->zoom_steps), in->pinch_x, in->pinch_y);
    cam_clamp(&app.cam, s->width, s->height, SW, SH);

    int tx, ty;
    cursor_tile(&tx, &ty);
    if (in->pressed[BTN_A] || in->tap) send_xy(CMD_PRIMARY, tx, ty);
    if (in->pressed[BTN_B]) send_xy(CMD_BOAT, tx, ty);
    if (!s->spawn_phase) {
        if (in->pressed[BTN_X] || in->secondary_tap) open_menu(CMD_BUILD_MENU, OVERLAY_BUILD);
        if (in->pressed[BTN_Y]) open_menu(CMD_PLAYER_MENU, OVERLAY_PLAYER);
    }
    if (in->pressed[BTN_L] || in->pressed[BTN_R]) {
        double r = s->ratio + (in->pressed[BTN_R] ? 0.1 : -0.1);
        r = r < 0.01 ? 0.01 : r > 1 ? 1 : r;
        if (s->ratio < 0.1 && in->pressed[BTN_R]) r = 0.1;
        Cmd c = {.type = CMD_RATIO, .f = r};
        engine_send(&c);
    }
    if (in->pressed[BTN_DOWN]) {
        Cmd c = {.type = CMD_CANCEL_ATTACK};
        engine_send(&c);
    }
    if (in->pressed[BTN_UP] || in->pressed[BTN_RSTICK]) {
        // Centre on the largest label we own, else the map centre.
        for (int i = 0; i < s->nlabels; i++) {
            if (s->labels[i].id == s->me) {
                app.cam.cx = s->labels[i].x;
                app.cam.cy = s->labels[i].y;
                if (app.cam.zoom < 2) app.cam.zoom = 2;
            }
        }
    }
    if (in->pressed[BTN_MINUS] && s->pending_alliances > 0) {
        Cmd c = {.type = CMD_ALLIANCES, .a = 1};
        engine_send(&c);
    }
    if (in->pressed[BTN_PLUS]) {
        app.overlay = OVERLAY_PAUSE;
        app.overlay_row = 0;
        app.paused = true;
        engine_set_paused(true);
    }
}

static void draw_cursor(const Snapshot *s) {
    float x = app.cur_x, y = app.cur_y;
    // Highlight the tile under the cursor when zoomed in enough to see it.
    if (app.cam.zoom >= 4) {
        int tx, ty;
        cursor_tile(&tx, &ty);
        float sx, sy;
        cam_tile_to_screen(&app.cam, SW, SH, (float)tx, (float)ty, &sx, &sy);
        SDL_SetRenderDrawColor(app.ren, 255, 255, 255, 200);
        SDL_FRect r = {sx, sy, app.cam.zoom, app.cam.zoom};
        SDL_RenderDrawRectF(app.ren, &r);
    }
    fill(x - 12, y - 1.5f, 24, 3, DARK);
    fill(x - 1.5f, y - 12, 3, 24, DARK);
    fill(x - 10, y - 0.5f, 20, 1, WHITE);
    fill(x - 0.5f, y - 10, 1, 20, WHITE);
    (void)s;
}

static void draw_hud(const Snapshot *s) {
    char a[64], b[64], line[160];
    // Stats
    panel(12, 12, 330, 132);
    format_troops(s->troops, a, sizeof a);
    format_troops(s->max_troops, b, sizeof b);
    snprintf(line, sizeof line, "%s: %s / %s", T("leaderboard.troops"), a, b);
    text_draw(line, 24, 20, 22, WHITE, ALIGN_LEFT);
    float frac = s->max_troops > 0 ? (float)(s->troops / s->max_troops) : 0;
    fill(24, 48, 306, 6, (SDL_Color){40, 50, 70, 255});
    fill(24, 48, 306 * fminf(frac, 1), 6, ACCENT);
    format_number(s->gold, a, sizeof a);
    snprintf(line, sizeof line, "%s: %s", T("leaderboard.gold"), a);
    text_draw(line, 24, 60, 22, GOLD, ALIGN_LEFT);
    snprintf(line, sizeof line, "%s: %.2f%%", T("leaderboard.owned"), s->pct);
    text_draw(line, 24, 86, 22, WHITE, ALIGN_LEFT);
    snprintf(line, sizeof line, "%s: %d%%  (L/R)", T("port.attack_ratio"), (int)lround(s->ratio * 100));
    text_draw(line, 24, 112, 22, ACCENT, ALIGN_LEFT);

    // Leaderboard
    panel(SW - 312, 12, 300, 30 + 26 * (float)s->nleaders);
    int mins = (int)s->seconds / 60, secs = (int)s->seconds % 60;
    snprintf(line, sizeof line, "%02d:%02d   %s %.0f ms", mins, secs, T("port.engine_slow"), s->tick_ms);
    text_draw(line, SW - 300, 16, 18, GREY, ALIGN_LEFT);
    for (int i = 0; i < s->nleaders; i++) {
        const Leader *l = &s->leaders[i];
        SDL_Color c = l->id == s->me ? ACCENT : WHITE;
        snprintf(line, sizeof line, "%d. %.22s", i + 1, l->name);
        text_draw(line, SW - 300, 40 + 26 * (float)i, 20, c, ALIGN_LEFT);
        snprintf(line, sizeof line, "%.1f%%", l->pct);
        text_draw(line, SW - 24, 40 + 26 * (float)i, 20, c, ALIGN_RIGHT);
    }

    // Log
    Uint32 now = SDL_GetTicks();
    float y = SH - 70;
    for (int i = LOG_LINES - 1; i >= 0; i--) {
        LogLine *l = &app.log[i];
        if (!l->text[0] || now - l->at > 10000) continue;
        SDL_Color c = l->kind == 1 ? GOOD : l->kind == 2 ? BAD : l->kind == 3 ? GOLD : WHITE;
        float w = text_width(l->text, 20);
        fill(12, y - 2, w + 16, 26, (SDL_Color){10, 14, 24, 180});
        text_draw(l->text, 20, y, 20, c, ALIGN_LEFT);
        y -= 28;
    }

    // Banner
    const char *banner = NULL;
    SDL_Color bc = WHITE;
    if (s->winner[0]) banner = s->winner, bc = GOLD;
    else if (s->spawn_phase) banner = T("port.spawn_hint");
    else if (!s->alive && s->spawned) banner = T("port.spectating"), bc = BAD;
    else if (s->pending_alliances > 0) banner = T("port.alliance_hint"), bc = GOLD;
    if (banner) {
        float w = text_width(banner, 26);
        panel(SW / 2 - w / 2 - 20, 156, w + 40, 44);
        text_draw(banner, SW / 2, 162, 26, bc, ALIGN_CENTER);
    }
    text_draw_outlined(T("port.controls"), SW / 2, SH - 32, 18, WHITE, DARK, ALIGN_CENTER);
}

static void draw_list(const char *title, const char **rows, const bool *enabled, const char **right, int n) {
    float w = 520, h = 70 + 44 * (float)(n > 0 ? n : 1);
    float x = SW / 2 - w / 2, y = SH / 2 - h / 2;
    panel(x, y, w, h);
    text_draw(title, x + 20, y + 14, 26, ACCENT, ALIGN_LEFT);
    if (n == 0) {
        text_draw(T("port.no_target"), x + 20, y + 62, 22, GREY, ALIGN_LEFT);
        return;
    }
    for (int i = 0; i < n; i++) {
        float ry = y + 56 + 44 * (float)i;
        bool sel = i == app.overlay_row;
        if (sel) fill(x + 8, ry, w - 16, 40, (SDL_Color){40, 70, 120, 255});
        SDL_Color c = enabled && !enabled[i] ? GREY : WHITE;
        text_draw(rows[i], x + 22, ry + 6, 24, c, ALIGN_LEFT);
        if (right && right[i]) text_draw(right[i], x + w - 22, ry + 6, 24, enabled && !enabled[i] ? GREY : GOLD, ALIGN_RIGHT);
    }
}

static void draw_overlay(void) {
    if (app.overlay == OVERLAY_NONE) return;
    fill(0, 0, SW, SH, (SDL_Color){0, 0, 0, 90});
    if (app.overlay == OVERLAY_PAUSE) {
        const char *rows[] = {T("port.resume"), T("port.accept_all"), T("port.reject_all"), T("port.restart"),
                              T("port.main_menu")};
        draw_list(T("port.paused"), rows, NULL, NULL, 5);
        return;
    }
    if (app.menu.seq != app.menu_seq) {
        draw_list(T("common.loading"), NULL, NULL, NULL, 0);
        return;
    }
    if (app.overlay == OVERLAY_BUILD) {
        const char *rows[MAX_MENU], *right[MAX_MENU];
        bool en[MAX_MENU];
        static char costs[MAX_MENU][32];
        for (int i = 0; i < app.menu.count; i++) {
            rows[i] = app.menu.items[i].name;
            en[i] = app.menu.items[i].enabled;
            format_number(app.menu.items[i].cost, costs[i], sizeof costs[i]);
            right[i] = costs[i];
        }
        draw_list(T("port.build"), rows, en, right, app.menu.count);
    } else {
        int actions[8];
        const char *rows[8];
        int n = app.menu.player >= 0 ? player_rows(&app.menu, actions, rows) : 0;
        char title[96];
        char tr[32];
        format_troops(app.menu.player_troops, tr, sizeof tr);
        if (app.menu.player >= 0) snprintf(title, sizeof title, "%s  (%s)", app.menu.player_name, tr);
        else snprintf(title, sizeof title, "Y");
        draw_list(title, rows, NULL, NULL, n);
    }
}

static void game_draw(void) {
    fill(0, 0, SW, SH, (SDL_Color){0x3c, 0x3c, 0x3c, 255});
    const Snapshot *s = engine_lock();
    Msg m;
    while (engine_pop_message(&m)) log_push(m.text, m.kind);
    if (s->status == GAME_RUNNING) {
        render_update_map(s);
        render_map(s, &app.cam, SW, SH);
        draw_cursor(s);
        draw_hud(s);
    } else if (s->status == GAME_LOADING || s->status == GAME_IDLE) {
        text_draw(T("port.loading"), SW / 2, SH / 2 - 20, 36, WHITE, ALIGN_CENTER);
    } else if (s->status == GAME_FAILED) {
        text_draw("Error", SW / 2, SH / 2 - 60, 36, BAD, ALIGN_CENTER);
        text_draw(s->error, SW / 2, SH / 2, 20, WHITE, ALIGN_CENTER);
    }
    engine_unlock();
    draw_overlay();
}

// --- Platform -----------------------------------------------------------------

static const char *system_language(void) {
#ifdef __SWITCH__
    static const struct { SetLanguage l; const char *code; } langs[] = {
        {SetLanguage_JA, "ja"},     {SetLanguage_ENUS, "en"},  {SetLanguage_FR, "fr"},
        {SetLanguage_DE, "de"},     {SetLanguage_IT, "it"},    {SetLanguage_ES, "es"},
        {SetLanguage_ZHCN, "zh-CN"}, {SetLanguage_KO, "ko"},   {SetLanguage_NL, "nl"},
        {SetLanguage_PT, "pt-PT"},  {SetLanguage_RU, "ru"},    {SetLanguage_ZHTW, "zh-TW"},
        {SetLanguage_ENGB, "en"},   {SetLanguage_FRCA, "fr"},  {SetLanguage_ES419, "es"},
        {SetLanguage_ZHHANS, "zh-CN"}, {SetLanguage_ZHHANT, "zh-TW"}, {SetLanguage_PTBR, "pt-BR"},
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

// Desktop test mode: --autoplay MAP --frames N --screenshot FILE
static struct {
    const char *map;
    int frames;
    const char *shot;
    int ticks_seen;
} test;

static void autoplay(int frame, const Snapshot *s) {
    if (frame == 2) {
        const MapList *ml = engine_maps();
        for (int i = 0; i < ml->nmaps; i++) {
            if (strcmp(ml->map_folders[i], test.map) == 0) app.opts.map = i;
        }
        start_game();
    }
    if (s->status != GAME_RUNNING) return;
    if (s->spawn_phase && !s->spawned && s->tick > 2) {
        // Spawn on the nearest land to the map centre.
        for (int r = 0; r < s->width; r += 2) {
            int x = s->width / 2 + r, y = s->height / 2;
            if (x < s->width && (s->terrain[y * s->width + x] & 0x80) && !(s->state[y * s->width + x] & 0xfff)) {
                send_xy(CMD_PRIMARY, x, y);
                break;
            }
        }
    }
    // Open an overlay shortly before the screenshot, to check the menus.
    const char *ov = getenv("OPENFRONT_TEST_OVERLAY");
    if (ov && test.frames && frame == test.frames - 60 && !s->spawn_phase) {
        if (!strcmp(ov, "build")) open_menu(CMD_BUILD_MENU, OVERLAY_BUILD);
        if (!strcmp(ov, "player")) {
            // Point at the nearest other player's label.
            for (int i = 0; i < s->nlabels; i++) {
                if (s->labels[i].id == s->me) continue;
                cam_tile_to_screen(&app.cam, SW, SH, s->labels[i].x, s->labels[i].y, &app.cur_x, &app.cur_y);
                if (app.cur_x > 0 && app.cur_x < SW && app.cur_y > 0 && app.cur_y < SH) break;
            }
            open_menu(CMD_PLAYER_MENU, OVERLAY_PLAYER);
        }
        if (!strcmp(ov, "pause")) app.overlay = OVERLAY_PAUSE;
    }
    if (ov && test.frames && frame >= test.frames - 60) return;
    if (!s->spawn_phase && s->tick % 15 == 0 && s->tick != test.ticks_seen) {
        test.ticks_seen = s->tick;
        for (int i = 0; i < s->nlabels; i++) {
            if (s->labels[i].id != s->me) continue;
            float a = (float)s->tick * 0.7f;
            send_xy(CMD_PRIMARY, (int)(s->labels[i].x + cosf(a) * 30), (int)(s->labels[i].y + sinf(a) * 30));
            app.cam.cx = s->labels[i].x;
            app.cam.cy = s->labels[i].y;
            app.cam.zoom = 2.5f;
        }
    }
}

static void screenshot(const char *path) {
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, SW, SH, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(app.ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    printf("screenshot: %s\n", path);
}

int main(int argc, char **argv) {
#ifdef __SWITCH__
    romfsInit();
    plInitialize(PlServiceType_User);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);  // 1785 MHz for the JS engine
    app.romfs = "romfs:/";
#else
    app.romfs = "romfs/";
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--romfs") && i + 1 < argc) app.romfs = argv[++i];
        else if (!strcmp(argv[i], "--autoplay") && i + 1 < argc) test.map = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) test.frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) test.shot = argv[++i];
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
    SDL_Window *win = SDL_CreateWindow("OpenFront", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SW, SH, 0);
    app.ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.ren) app.ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(app.ren, SW, SH);
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);

    if (!text_init(app.ren, app.romfs)) {
        fprintf(stderr, "text_init failed (romfs at %s?)\n", app.romfs);
        return 1;
    }
    render_init(app.ren);
    input_init();

    // Splash while engine.js loads.
    SDL_SetRenderDrawColor(app.ren, 18, 24, 38, 255);
    SDL_RenderClear(app.ren);
    text_draw("OpenFront", SW / 2, SH / 2 - 50, 64, WHITE, ALIGN_CENTER);
    text_draw("…", SW / 2, SH / 2 + 30, 32, GREY, ALIGN_CENTER);
    SDL_RenderPresent(app.ren);

    if (!engine_init(app.romfs, system_language(), STRINGS)) {
        fprintf(stderr, "engine failed to load\n");
        return 1;
    }
    app.opts.map = 0;
    app.opts.difficulty = 1;
    app.opts.bots = 50;
    app.opts.nations = -1;
    app.speed_idx = 1;
    app.preview_map = -1;
    default_name(app.opts.name, sizeof app.opts.name);
    // Start on a small map that runs well on the Switch.
    const MapList *ml = engine_maps();
    for (int i = 0; i < ml->nmaps; i++) {
        if (strcmp(ml->map_folders[i], "britannia") == 0) app.opts.map = i;
    }

    app.last_frame = SDL_GetTicks();
    for (int frame = 0; !app.quit; frame++) {
        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - app.last_frame) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        app.last_frame = now;
        input_poll(&app.in, SW, SH);
        if (app.in.quit) break;

        if (test.map) {
            const Snapshot *s = engine_lock();
            Snapshot copy = *s;
            engine_unlock();
            autoplay(frame, &copy);
        }

        if (app.screen == SCREEN_MENU) {
            menu_update(dt);
            menu_draw();
        } else {
            const Snapshot *s = engine_lock();
            Snapshot copy = *s;
            engine_unlock();
            game_update(&copy, dt);
            game_draw();
        }
        if (test.frames && frame == test.frames - 1) {
            if (test.shot) screenshot(test.shot);
            break;
        }
        SDL_RenderPresent(app.ren);
        if (test.map && !test.frames) SDL_Delay(1);
    }

    engine_shutdown();
    input_shutdown();
    if (app.preview) SDL_DestroyTexture(app.preview);
    render_shutdown();
    text_shutdown();
    SDL_DestroyRenderer(app.ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
#ifdef __SWITCH__
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    plExit();
    romfsExit();
#endif
    return 0;
}
