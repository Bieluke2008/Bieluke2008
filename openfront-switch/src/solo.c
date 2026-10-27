// The solo setup screen, after the web client's SinglePlayerModal: the map
// grid with terrain thumbnails, nation difficulty, tribes and nations
// sliders, the game option toggles and START GAME, in its dark purple and
// orange theme.
#include "solo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "app.h"
#include "gfx.h"
#include "home.h"
#include "map.h"
#include "ui.h"

#define PAGE_BG 0x0a060f
#define HEADER_BG 0x13051d
#define CARD_BG 0x13061e
#define CARD_BORDER 0x2b1e34
#define ITEM_BG 0x201229
#define ITEM_BORDER 0x33263d
#define SEL_BG 0x31151c
#define ORANGE 0xe06c12
#define START_BG 0xb84700
#define MUTED 0x786f7e
#define LABEL 0xa99aa8

static const char *root;
static SDL_Texture **thumbs;
static bool *thumb_tried;
static int *thumb_w, *thumb_h;
static int nthumbs;
static float scroll;
static float drag_scroll_start;
static bool dragging;

static const int NATION_STEPS[] = {-1, 0, 5, 10, 20, 40, 80};
#define N_NATION_STEPS 7

void solo_init(const char *romfs) {
    root = romfs;
    const MapList *ml = engine_maps();
    nthumbs = ml->nmaps;
    thumbs = calloc((size_t)nthumbs, sizeof *thumbs);
    thumb_tried = calloc((size_t)nthumbs, sizeof *thumb_tried);
    thumb_w = calloc((size_t)nthumbs, sizeof *thumb_w);
    thumb_h = calloc((size_t)nthumbs, sizeof *thumb_h);
}

void solo_shutdown(void) {
    for (int i = 0; i < nthumbs; i++) {
        if (thumbs[i]) SDL_DestroyTexture(thumbs[i]);
    }
    free(thumbs);
    free(thumb_tried);
    free(thumb_w);
    free(thumb_h);
}

static void edit_name(StartOptions *o) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    char out[NAME_LEN] = {0};
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, o->name);
        swkbdConfigSetStringLenMax(&kbd, 20);
        if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out)) && out[0]) snprintf(o->name, sizeof o->name, "%s", out);
        swkbdClose(&kbd);
    }
#else
    (void)o;
#endif
}

static void upper(char *s) {
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
    }
}

static void section_title(const char *text, float x, float y) {
    char t[96];
    snprintf(t, sizeof t, "%s", text);
    upper(t);
    gfx_fill_rrect((Rect){x, y, 32, 32}, 6, alpha(hex(ORANGE), 0.18f));
    gfx_fill_rrect((Rect){x + 10, y + 10, 12, 12}, 3, hex(ORANGE));
    gfx_text(FONT_700, 18, t, x + 48, y + 2, 28, hex(TW_WHITE), ALIGN_LEFT);
    gfx_fill_rect((Rect){x, y + 41, 706, 1}, hex(CARD_BORDER));
}

// The map grid: section headings (world and continents, real, fictional)
// and, inside them, group headings like the web picker's category bars.
#define SECTION_H 40.0f
#define GROUP_H 30.0f

/** Places map i's card at (rx[i], ry[i]) below the grid top; returns the grid height. */
static float layout_maps(const MapList *ml, int cols, float cw, float ch, float *rx, float *ry) {
    float y = 0;
    int col = 0;
    for (int i = 0; i < ml->nmaps; i++) {
        bool sec = ml->map_sections[i][0], grp = ml->map_groups[i][0];
        if ((sec || grp) && col != 0) y += ch + 16, col = 0;
        if (sec) y += SECTION_H;
        if (grp) y += GROUP_H;
        rx[i] = (cw + 16) * (float)col;
        ry[i] = y;
        if (++col == cols) col = 0, y += ch + 16;
    }
    if (col != 0) y += ch + 16;
    return y;
}

static void map_headings(const MapList *ml, int i, float x, float card_y) {
    float y = card_y;
    if (ml->map_groups[i][0]) {
        y -= GROUP_H;
        char t[96];
        snprintf(t, sizeof t, "%s", ml->map_groups[i]);
        upper(t);
        gfx_text(FONT_700, 12, t, x + 8, y + 4, 18, alpha(hex(TW_WHITE), 0.4f), ALIGN_LEFT);
    }
    if (ml->map_sections[i][0]) {
        y -= SECTION_H;
        char t[96];
        snprintf(t, sizeof t, "%s", ml->map_sections[i]);
        upper(t);
        gfx_fill_rrect((Rect){x, y + 8, 4, 18}, 2, hex(ORANGE));
        gfx_text(FONT_700, 15, t, x + 14, y + 6, 22, hex(TW_WHITE), ALIGN_LEFT);
    }
}

