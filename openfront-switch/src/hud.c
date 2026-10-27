// The web client's always-on HUD, laid out with the geometry of the real
// thing at 1280x720 (dumped from the browser): control-panel and
// unit-display (bottom), player-stats (top left), game-right-sidebar and
// replay-panel (top right), heads-up-message, attacks-display,
// events-display and actionable-events.
#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "json.h"
#include "ui.h"

#define HUD_X 390.0f
#define HUD_W 500.0f

static SDL_Color panel_bg(void) { return alpha(hex(TW_GRAY_800), 0.92f); }

/** An icon drawn white whatever its colours (brightness(0) invert(1)). */
static void white_icon(const char *name, Rect r, float a) { gfx_icon(name, r, rgba(255, 255, 255, (int)(255 * a)), ICON_SILHOUETTE); }

static void tooltip_box(Rect r) {
    gfx_shadow(r, 4, 8, rgba(0, 0, 0, 60));
    gfx_fill_rrect(r, 4, alpha(hex(TW_GRAY_800), 0.9f));
}

// --- control-panel ------------------------------------------------------------

/** The troop bar shared by the control panel and the hover card. */
void hud_troop_bar(Rect r, float troop_frac, float attack_frac, SDL_Color fill, SDL_Color attack_fill) {
    gfx_fill_rrect(r, 6, alpha(hex(TW_GRAY_900), 0.6f));
    Rect in = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
    float g = fmaxf(0, fminf(1, troop_frac));
    float o = fmaxf(0, fminf(1 - g, attack_frac));
    gfx_clip(&in);
    if (g > 0) gfx_fill_rrect4((Rect){in.x, in.y, in.w * g, in.h}, 5, 0, 0, 5, fill);
    if (o > 0) gfx_fill_rect((Rect){in.x + in.w * g, in.y, in.w * o, in.h}, attack_fill);
    gfx_clip(NULL);
    gfx_stroke_rrect(r, 6, 1, hex(TW_GRAY_600));
}

