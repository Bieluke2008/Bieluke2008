// The solo setup screen, after the web client's SinglePlayerModal and its
// GameConfigSettings: the map grid, nation difficulty, mode and teams, the
// tribes and nations sliders, every game option and modifier, the units to
// disable and START GAME, in its dark purple and orange theme.
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

// GameConfigSettings' section icons (romfs/ui, rasterized from its SVGs).
enum { ICON_MAP, ICON_DIFFICULTY, ICON_MODE, ICON_OPTIONS, ICON_ENABLES, ICON_SKULL, ICON_BURNING_SKULL, N_ICONS };
static const char *ICON_FILES[N_ICONS] = {"map_icon", "difficulty_icon", "mode_icon", "options_icon",
                                          "enables_icon", "skull", "burningSkull"};
static SDL_Texture *icons[N_ICONS];
static float content_h = 2000;  // the scrolling content's height, measured as it is drawn

void solo_init(const char *romfs) {
    root = romfs;
    const MapList *ml = engine_maps();
    nthumbs = ml->nmaps;
    thumbs = calloc((size_t)nthumbs, sizeof *thumbs);
    thumb_tried = calloc((size_t)nthumbs, sizeof *thumb_tried);
    thumb_w = calloc((size_t)nthumbs, sizeof *thumb_w);
    thumb_h = calloc((size_t)nthumbs, sizeof *thumb_h);
    for (int i = 0; i < N_ICONS; i++) {
        char path[512];
        int w, h;
        snprintf(path, sizeof path, "%sui/%s.png", romfs, ICON_FILES[i]);
        icons[i] = gfx_load_texture(path, &w, &h);
    }
}

void solo_shutdown(void) {
    for (int i = 0; i < nthumbs; i++) {
        if (thumbs[i]) SDL_DestroyTexture(thumbs[i]);
    }
    free(thumbs);
    free(thumb_tried);
    free(thumb_w);
    free(thumb_h);
    for (int i = 0; i < N_ICONS; i++) {
        if (icons[i]) SDL_DestroyTexture(icons[i]);
        icons[i] = NULL;
    }
}

/** A number from the Switch keyboard (decimals allowed); false if cancelled. */
static bool ask_number(double *v) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    char out[32] = {0}, init[32];
    bool ok = false;
    snprintf(init, sizeof init, "%g", *v);
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
        swkbdConfigSetLeftOptionalSymbolKey(&kbd, ".");
        swkbdConfigSetInitialText(&kbd, init);
        swkbdConfigSetStringLenMax(&kbd, 8);
        if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out)) && out[0]) *v = atof(out), ok = true;
        swkbdClose(&kbd);
    }
    return ok;
#else
    (void)v;
    return false;
#endif
}

/** renderSectionHeader: the coloured icon tile, the title and the rule under them. */
static void section_title(const char *text, int icon, unsigned fg, unsigned bg, float x, float y) {
    char t[96];
    snprintf(t, sizeof t, "%s", text);
    gfx_upper(t);
    gfx_fill_rrect((Rect){x, y, 32, 32}, 8, alpha(hex(bg), 0.2f));
    if (icons[icon]) gfx_image(icons[icon], NULL, (Rect){x + 6, y + 6, 20, 20}, hex(fg));
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
        gfx_upper(t);
        gfx_text(FONT_700, 12, t, x + 8, y + 4, 18, alpha(hex(TW_WHITE), 0.4f), ALIGN_LEFT);
    }
    if (ml->map_sections[i][0]) {
        y -= SECTION_H;
        char t[96];
        snprintf(t, sizeof t, "%s", ml->map_sections[i]);
        gfx_upper(t);
        gfx_fill_rrect((Rect){x, y + 8, 4, 18}, 2, hex(ORANGE));
        gfx_text(FONT_700, 15, t, x + 14, y + 6, 22, hex(TW_WHITE), ALIGN_LEFT);
    }
}

/** cardClass: on = dark red with an orange edge, off = muted purple. */
static void card_box(Rect r, bool on, bool hover) {
    gfx_box(r, 10, on ? hex(SEL_BG) : hex(hover ? 0x2a1834 : ITEM_BG), 1, on ? alpha(hex(ORANGE), 0.55f) : hex(ITEM_BORDER));
}