/** A toggle tile: on = dark red with an orange edge, off = muted purple. */
static bool toggle(int idx, Rect r, const char *label, bool on, bool locked) {
    uint32_t id = ui_id("solo-toggle", idx);
    bool hover = ui_hover(id, r) && !locked;
    char t[96];
    snprintf(t, sizeof t, "%s", label);
    upper(t);
    gfx_box(r, 10, on ? hex(SEL_BG) : hex(hover ? 0x2a1834 : ITEM_BG), 1, on ? alpha(hex(ORANGE), 0.55f) : hex(ITEM_BORDER));
    gfx_text(FONT_700, 13, t, r.x + r.w / 2, r.y + r.h / 2 - 10, 20, on ? hex(TW_WHITE) : hex(MUTED), ALIGN_CENTER);
    return ui_button(id, r) && !locked;
}

/** The orange range slider in its box; returns the new 0..1 value. */
static float slider_box(int idx, Rect box, const char *label, const char *value, float t) {
    gfx_box(box, 10, hex(SEL_BG), 1, alpha(hex(ORANGE), 0.35f));
    Rect track = {box.x + 16, box.y + 24, box.w - 32, 6};
    uint32_t id = ui_id("solo-slider", idx);
    static int active = -1;
    Rect hit = {track.x - 10, track.y - 14, track.w + 20, 34};
    bool hover = ui_hover(id, hit);
    const Pointer *p = ui_pointer();
    if (hover && p->pressed) active = idx;
    if (!p->down && active == idx) active = -1;
    if (active == idx) t = fmaxf(0, fminf(1, (p->x - track.x) / track.w));
    gfx_fill_rrect(track, 3, hex(0x50373d));
    gfx_fill_rrect((Rect){track.x, track.y, track.w * t, 6}, 3, hex(ORANGE));
    gfx_fill_circle(track.x + track.w * t, track.y + 3, 10, hex(ORANGE));
    char l[64];
    snprintf(l, sizeof l, "%s", label);
    upper(l);
    gfx_text(FONT_700, 13, l, box.x + box.w / 2, box.y + 44, 18, hex(LABEL), ALIGN_CENTER);
    gfx_text(FONT_700, 15, value, box.x + box.w / 2, box.y + 64, 20, hex(TW_WHITE), ALIGN_CENTER);
    return t;
}