static void control_panel(const cJSON *cp) {
    const float x = HUD_X + 8, y = 625 + 4;
    bool up = jbool(cp, "rateUp");
    SDL_Color rate_c = hex(up ? TW_GREEN_400 : TW_ORANGE_400);
    // Row 1: troop rate | troop bar | gold
    Rect rate = {x, y, 88, 26};
    gfx_stroke_rrect(rate, 6, 1, rate_c);
    gfx_icon("SoldierIcon", (Rect){x + 5, y + 7, 13, 13}, rate_c, ICON_SILHOUETTE);
    gfx_text(FONT_700, 14, jstr(cp, "rate", ""), x + 22, y + 3, 20, rate_c, ALIGN_LEFT);

    Rect bar = {492, 630, 312, 24};
    hud_troop_bar(bar, (float)jnum(cp, "troopFrac", 0), (float)jnum(cp, "attackFrac", 0), hex(MALIBU_BLUE), hex(AQUARIUS));
    gfx_text_shadow(FONT_700, 18, jstr(cp, "troops", ""), 641, 631, 22, hex(TW_WHITE), ALIGN_RIGHT);
    gfx_text_shadow(FONT_700, 18, "/", 648.5f, 631, 22, hex(TW_WHITE), ALIGN_CENTER);
    float mw = gfx_text_shadow(FONT_700, 18, jstr(cp, "max", ""), 656, 631, 22, hex(TW_WHITE), ALIGN_LEFT);
    (void)mw;
    white_icon("SoldierIcon", (Rect){720, 631, 22, 22}, 1);

    Rect gold = {810, y, 72, 26};
    gfx_stroke_rrect(gold, 6, 1, hex(TW_YELLOW_400));
    gfx_icon("GoldCoinIcon", (Rect){815, y + 7, 13, 13}, hex(TW_WHITE), ICON_NORMAL);
    gfx_text(FONT_700, 14, jstr(cp, "gold", ""), 832, y + 3, 20, hex(TW_YELLOW_400), ALIGN_LEFT);
    const char *gain = jstr(cp, "goldGain", NULL);
    if (gain) {
        char g[48];
        snprintf(g, sizeof g, "+%s", gain);
        gfx_text_shadow(FONT_800, 14, g, gold.x + gold.w - 9, y - 20, 20, hex(TW_GREEN_400), ALIGN_RIGHT);
    }

    // Row 2: attack ratio | slider
    Rect ratio = {x, 659, 128, 26};
    gfx_stroke_rrect(ratio, 6, 1, hex(TW_GRAY_600));
    white_icon("SwordIcon", (Rect){x + 5, 666, 12, 12}, 1);
    gfx_text(FONT_700, 14, jstr(cp, "ratioLabel", ""), x + 21, 662, 20, hex(TW_WHITE), ALIGN_LEFT);

    // <input type=range min=1 max=100 class="accent-aquarius">
    Rect track = {532, 669, 350, 6};
    float v = (float)jnum(cp, "ratio", 0.2);
    float t = (v * 100 - 1) / 99;
    float thumb_x = track.x + 8 + (track.w - 16) * t;
    uint32_t id = ui_id("ratio-slider", 0);
    Rect hit = {track.x - 4, track.y - 10, track.w + 8, 26};
    bool hover = ui_hover(id, hit);
    static bool dragging;
    const Pointer *p = ui_pointer();
    if (hover && p->pressed) dragging = true;
    if (!p->down) dragging = false;
    if (dragging) {
        float nt = (p->x - track.x - 8) / (track.w - 16);
        nt = fmaxf(0, fminf(1, nt));
        int pct = (int)lroundf(1 + nt * 99);
        if (pct != (int)lroundf(v * 100)) {
            char args[48];
            snprintf(args, sizeof args, "{\"value\":%.2f}", pct / 100.0);
            engine_call("ratio", args, false);
        }
        thumb_x = track.x + 8 + (track.w - 16) * nt;
    }
    gfx_fill_rrect(track, 3, rgba(220, 220, 220, 255));
    gfx_fill_rrect((Rect){track.x, track.y, thumb_x - track.x, track.h}, 3, hex(AQUARIUS));
    gfx_fill_circle(thumb_x, track.y + 3, 8, hex(AQUARIUS));

    const cJSON *notif = jget(cp, "notif");
    if (cJSON_IsObject(notif)) {
        bool warn = strcmp(jstr(notif, "type", ""), "warning") == 0;
        SDL_Color c = hex(warn ? 0xffb86a : 0x8ec5ff);  // orange-300 / blue-300
        SDL_Color b = hex(warn ? TW_ORANGE_400 : TW_BLUE_400);
        char line[256];
        snprintf(line, sizeof line, "%s  %s", warn ? "⚠" : "ℹ", jstr(notif, "text", ""));
        float w = gfx_text_width(FONT_500, 12, line) + 14;
        Rect r = {x, 625 - 30, fminf(w, HUD_W - 16), 24};
        gfx_box(r, 6, alpha(b, 0.1f), 1, alpha(b, 0.6f));
        gfx_text(FONT_500, 12, line, r.x + 7, r.y + 4, 16, c, ALIGN_LEFT);
    }
}

// --- unit-display (hotbar) -----------------------------------------------------