/** Centred uppercase card label, word-wrapped to at most three lines around `cy`. */
static void card_label(Rect r, const char *label, float cy, bool on) {
    char t[128];
    snprintf(t, sizeof t, "%s", label);
    gfx_upper(t);
    SDL_Color c = on ? hex(TW_WHITE) : hex(MUTED);
    float maxw = r.w - 16;
    char lines[3][128];
    int n = 0;
    char *word = strtok(t, " ");
    lines[0][0] = 0;
    while (word) {
        char trial[128];
        snprintf(trial, sizeof trial, "%s%s%s", lines[n], lines[n][0] ? " " : "", word);
        if (lines[n][0] && gfx_text_width(FONT_700, 12, trial) > maxw && n < 2) {
            n++;
            snprintf(lines[n], sizeof lines[n], "%s", word);
        } else {
            snprintf(lines[n], sizeof lines[n], "%s", trial);
        }
        word = strtok(NULL, " ");
    }
    n++;
    const float LH = 16;
    float y = cy - (float)n * LH / 2;
    for (int i = 0; i < n; i++) gfx_text(FONT_700, 12, lines[i], r.x + r.w / 2, y + (float)i * LH, LH, c, ALIGN_CENTER);
}

/** A toggle tile (renderTextCardButton); true when tapped. */
static bool toggle(int idx, Rect r, const char *label, bool on, bool locked) {
    uint32_t id = ui_id("solo-toggle", idx);
    bool hover = ui_hover(id, r) && !locked;
    card_box(r, on, hover);
    card_label(r, label, r.y + r.h / 2, on);
    return ui_button(id, r) && !locked;
}

/**
 * ToggleInputCard: a checkbox, the value with its − and + (tap the value to
 * type it), and the label. Toggling on starts from `def`.
 */
static void input_card(int idx, Rect r, const char *label, bool *on, double *v, double lo, double hi, double step,
                       double def, const char *zero_label) {
    uint32_t id = ui_id("solo-input", idx);
    Rect minus = {r.x + 10, r.y + r.h / 2 - 16, 32, 32}, plus = {r.x + r.w - 42, r.y + r.h / 2 - 16, 32, 32};
    Rect value = {minus.x + 36, minus.y, plus.x - minus.x - 40, 32};
    const Pointer *p = ui_pointer();
    bool on_controls = *on && (in_rect(minus, p->x, p->y) || in_rect(plus, p->x, p->y) || in_rect(value, p->x, p->y));
    bool hover = ui_hover(id, r) && !on_controls;
    card_box(r, *on, hover);
    // The checkbox.
    Rect cb = {r.x + r.w / 2 - 10, r.y + 12, 20, 20};
    if (*on) {
        gfx_fill_rrect(cb, 4, hex(0x3b82f6));
        gfx_line(cb.x + 5, cb.y + 10, cb.x + 9, cb.y + 14, 2, hex(TW_WHITE));
        gfx_line(cb.x + 9, cb.y + 14, cb.x + 15, cb.y + 6, 2, hex(TW_WHITE));
    } else {
        gfx_box(cb, 4, rgba(255, 255, 255, 13), 1, rgba(255, 255, 255, 51));
        gfx_fill_rrect((Rect){r.x + r.w / 2 - 8, r.y + r.h / 2 - 1, 16, 2}, 1, rgba(255, 255, 255, 26));
    }
    card_label(r, label, r.y + r.h - 26, *on);
    if (!*on) {
        if (ui_button(id, r)) *on = true, *v = def;
        return;
    }
    uint32_t mid = ui_id("solo-input-minus", idx), pid = ui_id("solo-input-plus", idx), vid = ui_id("solo-input-v", idx);
    bool mh = ui_hover(mid, minus), ph = ui_hover(pid, plus), vh = ui_hover(vid, value);
    gfx_fill_rrect(minus, 8, hex(mh ? 0x4a2a35 : 0x2a1834));
    gfx_fill_rrect(plus, 8, hex(ph ? 0x4a2a35 : 0x2a1834));
    gfx_text(FONT_700, 20, "−", minus.x + 16, minus.y + 3, 26, hex(TW_WHITE), ALIGN_CENTER);
    gfx_text(FONT_700, 20, "+", plus.x + 16, plus.y + 3, 26, hex(TW_WHITE), ALIGN_CENTER);
    gfx_box(value, 8, hex(vh ? 0x2a1834 : 0x1a0d22), 1, rgba(255, 255, 255, 38));
    char t[48];
    if (*v == 0 && zero_label) snprintf(t, sizeof t, "0");
    else snprintf(t, sizeof t, "%g", *v);
    gfx_text(FONT_700, 15, t, value.x + value.w / 2, value.y + 6, 20, hex(TW_WHITE), ALIGN_CENTER);
    if (*v == 0 && zero_label) {
        char z[96];
        gfx_ellipsize(FONT_500, 10, zero_label, r.w - 12, z, sizeof z);
        gfx_text(FONT_500, 10, z, r.x + r.w / 2, value.y + 34, 14, hex(MUTED), ALIGN_CENTER);
    }
    if (ui_button(mid, minus)) *v = fmax(lo, round((*v - step) / step) * step);
    if (ui_button(pid, plus)) *v = fmin(hi, round((*v + step) / step) * step);
    if (ui_button(vid, value)) {
        double nv = *v;
        if (ask_number(&nv)) *v = fmax(lo, fmin(hi, nv));
    }
    if (!on_controls && ui_button(id, r)) *on = false;
}