SoloResult solo_draw(StartOptions *o, int *speed_idx, const Input *in) {
    const MapList *ml = engine_maps();
    SoloResult result = SOLO_NONE;
    // The modal sits on the home page, as the web client's does.
    home_backdrop();

    Rect card = {262, 107, 756, 494};
    gfx_box(card, 12, hex(CARD_BG), 1, hex(CARD_BORDER));
    // Back = leave the app.
    Rect back = {287, 132, 40, 40};
    uint32_t bid = ui_id("solo-back", 0);
    bool bh = ui_hover(bid, back);
    gfx_fill_circle(307, 152, 20, bh ? hex(0x2a1834) : hex(0x1c0f27));
    gfx_stroke_circle(307, 152, 20, 1, hex(CARD_BORDER));
    gfx_text(FONT_400, 20, "←", 307, 140, 24, hex(0xa4a7b5), ALIGN_CENTER);
    if (ui_button(bid, back)) result = SOLO_QUIT;
    char title[64];
    snprintf(title, sizeof title, "%s", T("main.solo"));
    upper(title);
    gfx_text(FONT_800, 26, title, 343, 137, 30, hex(TW_WHITE), ALIGN_LEFT);
    gfx_fill_rect((Rect){263, 196, 754, 1}, hex(CARD_BORDER));
    gfx_fill_rect((Rect){263, 492, 754, 1}, hex(CARD_BORDER));

    // --- Scrolling content -------------------------------------------------------------
    Rect view = {263, 197, 754, 295};
    const float x0 = 287, w = 706;
    const int cols = 4;
    const float cw = (w - 16 * (cols - 1)) / cols, ch = 132;
    static float *rx, *ry;
    static int nlaid;
    if (nlaid != ml->nmaps) {
        rx = realloc(rx, sizeof(float) * (size_t)(ml->nmaps + 1));
        ry = realloc(ry, sizeof(float) * (size_t)(ml->nmaps + 1));
        nlaid = ml->nmaps;
    }
    float maps_h = layout_maps(ml, cols, cw, ch, rx, ry);
    float content = 16 + 42 + 16 + maps_h + 16 + 42 + 16 + 70 + 24 + 42 + 16 + 90 + 16 + 64 + 16 + 64 + 24;
    float max_scroll = fmaxf(0, content - view.h);
    const Pointer *p = ui_pointer();
    bool in_view = in_rect(view, p->x, p->y);
    if (in_view) scroll -= p->wheel * 60;
    scroll += in->ry * 12;
    if (p->pressed && in_view) {
        drag_scroll_start = scroll;
        dragging = true;
    }
    if (!p->down) dragging = false;
    if (dragging && in->dragging) scroll -= in->drag_dy / gfx_scale();
    scroll = fmaxf(0, fminf(max_scroll, scroll));
    (void)drag_scroll_start;
    ui_block(view);
    gfx_clip(&view);
    float y = view.y + 16 - scroll;

    section_title(T("map.map"), x0, y);
    y += 42 + 16;
    int loads = 0;
    for (int i = 0; i < ml->nmaps; i++) {
        Rect r = {x0 + rx[i], y + ry[i], cw, ch};
        if (r.y + r.h < view.y || r.y - SECTION_H - GROUP_H > view.y + view.h) continue;
        map_headings(ml, i, x0, r.y);
        if (r.y > view.y + view.h) continue;
        bool sel = o->map == i;
        uint32_t id = ui_id("solo-map", i);
        bool hover = ui_hover(id, r) && in_view;
        gfx_box(r, 10, sel ? hex(SEL_BG) : hex(hover ? 0x2a1834 : ITEM_BG), sel ? 2 : 1, sel ? hex(ORANGE) : hex(ITEM_BORDER));
        if (!thumb_tried[i] && loads < 3) {
            // OpenFront's own thumbnail, else one drawn from the terrain.
            thumb_tried[i] = true;
            char path[512];
            snprintf(path, sizeof path, "%sthumbnails/%s.png", root, ml->map_folders[i]);
            thumbs[i] = gfx_load_texture(path, &thumb_w[i], &thumb_h[i]);
            if (!thumbs[i]) thumbs[i] = map_preview(root, ml->map_folders[i], &thumb_w[i], &thumb_h[i]);
            loads++;
        }
        Rect img = {r.x + 10, r.y + 10, cw - 20, 86};
        gfx_fill_rrect(img, 6, hex(0x0d0614));
        if (thumbs[i]) {
            // object-cover: fill the box, cropping the long side.
            float s = fmaxf(img.w / (float)thumb_w[i], img.h / (float)thumb_h[i]);
            int sw = (int)(img.w / s), sh = (int)(img.h / s);
            SDL_Rect src = {(thumb_w[i] - sw) / 2, (thumb_h[i] - sh) / 2, sw, sh};
            gfx_image(thumbs[i], &src, img, rgba(255, 255, 255, 255));
        }
        char name[96];
        gfx_ellipsize(FONT_600, 13, ml->map_names[i], cw - 16, name, sizeof name);
        gfx_text(FONT_600, 13, name, r.x + cw / 2, r.y + 104, 20, sel ? hex(TW_WHITE) : hex(0xd8d0dc), ALIGN_CENTER);
        if (ui_button(id, r) && in_view) o->map = i;
    }
    y += maps_h + 16;

    section_title(T("difficulty.difficulty"), x0, y);
    y += 42 + 16;
    static const char *diff[4] = {"difficulty.easy", "difficulty.medium", "difficulty.hard", "difficulty.impossible"};
    for (int i = 0; i < 4; i++) {
        Rect r = {x0 + (cw + 16) * (float)i, y, cw, 70};
        if (toggle(10 + i, r, T(diff[i]), o->difficulty == i, false) && in_view) o->difficulty = i;
    }
    y += 70 + 24;

    section_title(T("game_settings.options"), x0, y);
    y += 42 + 16;
    char value[32];
    snprintf(value, sizeof value, "%d", o->bots);
    float half = (w - 16) / 2;
    float t = slider_box(0, (Rect){x0, y, half, 90}, T("game_settings.bots"), value, (float)o->bots / 400);
    if (in_view || dragging) o->bots = (int)lroundf(t * 40) * 10;
    int ni = 0;
    for (int i = 0; i < N_NATION_STEPS; i++) {
        if (NATION_STEPS[i] == o->nations) ni = i;
    }
    if (o->nations < 0) snprintf(value, sizeof value, "%s", T("port.default"));
    else snprintf(value, sizeof value, "%d", o->nations);
    t = slider_box(1, (Rect){x0 + half + 16, y, half, 90}, T("game_settings.nations"), value, (float)ni / (N_NATION_STEPS - 1));
    o->nations = NATION_STEPS[(int)lroundf(t * (N_NATION_STEPS - 1))];
    y += 90 + 16;
    Rect tr = {x0, y, cw, 64};
    if (toggle(20, tr, T("game_settings.instant_build"), o->instant_build, false)) o->instant_build = !o->instant_build;
    tr.x += cw + 16;
    if (toggle(21, tr, T("game_settings.infinite_gold"), o->infinite_gold, false)) o->infinite_gold = !o->infinite_gold;
    tr.x += cw + 16;
    if (toggle(22, tr, T("game_settings.infinite_troops"), o->infinite_troops, false)) o->infinite_troops = !o->infinite_troops;
    tr.x += cw + 16;
    // Compact maps keep the engine fast enough on the console.
    toggle(23, tr, T("game_settings.compact_map"), true, true);
    y += 64 + 16;
    // The port's own: player name and game speed.
    Rect nr = {x0, y, half, 64};
    uint32_t nid = ui_id("solo-name", 0);
    bool nh = ui_hover(nid, nr);
    gfx_box(nr, 10, hex(nh ? 0x2a1834 : ITEM_BG), 1, hex(ITEM_BORDER));
    char l[64];
    snprintf(l, sizeof l, "%s", T("port.name"));
    upper(l);
    gfx_text(FONT_700, 13, l, nr.x + 16, nr.y + 10, 18, hex(LABEL), ALIGN_LEFT);
    gfx_text(FONT_600, 16, o->name, nr.x + 16, nr.y + 32, 22, hex(TW_WHITE), ALIGN_LEFT);
    if (ui_button(nid, nr)) edit_name(o);
    static const char *speeds[4] = {"×0.5", "×1", "×2", "×4"};
    snprintf(l, sizeof l, "%s", T("port.speed"));
    upper(l);
    Rect sr = {x0 + half + 16, y, half, 64};
    gfx_box(sr, 10, hex(ITEM_BG), 1, hex(ITEM_BORDER));
    gfx_text(FONT_700, 13, l, sr.x + 16, sr.y + 10, 18, hex(LABEL), ALIGN_LEFT);
    for (int i = 0; i < 4; i++) {
        Rect b = {sr.x + 16 + (float)i * ((half - 32) / 4), sr.y + 30, (half - 32) / 4 - 6, 26};
        uint32_t id = ui_id("solo-speed", i);
        bool hover = ui_hover(id, b);
        bool on = *speed_idx == i;
        gfx_fill_rrect(b, 6, on ? hex(ORANGE) : hex(hover ? 0x3a2845 : 0x2a1834));
        gfx_text(FONT_600, 13, speeds[i], b.x + b.w / 2, b.y + 4, 18, hex(TW_WHITE), ALIGN_CENTER);
        if (ui_button(id, b)) *speed_idx = i;
    }
    gfx_clip(NULL);

    // START GAME
    Rect start = {287, 517, 706, 59};
    uint32_t sid = ui_id("solo-start", 0);
    bool sh = ui_hover(sid, start);
    gfx_fill_rrect(start, 10, hex(sh ? 0xcc5200 : START_BG));
    char st[64];
    snprintf(st, sizeof st, "%s", T("game_settings.start"));
    upper(st);
    gfx_text(FONT_700, 20, st, PAGE_W / 2.0f, 532, 28, hex(TW_WHITE), ALIGN_CENTER);
    if (ui_button(sid, start)) result = SOLO_START;

    // Controller shortcuts: + starts, L/R pick the map.
    if (in->pressed[BTN_PLUS]) result = SOLO_START;
    if (ml->nmaps && (in->pressed[BTN_L] || in->pressed[BTN_R])) {
        o->map = (o->map + (in->pressed[BTN_R] ? 1 : ml->nmaps - 1)) % ml->nmaps;
        float target = 16 + 42 + 16 + ry[o->map];
        if (target < scroll || target + ch > scroll + view.h) scroll = target - 16;
    }
    return result;
}