static void hotbar(const cJSON *items) {
    int n = jlen(items);
    if (n == 0) return;
    gfx_fill_rect((Rect){HUD_X, 689, HUD_W, 1}, rgba(255, 255, 255, 26));
    // Item widths: border, hotkey, icon, count.
    float widths[16], total = 0;
    for (int i = 0; i < n && i < 16; i++) {
        const cJSON *it = jat(items, i);
        float w = 1 + 2 + 2 + gfx_text_width(FONT_400, 10, jstr(it, "hotkey", "")) + 2 + 20;
        const char *count = jstr(it, "count", NULL);
        if (count) w += 2 + gfx_text_width(FONT_400, 12, count);
        w += 2 + 1;
        widths[i] = roundf(w);
        total += widths[i] + (i ? 2 : 0);
    }
    float x = roundf(HUD_X + (HUD_W - total) / 2);
    for (int i = 0; i < n && i < 16; i++) {
        const cJSON *it = jat(items, i);
        Rect r = {x, 692, widths[i], 26};
        bool can = jbool(it, "can"), sel = jbool(it, "sel");
        uint32_t id = ui_id("hotbar", i);
        bool hover = ui_hover(id, r);
        if (ui_button(id, r)) {
            if (sel) app_set_ghost("");
            else if (can) app_set_ghost(jstr(it, "type", ""));
        }
        float a = can ? 1 : 0.4f;
        if (sel) gfx_fill_rrect(r, 4, alpha(hex(TW_SLATE_400), 0.2f * a));
        else if (hover) gfx_fill_rrect(r, 4, alpha(hex(TW_GRAY_800), a));
        gfx_stroke_rrect(r, 4, 1, alpha(hex(TW_SLATE_500), a));
        float cx = r.x + 5;
        cx += gfx_text(FONT_400, 10, jstr(it, "hotkey", ""), cx, r.y + 1, 15, alpha(hex(TW_GRAY_400), a), ALIGN_LEFT) + 2;
        gfx_icon(jstr(it, "icon", ""), (Rect){cx, r.y + 3, 20, 20}, rgba(255, 255, 255, (int)(255 * a)), ICON_NORMAL);
        cx += 22;
        const char *count = jstr(it, "count", NULL);
        if (count) gfx_text(FONT_400, 12, count, cx, r.y + 5, 16, alpha(hex(TW_WHITE), a), ALIGN_LEFT);
        if (hover) {
            // Tooltip above: name [key], description, cost.
            char title[128];
            snprintf(title, sizeof title, "%s [%s]", jstr(it, "name", ""), jstr(it, "hotkey", ""));
            const char *desc = jstr(it, "desc", "");
            float w = fmaxf(gfx_text_width(FONT_700, 14, title), gfx_text_width(FONT_400, 12, desc) + 16) + 8;
            const char *ws = jstr(it, "warship", NULL);
            float h = 4 + 20 + 4 + 8 + 16 + 8 + 20 + 4;
            Rect tr = {r.x + r.w / 2 - w / 2, r.y - 4 - h, w, h};
            tooltip_box(tr);
            gfx_text(FONT_700, 14, title, tr.x + w / 2, tr.y + 4, 20, hex(TW_GRAY_200), ALIGN_CENTER);
            gfx_text(FONT_400, 12, desc, tr.x + w / 2, tr.y + 36, 16, hex(TW_GRAY_200), ALIGN_CENTER);
            (void)ws;
            char cost[48];
            snprintf(cost, sizeof cost, "%s", jstr(it, "cost", "0"));
            float cw = 13 + 4 + gfx_text_width(FONT_400, 12, cost);
            float cx2 = tr.x + w / 2 - cw / 2;
            gfx_icon("GoldCoinIcon", (Rect){cx2, tr.y + h - 22, 13, 13}, hex(TW_WHITE), ICON_NORMAL);
            gfx_text(FONT_400, 12, cost, cx2 + 17, tr.y + h - 24, 16, hex(TW_YELLOW_300), ALIGN_LEFT);
        }
        x += widths[i] + 2;
    }
}

// --- player-stats (leaderboard) ------------------------------------------------

typedef struct {
    const char *id;
    float w;        // 0 = auto
    Align align;
    const char *icon;
} Column;