/** FluentSlider in its card; `suffix` follows the value, muted. Returns the new 0..1 value. */
static float slider_box(int idx, Rect box, const char *label, const char *value, const char *suffix, float t) {
    card_box(box, t > 0, false);
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
    gfx_upper(l);
    gfx_text(FONT_700, 13, l, box.x + box.w / 2, box.y + 44, 18, hex(LABEL), ALIGN_CENTER);
    float vw = gfx_text_width(FONT_700, 15, value), sw = suffix ? gfx_text_width(FONT_700, 15, suffix) + 6 : 0;
    float vx = box.x + box.w / 2 - (vw + sw) / 2;
    gfx_text(FONT_700, 15, value, vx, box.y + 64, 20, hex(TW_WHITE), ALIGN_LEFT);
    if (suffix) gfx_text(FONT_700, 15, suffix, vx + vw + 6, box.y + 64, 20, hex(MUTED), ALIGN_LEFT);
    return t;
}

/** Difficulties.ts: one to three skulls, or the burning skull for Impossible. */
static void skulls(int difficulty, float cx, float cy) {
    SDL_Color red = hex(0xff3838), dim = rgba(255, 255, 255, 77);
    if (difficulty == 3) {
        if (icons[ICON_BURNING_SKULL]) gfx_image(icons[ICON_BURNING_SKULL], NULL, (Rect){cx - 25, cy - 25, 50, 50}, red);
        return;
    }
    for (int i = 0; i < 3; i++) {
        Rect r = {cx - 36 + (float)i * 26, cy - 10, 20, 20};
        if (icons[ICON_SKULL]) gfx_image(icons[ICON_SKULL], NULL, r, i <= difficulty ? red : dim);
    }
}

// GameConfigSettings' unit options, in its order (StartOptions.disabled_units bits).
static const char *UNIT_KEYS[] = {"unit_type.city",         "unit_type.defense_post", "unit_type.port",
                                  "unit_type.warship",      "unit_type.boat",         "unit_type.missile_silo",
                                  "unit_type.sam_launcher", "unit_type.atom_bomb",    "unit_type.hydrogen_bomb",
                                  "unit_type.mirv",         "unit_type.factory"};
#define N_UNIT_OPTIONS 11
static const char *SPEED_KEYS[] = {"doomsday_clock_speed.slow", "doomsday_clock_speed.normal",
                                   "doomsday_clock_speed.fast", "doomsday_clock_speed.veryfast"};

static void team_label(int i, char *out, size_t n) {
    static const char *special[] = {"host_modal.teams_Quads", "host_modal.teams_Trios", "host_modal.teams_Duos",
                                    "host_modal.teams_Humans Vs Nations"};
    if (i >= 6) {
        snprintf(out, n, "%s", T(special[i - 6]));
        return;
    }
    // public_lobby.teams: "{num} teams".
    const char *f = T("public_lobby.teams");
    const char *at = strstr(f, "{num}");
    if (at) snprintf(out, n, "%.*s%d%s", (int)(at - f), f, i + 2, at + 5);
    else snprintf(out, n, "%d", i + 2);
}