static void leaderboard(const cJSON *lb) {
    const float left = 8, top = 38;
    const cJSON *rows = jget(lb, "rows");
    const cJSON *pinned = jget(lb, "pinned");
    int nrows = jlen(rows);
    // Auto columns size to their widest cell (+ px-2 padding).
    float auto_w[3] = {0, 0, 0};
    const char *keys[3] = {"pct", "gold", "maxt"};
    for (int c = 0; c < 3; c++) {
        auto_w[c] = 36;
        for (int i = 0; i <= nrows; i++) {
            const cJSON *r = i < nrows ? jat(rows, i) : (cJSON_IsObject(pinned) ? pinned : NULL);
            if (!r) continue;
            float w = gfx_text_width(i < nrows ? FONT_400 : FONT_700, 14, jstr(r, keys[c], "")) + 8;
            if (w > auto_w[c]) auto_w[c] = w;
        }
    }
    Column cols[] = {
        {"rank", 34, ALIGN_CENTER, NULL}, {"clan", 80, ALIGN_CENTER, "GuildIconWhite"},
        {"player", 100, ALIGN_LEFT, "ProfileIcon"}, {"tiles", auto_w[0], ALIGN_RIGHT, "ClaimIcon"},
        {"gold", auto_w[1], ALIGN_RIGHT, "GoldCoinIcon"}, {"maxtroops", auto_w[2], ALIGN_RIGHT, "SoldierIcon"},
        {"picker", 32, ALIGN_CENTER, NULL},
    };
    int ncols = 7;
    float width = 0;
    for (int c = 0; c < ncols; c++) width += cols[c].w;
    width = fmaxf(width, 377);
    float extra = width - (34 + 80 + 100 + auto_w[0] + auto_w[1] + auto_w[2] + 32);
    // Spare width goes to the auto tracks.
    for (int c = 3; c <= 5; c++) cols[c].w += extra / 3;

    int shown_rows = nrows + (cJSON_IsObject(pinned) ? 1 : 0);
    Rect table = {left, top, width, 36 + 36 * (float)shown_rows};
    gfx_fill_rrect(table, 8, alpha(hex(TW_GRAY_800), 0.85f));
    ui_block(table);
    gfx_clip(&table);
    gfx_fill_rect((Rect){left, top, width, 36}, rgba(54, 65, 83, 242));
    const char *sort = jstr(lb, "sort", "tiles");
    bool asc = jbool(lb, "asc");
    float cx = left;
    for (int c = 0; c < ncols; c++) {
        Rect cell = {cx, top, cols[c].w, 36};
        if (c > 0) gfx_fill_rect((Rect){cx, top, 1, table.h}, alpha(hex(TW_SLATE_500), 1));
        if (strcmp(cols[c].id, "rank") == 0) {
            gfx_text(FONT_700, 14, "#", cell.x + cell.w / 2, top + 8, 20, hex(TW_WHITE), ALIGN_CENTER);
        } else if (strcmp(cols[c].id, "picker") == 0) {
            Rect b = {cell.x + 5, top + 10, 23, 16};
            gfx_box(b, 6, alpha(hex(TW_GRAY_700), 0.5f), 1, hex(TW_SLATE_500));
            gfx_icon("SettingIconWhite", (Rect){b.x + 5, b.y + 2, 12, 12}, hex(TW_WHITE), ICON_NORMAL);
        } else if (cols[c].icon) {
            bool sortable = c >= 3;
            bool sorted = sortable && strcmp(sort, cols[c].id) == 0;
            float iw = 18 + (sorted ? 15 : 0) + (c == 5 ? 13 : 0);
            float ix = cell.x + cell.w / 2 - iw / 2;
            bool white = c == 3 || c == 5;
            gfx_icon(cols[c].icon, (Rect){ix, top + 9, 18, 18}, hex(TW_WHITE), white ? ICON_SILHOUETTE : ICON_NORMAL);
            if (c == 5) white_icon("UpperLimitIcon", (Rect){ix + 16, top + 9, 13, 13}, 1);
            if (sorted) {
                gfx_text(FONT_700, 14, asc ? "↑" : "↓", ix + iw - 13, top + 8, 20, hex(TW_SKY_300), ALIGN_LEFT);
            }
            if (sortable && ui_button(ui_id("lb-sort", c), cell)) {
                char args[48];
                snprintf(args, sizeof args, "{\"sort\":\"%s\"}", cols[c].id);
                engine_call("leaderboard", args, false);
            }
        }
        cx += cols[c].w;
    }
    gfx_fill_rect((Rect){left, top + 35, width, 1}, hex(TW_SLATE_500));

    for (int i = 0; i < shown_rows; i++) {
        bool is_pinned = i == nrows;
        const cJSON *r = is_pinned ? pinned : jat(rows, i);
        float ry = top + 36 + 36 * (float)i;
        bool emph = jbool(r, "emph");
        Font f = is_pinned || emph ? FONT_700 : FONT_400;
        if (is_pinned) gfx_fill_rect((Rect){left, ry, width, 36}, rgba(54, 65, 83, 242));
        Rect rowr = {left, ry, width, 36};
        uint32_t id = ui_id("lb-row", (int)jnum(r, "sid", 0));
        if (ui_hover(id, rowr) && !is_pinned) gfx_fill_rect(rowr, rgba(255, 255, 255, 13));
        if (ui_button(id, rowr)) app_focus_player((int)jnum(r, "sid", 0));
        if (i < shown_rows - 1) gfx_fill_rect((Rect){left, ry + 35, width, 1}, hex(TW_SLATE_500));
        cx = left;
        char rank[16];
        snprintf(rank, sizeof rank, "%d", (int)jnum(r, "rank", 0));
        const char *vals[7] = {rank, jstr(r, "clan", ""), jstr(r, "name", ""), jstr(r, "pct", ""), jstr(r, "gold", ""), jstr(r, "maxt", ""), ""};
        for (int c = 0; c < ncols; c++) {
            char buf[96];
            const char *s = gfx_ellipsize(f, 14, vals[c], cols[c].w - 8, buf, sizeof buf);
            float tx = cols[c].align == ALIGN_LEFT ? cx + 5 : cols[c].align == ALIGN_RIGHT ? cx + cols[c].w - 4 : cx + cols[c].w / 2;
            gfx_text(f, 14, s, tx, ry + 8, 20, hex(TW_WHITE), cols[c].align);
            cx += cols[c].w;
        }
    }
    gfx_clip(NULL);
    // Mouse wheel / stick scroll over the list.
    if (in_rect(table, ui_pointer()->x, ui_pointer()->y) && ui_pointer()->wheel != 0) {
        char args[48];
        snprintf(args, sizeof args, "{\"scroll\":%d}", ui_pointer()->wheel > 0 ? -1 : 1);
        engine_call("leaderboard", args, false);
    }
}

static void left_sidebar(const cJSON *lb) {
    bool shown = app.leaderboard_shown && cJSON_IsObject(lb);
    float w = shown ? fmaxf(393, 0) : 42;
    float h = shown ? 262 : 42;
    if (shown) {
        // Height follows the table.
        int rows = jlen(jget(lb, "rows")) + (cJSON_IsObject(jget(lb, "pinned")) ? 1 : 0);
        h = 38 + 36 + 36 * (float)rows + 8;
    }
    Rect aside = {0, 0, w, h};
    gfx_fill_rrect4(aside, 0, 0, 8, 0, panel_bg());
    ui_block(aside);
    Rect btn = {8, 8, 26, 26};
    uint32_t id = ui_id("lb-toggle", 0);
    bool hover = ui_hover(id, btn);
    gfx_box(btn, 6, hover ? hex(TW_GRAY_600) : alpha(hex(TW_GRAY_700), 0.5f), 1, hex(TW_SLATE_500));
    gfx_icon(shown ? "LeaderboardIconSolidWhite" : "LeaderboardIconRegularWhite", (Rect){11, 11, 20, 20}, hex(TW_WHITE), ICON_NORMAL);
    if (ui_button(id, btn)) app.leaderboard_shown = !app.leaderboard_shown;
    if (shown) {
        gfx_text(FONT_400, 10, jstr(lb, "gameID", ""), w - 8, 8, 12, hex(TW_SLATE_500), ALIGN_RIGHT);
        leaderboard(lb);
    }
}

// --- game-right-sidebar + replay-panel ---------------------------------------------

static void right_sidebar(const cJSON *frame) {
    const char *timer = jstr(frame, "timer", "00:00");
    float tw = gfx_text_width(FONT_400, 16, timer);
    // timer | fast-forward | pause | settings | exit, gap-3, px-3 py-2. On a
    // server there is no fast-forward, and only the lobby's creator may pause.
    bool show[4] = {!app.multiplayer, !app.multiplayer || app.mp_host, true, true};
    float w = 12 + tw + 12;
    for (int i = 0; i < 4; i++) w += show[i] ? 32 : 0;
    Rect aside = {PAGE_W - w, 0, w, 40};
    gfx_shadow(aside, 8, 2, rgba(0, 0, 0, 30));
    gfx_fill_rrect4(aside, 0, 0, 0, 8, panel_bg());
    ui_block(aside);
    float x = aside.x + 12;
    gfx_text(FONT_400, 16, timer, x, 8, 24, hex(TW_WHITE), ALIGN_LEFT);
    x += tw + 12;
    struct {
        const char *icon;
        float h;
    } buttons[4] = {
        {"FastForwardIconSolidWhite", 13},
        {app.paused ? "PlayIconWhite" : "PauseIconWhite", 20},
        {"SettingIconWhite", 20},
        {"ExitIconWhite", 20},
    };
    for (int i = 0; i < 4; i++) {
        if (!show[i]) continue;
        Rect r = {x, 20 - buttons[i].h / 2, 20, buttons[i].h};
        Rect hit = {x - 6, 0, 32, 40};
        if (ui_button(ui_id("sidebar", i), hit)) {
            if (i == 0) app.replay_panel = !app.replay_panel;
            if (i == 1) app_set_paused(!app.paused);
            if (i == 2) app_open_modal(MODAL_SETTINGS, -1);
            if (i == 3) app_open_modal(MODAL_EXIT, -1);
        }
        gfx_icon(buttons[i].icon, r, hex(TW_WHITE), ICON_NORMAL);
        x += 32;
    }
    if (app.replay_panel && !app.multiplayer) {
        // ReplayPanel: game speed buttons ×0.5 ×1 ×2 max.
        static const char *labels[4] = {"×0.5", "×1", "×2", "max"};
        Rect panel = {PAGE_W - 8 - 216, 48, 216, 40};
        gfx_fill_rrect(panel, 8, panel_bg());
        ui_block(panel);
        for (int i = 0; i < 4; i++) {
            Rect b = {panel.x + 8 + 51 * (float)i, panel.y + 6, 47, 28};
            uint32_t id = ui_id("speed", i);
            bool hover = ui_hover(id, b);
            bool active = app.speed_idx == i;
            gfx_fill_rrect(b, 6, active ? hex(TW_BLUE_600) : hover ? hex(TW_GRAY_600) : hex(TW_GRAY_700));
            gfx_text(FONT_600, 14, labels[i], b.x + b.w / 2, b.y + 4, 20, hex(TW_WHITE), ALIGN_CENTER);
            if (ui_button(id, b)) app_set_speed(i);
        }
    }
}