/** The lobby's code and players (HostLobbyModal's header and player list). */
static float lobby_header(const SoloLobby *lobby, float x0, float y, float w) {
    Rect cr = {x0, y, w, 64};
    gfx_box(cr, 10, hex(ITEM_BG), 1, hex(ITEM_BORDER));
    char l[96];
    snprintf(l, sizeof l, "%s", T("port.mp_code"));
    gfx_upper(l);
    gfx_text(FONT_700, 13, l, cr.x + 16, cr.y + 12, 18, hex(LABEL), ALIGN_LEFT);
    gfx_text(FONT_400, 12, lobby->hint ? lobby->hint : T("port.mp_code_hint"), cr.x + 16, cr.y + 34, 18, hex(0x8f8696),
             ALIGN_LEFT);
    gfx_text(FONT_800, 28, lobby->code, cr.x + cr.w - 16, cr.y + 14, 34, hex(ORANGE), ALIGN_RIGHT);
    y += 64 + 24;
    char title[96];
    snprintf(title, sizeof title, "%s (%d)", T("host_modal.players"), lobby->nplayers);
    section_title(title, ICON_MODE, 0xc084fc, 0xa855f7, x0, y);
    y += 42 + 16;
    float cx = x0;
    for (int i = 0; i < lobby->nplayers; i++) {
        char name[96];
        if (i == 0) snprintf(name, sizeof name, "%s · %s", lobby->players[i], T("host_modal.host_badge"));
        else snprintf(name, sizeof name, "%s", lobby->players[i]);
        float pw = gfx_text_width(FONT_600, 14, name) + 28;
        if (cx > x0 && cx + pw > x0 + w) cx = x0, y += 40;
        gfx_box((Rect){cx, y, pw, 32}, 8, i == 0 ? hex(SEL_BG) : hex(ITEM_BG), 1, i == 0 ? hex(ORANGE) : hex(ITEM_BORDER));
        gfx_text(FONT_600, 14, name, cx + 14, y + 7, 18, hex(TW_WHITE), ALIGN_LEFT);
        cx += pw + 10;
    }
    if (lobby->nplayers < 2) {
        gfx_text(FONT_400, 14, T("host_modal.waiting"), cx + 6, y + 7, 18, hex(0x8f8696), ALIGN_LEFT);
    }
    return y + 32 + 24;
}