// --- heads-up-message ----------------------------------------------------------

static void heads_up(const char *msg) {
    if (!msg || !*msg) return;
    float w = gfx_text_width(FONT_400, 20, msg) + 32;
    Rect r = {PAGE_W / 2.0f - w / 2, 108, w, 40};
    gfx_fill_rrect(r, 8, alpha(hex(TW_GRAY_800), 0.7f));
    gfx_text(FONT_400, 20, msg, PAGE_W / 2.0f, r.y + 6, 28, hex(TW_WHITE), ALIGN_CENTER);
}

// --- attacks-display -------------------------------------------------------------

static float attacks(const cJSON *list, float bottom) {
    int n = jlen(list);
    if (n == 0) return bottom;
    // grid-cols-2 gap-1, rows of ~28px, above the control panel (mb-1).
    int rows = (n + 1) / 2;
    if (rows > 4) rows = 4;
    float row_h = 28;
    float top = bottom - 4 - rows * row_h - (rows - 1) * 4;
    for (int i = 0; i < n && i < 8; i++) {
        const cJSON *a = jat(list, i);
        int col = i % 2, row = i / 2;
        Rect r = {HUD_X + col * (HUD_W / 2 + 2), top + row * (row_h + 4), HUD_W / 2 - 2, row_h};
        gfx_fill_rrect(r, 8, panel_bg());
        ui_block(r);
        const char *kind = jstr(a, "kind", "");
        bool incoming = strcmp(kind, "in") == 0 || strcmp(kind, "inBoat") == 0;
        bool boat = strcmp(kind, "boat") == 0 || strcmp(kind, "inBoat") == 0;
        SDL_Color c = incoming ? hex(TW_RED_400) : hex(AQUARIUS);
        float x = r.x + 6;
        if (boat) {
            gfx_icon("BoatIconWhite", (Rect){x, r.y + 4, 20, 20}, c, ICON_SILHOUETTE);
            x += 22;
            x += gfx_text(FONT_400, 16, jstr(a, "troops", ""), x + 48, r.y + 2, 24, c, ALIGN_RIGHT) * 0 + 52;
        } else {
            gfx_icon("SoldierIcon", (Rect){x, r.y + 6, 16, 16}, c, ICON_SILHOUETTE);
            x += 16;
            x += gfx_text(FONT_400, 16, incoming ? "↓" : "↑", x, r.y + 2, 24, c, ALIGN_LEFT) + 4;
            x += gfx_text(FONT_400, 16, jstr(a, "troops", ""), x, r.y + 2, 24, c, ALIGN_LEFT) + 4;
        }
        bool retreating = jbool(a, "retreating");
        char name[96];
        float right_w = retreating ? 90 : 28;
        gfx_ellipsize(FONT_400, boat ? 12 : 16, jstr(a, "name", ""), r.x + r.w - right_w - x, name, sizeof name);
        gfx_text(FONT_400, boat ? 12 : 16, name, x, r.y + (boat ? 6 : 2), boat ? 16 : 24, c, ALIGN_LEFT);
        Rect name_hit = {r.x, r.y, r.w - right_w, r.h};
        if (ui_button(ui_id("attack-focus", i), name_hit)) {
            int focus = (int)jnum(a, "focus", -1);
            if (focus >= 0) app_focus_player(focus);
            else if (boat) app_focus_unit((int)jnum(a, "id", 0));
        }
        if (retreating) {
            char rt[64];
            snprintf(rt, sizeof rt, "(%s...)", T("events_display.retreating"));
            gfx_text(FONT_400, 14, rt, r.x + r.w - 6, r.y + 4, 20, c, ALIGN_RIGHT);
        } else if (strcmp(kind, "in") == 0) {
            Rect b = {r.x + r.w - 32, r.y + 2, 28, 24};
            gfx_box(b, 8, alpha(hex(TW_RED_900), 0.5f), 1, alpha(hex(TW_RED_700), 0.5f));
            gfx_icon("SwordIcon", (Rect){b.x + 6, b.y + 4, 16, 16}, hex(TW_RED_400), ICON_SILHOUETTE);
            if (ui_button(ui_id("retaliate", i), b)) {
                char args[96];
                snprintf(args, sizeof args, "{\"kind\":\"retaliate\",\"id\":\"%s\"}", jstr(a, "id", ""));
                engine_call("attackAction", args, false);
            }
        } else if (strcmp(kind, "inBoat") != 0) {
            Rect b = {r.x + r.w - 26, r.y + 4, 20, 20};
            gfx_icon("XIcon", (Rect){b.x + 3, b.y + 3, 14, 14}, hex(TW_RED_500), ICON_SILHOUETTE);
            if (ui_button(ui_id("cancel-attack", i), b)) {
                char args[96];
                if (boat) snprintf(args, sizeof args, "{\"kind\":\"cancelBoat\",\"id\":\"%d\"}", (int)jnum(a, "id", 0));
                else snprintf(args, sizeof args, "{\"kind\":\"cancel\",\"id\":\"%s\"}", jstr(a, "id", ""));
                engine_call("attackAction", args, false);
            }
        }
    }
    return top;
}

// --- events-display + actionable-events --------------------------------------------

static SDL_Color severity(const char *s) {
    if (!strcmp(s, "fail")) return hex(TW_RED_400);
    if (!strcmp(s, "warn")) return hex(TW_YELLOW_400);
    if (!strcmp(s, "success")) return hex(TW_GREEN_400);
    if (!strcmp(s, "info")) return hex(TW_GRAY_200);
    if (!strcmp(s, "blue")) return hex(TW_BLUE_400);
    return hex(TW_WHITE);
}

/** Wrapped rows of a table; returns the total height. */
static float event_rows(const cJSON *rows, float x, float y, float w, float px, float lh, Font f, bool draw, int base_id) {
    float h = 0;
    for (int i = 0; i < jlen(rows); i++) {
        const cJSON *r = jat(rows, i);
        char lines[2048];
        int n = gfx_wrap(f, px, jstr(r, "text", ""), w - 16, lines, sizeof lines);
        float rh = 8 + lh * (float)n;
        if (draw) {
            const char *l = lines;
            for (int k = 0; k < n; k++) {
                gfx_text(f, px, l, x + 8, y + h + 4 + lh * (float)k, lh, severity(jstr(r, "sev", "")), ALIGN_LEFT);
                l += strlen(l) + 1;
            }
            Rect hit = {x, y + h, w, rh};
            if (ui_button(ui_id("event", base_id + i), hit)) {
                int focus = (int)jnum(r, "focus", -1), unit = (int)jnum(r, "unit", -1);
                if (focus >= 0) app_focus_player(focus);
                else if (unit >= 0) app_focus_unit(unit);
            }
        }
        h += rh;
    }
    return h;
}