SoloResult solo_draw(StartOptions *o, int *speed_idx, const Input *in, const SoloLobby *lobby) {
    const MapList *ml = engine_maps();
    SoloResult result = SOLO_NONE;
    // The modal sits on the home page, as the web client's does.
    home_backdrop();

    Rect card = {262, 100, 756, 530};
    gfx_box(card, 12, hex(CARD_BG), 1, hex(CARD_BORDER));
    // Back: to the home page.
    Rect back = {287, 123, 40, 40};
    uint32_t bid = ui_id("solo-back", 0);
    bool bh = ui_hover(bid, back);
    gfx_fill_circle(307, 143, 20, bh ? hex(0x2a1834) : hex(0x1c0f27));
    gfx_stroke_circle(307, 143, 20, 1, hex(CARD_BORDER));
    gfx_text(FONT_400, 20, "←", 307, 131, 24, hex(0xa4a7b5), ALIGN_CENTER);
    if (ui_button(bid, back)) result = SOLO_QUIT;
    char title[64];
    snprintf(title, sizeof title, "%s", lobby ? T("port.mp_lobby") : T("main.solo"));
    gfx_upper(title);
    gfx_text(FONT_800, 26, title, 343, 128, 30, hex(TW_WHITE), ALIGN_LEFT);
    gfx_fill_rect((Rect){263, 186, 754, 1}, hex(CARD_BORDER));
    gfx_fill_rect((Rect){263, 532, 754, 1}, hex(CARD_BORDER));

    // --- Scrolling content -------------------------------------------------------------
    Rect view = {263, 187, 754, 345};
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
    float max_scroll = fmaxf(0, content_h - view.h);
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
    const float top = view.y + 16 - scroll;
    float y = top;
    if (lobby) y = lobby_header(lobby, x0, y, w);
    const float maps_top = y - top + 16;  // where the map section starts in the content

    // Map.
    section_title(T("map.map"), ICON_MAP, 0x38bdf8, 0x0084d1, x0, y);
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
            float sc = fmaxf(img.w / (float)thumb_w[i], img.h / (float)thumb_h[i]);
            int sw = (int)(img.w / sc), sh = (int)(img.h / sc);
            SDL_Rect src = {(thumb_w[i] - sw) / 2, (thumb_h[i] - sh) / 2, sw, sh};
            gfx_image(thumbs[i], &src, img, rgba(255, 255, 255, 255));
        }
        char name[96];
        gfx_ellipsize(FONT_600, 13, ml->map_names[i], cw - 16, name, sizeof name);
        gfx_text(FONT_600, 13, name, r.x + cw / 2, r.y + 104, 20, sel ? hex(TW_WHITE) : hex(0xd8d0dc), ALIGN_CENTER);
        if (ui_button(id, r) && in_view && o->map != i) {
            o->map = i;
            o->nations = -1;  // the new map's own count
        }
    }
    y += maps_h + 16;

    // Nation difficulty: off without nations.
    int default_nations = ml->map_nations ? ml->map_nations[o->map] : 0;
    int nations = o->nations < 0 ? default_nations : o->nations;
    bool no_nations = nations == 0;
    section_title(T("difficulty.difficulty"), ICON_DIFFICULTY, 0x4ade80, 0x22c55e, x0, y);
    y += 42 + 16;
    static const char *diff[4] = {"difficulty.easy", "difficulty.medium", "difficulty.hard", "difficulty.impossible"};
    for (int i = 0; i < 4; i++) {
        Rect r = {x0 + (cw + 16) * (float)i, y, cw, 108};
        uint32_t id = ui_id("solo-diff", i);
        bool hover = ui_hover(id, r) && in_view && !no_nations;
        if (no_nations) {
            gfx_box(r, 10, rgba(255, 255, 255, 13), 1, rgba(255, 255, 255, 13));
        } else {
            card_box(r, o->difficulty == i, hover);
        }
        skulls(i, r.x + r.w / 2, r.y + 40);
        card_label(r, T(diff[i]), r.y + 84, true);
        if (no_nations) gfx_fill_rrect(r, 10, rgba(19, 6, 30, 179));  // opacity-30 grayscale
        else if (ui_button(id, r) && in_view) o->difficulty = i;
    }
    y += 108 + 24;

    // Mode, and the team count in team games.
    section_title(T("host_modal.mode"), ICON_MODE, 0xc084fc, 0xa855f7, x0, y);
    y += 42 + 16;
    float half = (w - 16) / 2;
    if (toggle(30, (Rect){x0, y, half, 72}, T("game_mode.ffa"), o->game_mode == 0, false) && in_view) o->game_mode = 0;
    if (toggle(31, (Rect){x0 + half + 16, y, half, 72}, T("game_mode.teams"), o->game_mode == 1, false) && in_view)
        o->game_mode = 1;
    y += 72 + 24;
    if (o->game_mode == 1) {
        char tc[64];
        snprintf(tc, sizeof tc, "%s", T("host_modal.team_count"));
        gfx_upper(tc);
        gfx_text(FONT_700, 12, tc, x0 + 8, y, 18, alpha(hex(TW_WHITE), 0.4f), ALIGN_LEFT);
        y += 28;
        float tw = (w - 12 * 4) / 5;
        for (int i = 0; i < 10; i++) {
            char l[64];
            team_label(i, l, sizeof l);
            Rect r = {x0 + (tw + 12) * (float)(i % 5), y + (float)(i / 5) * 60, tw, 48};
            if (toggle(40 + i, r, l, o->team_count == i, false) && in_view) o->team_count = i;
        }
        y += 2 * 60 + 12;
    }

    // Options: sliders, toggles and the toggle-input cards.
    section_title(T("game_settings.options"), ICON_OPTIONS, 0xfb923c, 0xf97316, x0, y);
    y += 42 + 16;
    char value[32], suffix[64];
    snprintf(value, sizeof value, "%d", o->bots);
    float t = slider_box(0, (Rect){x0, y, half, 90}, T("game_settings.bots"), o->bots ? value : T("common.disabled"),
                         NULL, (float)o->bots / 400);
    if (in_view || dragging) o->bots = (int)lroundf(t * 400);
    snprintf(value, sizeof value, "%d", nations);
    snprintf(suffix, sizeof suffix, "(%s)", T("common.map_default"));
    gfx_upper(suffix);
    t = slider_box(1, (Rect){x0 + half + 16, y, half, 90}, T("game_settings.nations"), nations ? value : T("common.disabled"),
                   nations && nations == default_nations ? suffix : NULL, (float)nations / 400);
    {
        int nv = (int)lroundf(t * 400);
        if (nv != nations) o->nations = nv == default_nations ? -1 : nv;  // sliderToNationsConfig
    }
    y += 90 + 16;
    Rect tr = {x0, y, cw, 64};
    if (toggle(20, tr, T("game_settings.instant_build"), o->instant_build, false)) o->instant_build = !o->instant_build;
    tr.x += cw + 16;
    if (toggle(21, tr, T("game_settings.random_spawn"), o->random_spawn, false)) o->random_spawn = !o->random_spawn;
    tr.x += cw + 16;
    if (toggle(22, tr, T("game_settings.infinite_gold"), o->infinite_gold, false)) o->infinite_gold = !o->infinite_gold;
    tr.x += cw + 16;
    if (toggle(23, tr, T("game_settings.infinite_troops"), o->infinite_troops, false))
        o->infinite_troops = !o->infinite_troops;
    y += 64 + 16;
    const float IH = 120;
    // Compact maps keep the engine fast enough on the console: always on.
    toggle(24, (Rect){x0, y, cw, IH}, T("game_settings.compact_map"), true, true);
    if (toggle(25, (Rect){x0 + (cw + 16), y, cw, IH}, T("game_settings.water_nukes"), o->water_nukes, false))
        o->water_nukes = !o->water_nukes;
    {
        // The doomsday clock card grows a pace selector while on.
        Rect r = {x0 + 2 * (cw + 16), y, cw, IH};
        Rect pace = {r.x + 12, r.y + IH - 40, r.w - 24, 28};
        bool on_pace = o->doomsday && in_rect(pace, p->x, p->y);
        uint32_t id = ui_id("solo-doomsday", 0);
        bool hover = ui_hover(id, r) && !on_pace;
        card_box(r, o->doomsday, hover);
        card_label(r, T("game_settings.doomsday_clock"), o->doomsday ? r.y + 40 : r.y + IH / 2, o->doomsday);
        if (o->doomsday) {
            uint32_t pid = ui_id("solo-doomsday-pace", 0);
            bool ph = ui_hover(pid, pace);
            gfx_box(pace, 8, rgba(255, 255, 255, ph ? 38 : 26), 1, rgba(255, 255, 255, 51));
            const char *sp = T(SPEED_KEYS[o->doomsday_speed & 3]);
            float spw = gfx_text_width(FONT_500, 12, sp);
            gfx_text(FONT_500, 12, sp, pace.x + pace.w / 2 - 6, pace.y + 6, 17, hex(TW_WHITE), ALIGN_CENTER);
            float ax = pace.x + pace.w / 2 + spw / 2 + 2, ay = pace.y + 12;
            gfx_fill_triangle(ax, ay, ax + 8, ay, ax + 4, ay + 5, hex(TW_WHITE));
            if (ui_button(pid, pace)) o->doomsday_speed = (o->doomsday_speed + 1) % 4;
        }
        if (!on_pace && ui_button(id, r)) {
            o->doomsday = !o->doomsday;
            if (o->doomsday && (o->doomsday_speed < 0 || o->doomsday_speed > 3)) o->doomsday_speed = 1;
        }
    }
    input_card(0, (Rect){x0 + 3 * (cw + 16), y, cw, IH}, T("game_settings.max_timer"), &o->max_timer_on, &o->max_timer, 1,
               120, 5, 30, NULL);
    y += IH + 16;
    input_card(1, (Rect){x0, y, cw, IH}, T("game_settings.gold_multiplier"), &o->gold_mult_on, &o->gold_mult, 0.1, 1000, 0.5,
               2, NULL);
    input_card(2, (Rect){x0 + (cw + 16), y, cw, IH}, T("game_settings.starting_gold"), &o->starting_gold_on,
               &o->starting_gold, 0.1, 1000, 1, 5, NULL);
    char zero[96];
    snprintf(zero, sizeof zero, "(%s)", T("public_game_modifier.disable_alliances"));
    input_card(3, (Rect){x0 + 2 * (cw + 16), y, cw, IH}, T("game_settings.custom_alliances"), &o->alliance_on,
               &o->alliance_minutes, 0, 15, 1, 0, zero);
    input_card(4, (Rect){x0 + 3 * (cw + 16), y, cw, IH}, T("game_settings.overtime"), &o->overtime_on, &o->overtime, 1, 120,
               5, 30, NULL);
    y += IH + 16;
    if (!lobby) {
        // The port's own: game speed (a server sets its own pace).
        static const char *speeds[4] = {"×0.5", "×1", "×2", "×4"};
        char l[64];
        snprintf(l, sizeof l, "%s", T("port.speed"));
        gfx_upper(l);
        Rect sr = {x0, y, w, 64};
        gfx_box(sr, 10, hex(ITEM_BG), 1, hex(ITEM_BORDER));
        gfx_text(FONT_700, 13, l, sr.x + 16, sr.y + 22, 18, hex(LABEL), ALIGN_LEFT);
        for (int i = 0; i < 4; i++) {
            Rect b = {sr.x + 240 + (float)i * 112, sr.y + 16, 100, 32};
            uint32_t id = ui_id("solo-speed", i);
            bool hover = ui_hover(id, b);
            bool on = *speed_idx == i;
            gfx_fill_rrect(b, 6, on ? hex(ORANGE) : hex(hover ? 0x3a2845 : 0x2a1834));
            gfx_text(FONT_600, 14, speeds[i], b.x + b.w / 2, b.y + 6, 20, hex(TW_WHITE), ALIGN_CENTER);
            if (ui_button(id, b)) *speed_idx = i;
        }
        y += 64 + 24;
    } else {
        y += 8;
    }

    // Units to disable: on = enabled.
    section_title(T("game_settings.disable_units"), ICON_ENABLES, 0x2dd4bf, 0x14b8a6, x0, y);
    y += 42 + 16;
    float uw = (w - 16 * 4) / 5;
    for (int i = 0; i < N_UNIT_OPTIONS; i++) {
        Rect r = {x0 + (uw + 16) * (float)(i % 5), y + (float)(i / 5) * 96, uw, 80};
        bool enabled = !(o->disabled_units >> i & 1);
        if (toggle(60 + i, r, T(UNIT_KEYS[i]), enabled, false) && in_view) o->disabled_units ^= 1u << i;
    }
    y += 3 * 96;
    content_h = y - top + 16;
    gfx_clip(NULL);

    // START GAME
    Rect start = {287, 552, 706, 56};
    uint32_t sid = ui_id("solo-start", 0);
    bool sh = ui_hover(sid, start);
    gfx_fill_rrect(start, 10, hex(sh ? 0xcc5200 : START_BG));
    char st[64];
    snprintf(st, sizeof st, "%s", lobby && lobby->starting ? T("game_starting_modal.title") : T("game_settings.start"));
    gfx_upper(st);
    gfx_text(FONT_700, 20, st, PAGE_W / 2.0f, 565, 28, hex(TW_WHITE), ALIGN_CENTER);
    if (ui_button(sid, start)) result = SOLO_START;

    // Controller shortcuts: + starts, L/R pick the map.
    if (in->pressed[BTN_PLUS]) result = SOLO_START;
    if (ml->nmaps && (in->pressed[BTN_L] || in->pressed[BTN_R])) {
        o->map = (o->map + (in->pressed[BTN_R] ? 1 : ml->nmaps - 1)) % ml->nmaps;
        o->nations = -1;
        float target = maps_top + 42 + 16 + ry[o->map];
        if (target < scroll || target + ch > scroll + view.h) scroll = target - 16;
    }
    return result;
}