static void events(const cJSON *ev) {
    if (!cJSON_IsObject(ev)) return;
    const float w = 384, right = PAGE_W - 16;
    const float x = right - w;
    float y = PAGE_H;
    // actionable-events (bottom), then the important feed, then the minor feed.
    const cJSON *actions = jget(ev, "actions");
    float heights[16];
    int na = jlen(actions);
    for (int i = na - 1; i >= 0 && i < 16; i--) {
        const cJSON *a = jat(actions, i);
        char lines[1024];
        int n = gfx_wrap(FONT_600, 16, jstr(a, "text", ""), w - 32 - 4, lines, sizeof lines);
        heights[i] = 16 + 24 * (float)n + 8 + 28 + 16;
        y -= heights[i] + 8;
        Rect card = {x, y, w, heights[i]};
        gfx_shadow(card, 8, 8, rgba(0, 0, 0, 50));
        gfx_fill_rrect(card, 8, panel_bg());
        gfx_fill_rrect4((Rect){x, y, 4, card.h}, 8, 0, 0, 8, hex(TW_YELLOW_400));
        ui_block(card);
        const char *l = lines;
        for (int k = 0; k < n; k++) {
            gfx_text(FONT_600, 16, l, x + 20, y + 16 + 24 * (float)k, 24, severity(jstr(a, "sev", "")), ALIGN_LEFT);
            l += strlen(l) + 1;
        }
        if (ui_button(ui_id("action-focus", (int)jnum(a, "id", 0)), (Rect){x, y, w, 16 + 24 * (float)n})) {
            app_focus_player((int)jnum(a, "focus", -1));
        }
        const cJSON *buttons = jget(a, "buttons");
        float bx = x + 20;
        for (int b = 0; b < jlen(buttons); b++) {
            const cJSON *btn = jat(buttons, b);
            const char *text = jstr(btn, "text", "");
            const char *kind = jstr(btn, "kind", "");
            float bw = gfx_text_width(FONT_400, 14, text) + 24;
            if (bx + bw > x + w - 12) break;
            Rect br = {bx, y + 16 + 24 * (float)n + 8, bw, 28};
            uint32_t id = ui_id("action-btn", (int)jnum(a, "id", 0) * 8 + b);
            bool hover = ui_hover(id, br);
            unsigned c = !strcmp(kind, "blue") ? (hover ? 0x155dfc : TW_BLUE_500)
                         : !strcmp(kind, "gray") ? (hover ? TW_GRAY_600 : TW_GRAY_500)
                                                 : (hover ? 0x008236 : TW_GREEN_600);
            gfx_fill_rrect(br, 4, hex(c));
            gfx_text(FONT_400, 14, text, br.x + bw / 2, br.y + 4, 20, hex(TW_WHITE), ALIGN_CENTER);
            if (ui_button(id, br)) {
                char args[64];
                snprintf(args, sizeof args, "{\"id\":%d,\"button\":%d}", (int)jnum(a, "id", 0), b);
                engine_call("eventButton", args, false);
                if (!strcmp(kind, "gray")) app_focus_player((int)jnum(a, "focus", -1));
            }
            bx += bw + 6;
        }
    }
    const cJSON *major = jget(ev, "major");
    const char *betrayal = jstr(ev, "betrayal", NULL);
    if (jlen(major) > 0 || betrayal) {
        float h = event_rows(major, x, 0, w, 18, 28, FONT_500, false, 0) + (betrayal ? 36 : 0);
        h = fminf(h, PAGE_H * 0.4f);
        y -= h + 4;
        Rect box = {x, y, w, h};
        gfx_shadow(box, 8, 8, rgba(0, 0, 0, 50));
        gfx_fill_rrect(box, 8, hex(TW_GRAY_800));
        gfx_fill_rrect4((Rect){x, y, 4, h}, 8, 0, 0, 8, hex(TW_RED_500));
        ui_block(box);
        gfx_clip(&box);
        float rows_h = event_rows(major, x, 0, w, 18, 28, FONT_500, false, 0);
        event_rows(major, x + 4, y + h - rows_h - (betrayal ? 36 : 0), w - 4, 18, 28, FONT_500, true, 100);
        if (betrayal) gfx_text(FONT_500, 18, betrayal, x + 12, y + h - 32, 28, hex(TW_YELLOW_400), ALIGN_LEFT);
        gfx_clip(NULL);
    }
    const cJSON *minor = jget(ev, "minor");
    if (jlen(minor) > 0) {
        float h = fminf(event_rows(minor, x, 0, w, 14, 20, FONT_400, false, 0), PAGE_H * 0.22f);
        y -= h + 4;
        Rect box = {x, y, w, h};
        gfx_fill_rrect(box, 8, rgba(30, 41, 57, (int)(0.92f * 0.9f * 255)));
        ui_block(box);
        gfx_clip(&box);
        float rows_h = event_rows(minor, x, 0, w, 14, 20, FONT_400, false, 0);
        event_rows(minor, x, y + h - rows_h, w, 14, 20, FONT_400, true, 200);
        gfx_clip(NULL);
    }
}

// --- Entry ----------------------------------------------------------------------------

void hud_draw(const cJSON *frame) {
    if (!frame) return;
    const cJSON *cp = jget(frame, "cp");
    const cJSON *bar = jget(frame, "hotbar");
    bool spawn = jbool(frame, "spawn");

    heads_up(jstr(frame, "heads", NULL));

    // The bottom HUD box (control panel + unit display) only shows after spawn.
    if (cJSON_IsObject(cp)) {
        float top = attacks(jget(frame, "attacks"), 625);
        (void)top;
        Rect box = {HUD_X, 625, HUD_W, 95};
        gfx_shadow(box, 8, 10, rgba(0, 0, 0, 60));
        gfx_fill_rrect(box, 8, panel_bg());
        ui_block(box);
        control_panel(cp);
        hotbar(bar);
    }
    events(jget(frame, "events"));

    if (!spawn && app.leaderboard_auto) {
        app.leaderboard_auto = false;
        app.leaderboard_shown = true;
    }
    left_sidebar(jget(frame, "lb"));
    right_sidebar(frame);
}
