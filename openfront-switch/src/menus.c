// The web client's overlays and modals, with its layout and colours:
// PlayerInfoOverlay (the hover card at the top), PlayerPanel, the
// SendResourceModal, EmojiTable, the in-game menu (SettingsModal), the exit
// confirmation (ConfirmDialog) and WinModal.
#include "menus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "hud.h"
#include "json.h"
#include "map.h"
#include "ui.h"

// Tailwind v4 palette entries the modals use.
#define ZINC_50 0xfafafa
#define ZINC_500 0x71717b
#define ZINC_600 0x52525c
#define INDIGO_100 0xe0e7ff
#define INDIGO_200 0xc6d2ff
#define INDIGO_400 0x7c86ff
#define INDIGO_500 0x615fff
#define INDIGO_600 0x4f39f6
#define PURPLE_200 0xe9d4ff
#define PURPLE_400 0xc27aff
#define PURPLE_500 0xad46ff
#define EMERALD_200 0xa4f4cf
#define EMERALD_400 0x00d492
#define EMERALD_500 0x00bc7d
#define AMBER_200 0xfee685
#define AMBER_300 0xffd230
#define AMBER_500 0xfe9a00
#define RED_600 0xe7000b
#define SLATE_800 0x1d293d
#define YELLOW_500 0xf0b100
#define F59E0B 0xf59e0b

static struct {
    // Hover card
    int hover_req;
    int hover_tx, hover_ty;
    double hover_at;
    cJSON *hover;
    bool hover_visible;
    // PlayerPanel
    int panel_req;
    double panel_at;
    cJSON *panel;
    int panel_sid;
    // Send modal
    double send_amount;
    int send_pct;
    bool send_dragging;
    // Emoji table
    int emoji_req;
    cJSON *emojis;
    float emoji_scroll;
    // Quick chat
    int chat_req;
    cJSON *chat;
    int chat_sid;
    // Win modal
    int win_id_seen;
    char win_text[256];
    bool win_open;
    // Exit / settings
    bool alt_view;
} M;

// --- Small helpers -------------------------------------------------------------------

/** T() with one {param} substituted. */
static const char *Tp(const char *key, const char *param, const char *value) {
    static char out[512];
    const char *s = T(key);
    char pat[64];
    snprintf(pat, sizeof pat, "{%s}", param);
    const char *at = strstr(s, pat);
    if (!at) {
        snprintf(out, sizeof out, "%s", s);
        return out;
    }
    snprintf(out, sizeof out, "%.*s%s%s", (int)(at - s), s, value, at + strlen(pat));
    return out;
}

static SDL_Color hexa(unsigned v, float a) { return alpha(hex(v), a); }

/** The round ✕ close button (bg-zinc-700, hover red-500). */
static bool close_button(const char *id, float x, float y) {
    Rect r = {x, y, 28, 28};
    uint32_t uid = ui_id(id, 0);
    bool hover = ui_hover(uid, r);
    gfx_fill_circle(x + 14, y + 14, 14, hex(hover ? TW_RED_500 : TW_ZINC_700));
    // "✕" (Inter has no such glyph): two strokes.
    gfx_line(x + 9.5f, y + 9.5f, x + 18.5f, y + 18.5f, 1.6f, hex(TW_WHITE));
    gfx_line(x + 18.5f, y + 9.5f, x + 9.5f, y + 18.5f, 1.6f, hex(TW_WHITE));
    return ui_button(uid, r);
}

/** A full-screen backdrop; true when it was clicked (outside the dialog). */
static bool backdrop(const char *id, SDL_Color c) {
    Rect all = {0, 0, PAGE_W, PAGE_H};
    gfx_fill_rect(all, c);
    return ui_button(ui_id(id, 0), all);
}

static void ui_divider(float x, float y, float w) { gfx_fill_rect((Rect){x, y + 4, w, 1}, hexa(TW_ZINC_700, 0.8f)); }

// --- PlayerInfoOverlay ---------------------------------------------------------------

void menus_hover_tile(int tx, int ty, bool valid) {
    if (!valid) {
        M.hover_visible = false;
        return;
    }
    M.hover_visible = true;
    double now = app_now_ms();
    if ((tx != M.hover_tx || ty != M.hover_ty || now - M.hover_at > 500) && !M.hover_req && now - M.hover_at > 80) {
        char args[64];
        snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", tx, ty);
        M.hover_req = engine_call("hover", args, true);
        M.hover_tx = tx, M.hover_ty = ty;
        M.hover_at = now;
    }
}

static void shortcut_button(const char *id, int idx, float x, float y, const char *icon, const char *action, int sid) {
    Rect r = {x, y, 26, 26};
    uint32_t uid = ui_id(id, idx);
    bool hover = ui_hover(uid, r);
    if (hover) gfx_fill_rrect(r, 6, rgba(255, 255, 255, 26));
    gfx_stroke_rrect(r, 6, 1, hex(TW_GRAY_500));
    gfx_icon(icon, (Rect){x + 3, y + 3, 20, 20}, hex(TW_WHITE), ICON_NORMAL);
    if (ui_button(uid, r)) {
        if (!strcmp(action, "emoji")) app_open_modal(MODAL_EMOJI, sid);
        else if (!strcmp(action, "chat")) app_open_modal(MODAL_CHAT, sid);
        else {
            char args[96];
            snprintf(args, sizeof args, "{\"sid\":%d,\"action\":\"%s\"}", sid, action);
            engine_call("playerAction", args, false);
        }
        M.hover_visible = false;
    }
}

/** RelationSmiley: a 16px face in the relation's colour. */
static void smiley(float x, float y, bool good) {
    SDL_Color c = good ? hex(TW_GREEN_500) : hex(TW_RED_500);
    gfx_stroke_circle(x + 8, y + 8, 7.2f, 1.4f, c);
    gfx_fill_circle(x + 5.8f, y + 6.5f, 0.9f, c);
    gfx_fill_circle(x + 10.2f, y + 6.5f, 0.9f, c);
    for (int i = 0; i < 8; i++) {
        float a0 = 0.6f + 1.94f * (float)i / 8, a1 = 0.6f + 1.94f * (float)(i + 1) / 8;
        float r = 3.2f, cy = good ? y + 8.5f : y + 13.5f;
        float s = good ? 1 : -1;
        gfx_line(x + 8 - cosf(a0) * r, cy + s * sinf(a0) * r * 0.8f, x + 8 - cosf(a1) * r, cy + s * sinf(a1) * r * 0.8f, 1.4f, c);
    }
}

static void hover_card(void) {
    const cJSON *h = M.hover;
    if (!M.hover_visible || !cJSON_IsObject(h)) return;
    if (!strcmp(jstr(h, "kind", ""), "unit")) {
        // renderUnitInfo(): owner, type, health, troops.
        float w = 500, x = PAGE_W / 2.0f - w / 2;
        float hh = 8 + 24 + 4 + 20 + (jnull(h, "health") ? 0 : 20) + (jnull(h, "troops") ? 0 : 20) + 8;
        Rect card = {x, 0, w, hh};
        gfx_fill_rrect4(card, 0, 0, 8, 8, alpha(hex(TW_GRAY_800), 0.92f));
        ui_block(card);
        float y = 8;
        gfx_text(FONT_700, 16, jstr(h, "owner", ""), x + 8, y, 24, jbool(h, "ally") ? hex(TW_GREEN_500) : hex(TW_WHITE), ALIGN_LEFT);
        y += 28;
        gfx_text(FONT_400, 14, jstr(h, "type", ""), x + 8, y, 20, rgba(255, 255, 255, 204), ALIGN_LEFT);
        y += 20;
        char line[64];
        if (!jnull(h, "health")) {
            snprintf(line, sizeof line, "Health: %d", (int)jnum(h, "health", 0));
            gfx_text(FONT_400, 14, line, x + 8, y, 20, hex(TW_WHITE), ALIGN_LEFT);
            y += 20;
        }
        if (!jnull(h, "troops")) {
            snprintf(line, sizeof line, "Troops: %s", jstr(h, "troops", ""));
            gfx_text(FONT_400, 14, line, x + 8, y, 20, hex(TW_WHITE), ALIGN_LEFT);
        }
        return;
    }
    int sid = (int)jnum(h, "sid", 0);
    bool me = jbool(h, "me");
    const float W = 598, L = PAGE_W / 2.0f - W / 2;
    Rect card = {L, 0, W, 70};
    gfx_fill_rrect4(card, 0, 0, 8, 8, alpha(hex(TW_GRAY_800), 0.92f));
    ui_block(card);

    // Left: chat and emoji shortcuts.
    if (!me && jbool(h, "canChat")) shortcut_button("hov-sc", 0, L + 6, 7, "ChatIconWhite", "chat", sid);
    shortcut_button("hov-sc", 1, L + 6, me ? 22 : 37, "EmojiIconWhite", "emoji", sid);

    // Gold, attacking troops and the troop bar.
    float cx = L + 40;
    const char *gold = jstr(h, "gold", "0");
    float gw = gfx_text_width(FONT_700, 14, gold);
    Rect gbox = {cx, 7, 4 + 13 + 4 + 2 + gw + 2 + 4 + 2, 26};
    gfx_stroke_rrect(gbox, 6, 1, hex(TW_YELLOW_400));
    gfx_icon("GoldCoinIcon", (Rect){cx + 5, 14, 13, 13}, hex(TW_WHITE), ICON_NORMAL);
    gfx_text(FONT_700, 14, gold, cx + 22, 10, 20, hex(TW_YELLOW_400), ALIGN_LEFT);
    bool attacking = jbool(h, "attackingActive");
    SDL_Color ac = attacking ? hex(AQUARIUS) : rgba(255, 255, 255, 102);
    float acx = (gbox.x + gbox.w + 4 + cx + 144) / 2;
    gfx_icon(attacking ? "SoldierIconAquarius" : "SoldierIcon", (Rect){acx - 11, 7, 10, 10}, attacking ? hex(TW_WHITE) : ac,
             attacking ? ICON_NORMAL : ICON_SILHOUETTE);
    gfx_text_shadow(FONT_700, 12, "↑", acx, 6, 12, ac, ALIGN_LEFT);
    gfx_text_shadow(FONT_700, 14, jstr(h, "attacking", "0"), acx, 20, 14, ac, ALIGN_CENTER);
    Rect bar = {cx, 38, 144, 24};
    hud_troop_bar(bar, (float)jnum(h, "troopFrac", 0), (float)jnum(h, "attackFrac", 0), hex(TW_SKY_700), hex(MALIBU_BLUE));
    gfx_text_shadow(FONT_700, 14, jstr(h, "troops", ""), cx + 7, 42, 16, hex(TW_WHITE), ALIGN_LEFT);
    gfx_text_shadow(FONT_700, 14, jstr(h, "max", ""), cx + 137, 42, 16, hex(TW_WHITE), ALIGN_RIGHT);
    gfx_icon("SoldierIcon", (Rect){cx + 65, 43, 14, 14}, hex(TW_WHITE), ICON_SILHOUETTE);

    // Right: identity row and unit counts.
    float rx = L + 192, rw = 336;
    float x = rx;
    const char *flag = jstr(h, "flag", "");
    if (*flag && gfx_flag(flag, (Rect){x, 8, 36, 24})) x += 36 + 8;
    SDL_Color name_c = jbool(h, "friendly") ? hex(TW_GREEN_500) : hex(TW_WHITE);
    const char *name = jstr(h, "name", "");
    const char *type = jstr(h, "type", "");
    const cJSON *icons = jget(h, "icons");
    int nicons = jlen(icons);
    // getNameFontSize(): shrink long names to fit the row (min 0.65).
    float pressure = (float)strlen(type) * (12.0f / 18) + 8 / 10.8f + (float)nicons * 2.0f + (nicons ? 0.5f : 0) +
                     (*flag ? 3.5f : 0) + (jnull(h, "traitor") ? 0 : 4.2f) + (jnull(h, "alliance") ? 0 : 6.7f);
    int len = 0;
    for (const char *p = name; *p; p++) len += ((unsigned char)*p & 0xc0) != 0x80;
    float scale = len > 0 ? (31.1f - pressure) / (float)len : 1;
    float fs = 18 * fminf(1, fmaxf(0.65f, scale));
    char buf[128];
    float name_max = rx + rw - x - gfx_text_width(FONT_MONO, 12, type) - 8 - 20 * (float)nicons;
    gfx_ellipsize(FONT_MONO_BOLD, fs, name, fmaxf(60, name_max), buf, sizeof buf);
    x += gfx_text(FONT_MONO_BOLD, fs, buf, x, 11, 22, name_c, ALIGN_LEFT) + 8;
    const char *rel = jstr(h, "relation", "");
    if (*rel) {
        smiley(x, 12, !strcmp(rel, "good"));
        x += 16 + 8;
    }
    x += gfx_text(FONT_MONO, 12, type, x, 12, 16, hex(TW_GRAY_400), ALIGN_LEFT) + 8;
    for (int i = 0; i < nicons; i++) {
        const cJSON *it = jat(icons, i);
        const char *ic = cJSON_IsString(it) ? it->valuestring : "";
        const char *img = !strcmp(ic, "crown") ? "CrownIcon"
                          : !strcmp(ic, "disconnected") ? "DisconnectedIcon"
                          : !strcmp(ic, "allianceRequest") ? "AllianceRequestWhiteIcon"
                          : !strcmp(ic, "target") ? "TargetIcon"
                          : !strcmp(ic, "embargo") ? "EmbargoWhiteIcon"
                          : !strcmp(ic, "nukeRed") ? "NukeIconRed"
                          : !strcmp(ic, "nukeWhite") ? "NukeIconWhite"
                                                     : NULL;
        if (img) {
            gfx_icon(img, (Rect){x, 12, 16, 16}, hex(TW_WHITE), ICON_NORMAL);
            x += 20;
        }
    }
    const char *emoji = jstr(h, "emoji", NULL);
    if (emoji) x += gfx_text(FONT_MONO, 14, emoji, x, 10, 20, hex(TW_WHITE), ALIGN_LEFT) + 4;
    const char *traitor = jstr(h, "traitor", NULL);
    if (traitor) {
        float tw = gfx_text_width(FONT_400, 14, traitor);
        float tx = rx + rw - tw - 20 - (jnull(h, "alliance") ? 0 : 70);
        gfx_icon("TraitorIcon", (Rect){tx, 12, 16, 16}, hex(TW_WHITE), ICON_NORMAL);
        gfx_text(FONT_400, 14, traitor, tx + 18, 10, 20, hex(TW_RED_900), ALIGN_LEFT);
    }
    const char *alliance = jstr(h, "alliance", NULL);
    if (alliance) {
        float aw = gfx_text_width(FONT_700, 12, alliance);
        float ax = rx + rw - aw - 22;
        gfx_icon("AllianceIcon", (Rect){ax, 11, 18, 18}, hex(TW_WHITE), ICON_NORMAL);
        gfx_text(FONT_700, 12, alliance, ax + 22, 12, 16, hex(TW_WHITE), ALIGN_LEFT);
    }
    const cJSON *units = jget(h, "units");
    for (int i = 0; i < jlen(units); i++) {
        const cJSON *u = jat(units, i);
        Rect b = {rx + 52 * (float)i, 36, 48, 28};
        gfx_stroke_rrect(b, 6, 1, hex(TW_GRAY_500));
        char n[16];
        snprintf(n, sizeof n, "%d", (int)jnum(u, "n", 0));
        float nw = gfx_text_width(FONT_400, 12, n);
        float bx = b.x + (48 - 16 - 4 - nw) / 2;
        gfx_icon(jstr(u, "icon", ""), (Rect){bx, 42, 16, 16}, hex(TW_WHITE), ICON_NORMAL);
        gfx_text(FONT_400, 12, n, bx + 20, 42, 16, hex(TW_WHITE), ALIGN_LEFT);
    }

    // Right: what I can do to this player.
    const cJSON *a = jget(h, "actions");
    if (cJSON_IsObject(a)) {
        const char *icons_[4], *acts[4];
        int n = 0;
        if (jbool(a, "requestAlliance")) icons_[n] = "AllianceIconWhite", acts[n++] = "alliance";
        else if (jbool(a, "breakAlliance")) icons_[n] = "TraitorIconWhite", acts[n++] = "breakAlliance";
        if (jbool(a, "target")) icons_[n] = "TargetIconWhite", acts[n++] = "target";
        if (jbool(a, "boat")) icons_[n] = "BoatIconWhite", acts[n++] = "boat";
        int cols = (n + 1) / 2;
        for (int i = 0; i < n; i++) {
            int col = i / 2, row = i % 2;
            float bx = L + W - 6 - 26 * (float)(cols - col) - 4 * (float)(cols - 1 - col);
            shortcut_button("hov-act", i, bx, 7 + 30 * (float)row, icons_[i], acts[i], sid);
        }
    }
}

// --- PlayerPanel ---------------------------------------------------------------------

static void request_panel(void) {
    char args[32];
    snprintf(args, sizeof args, "{\"sid\":%d}", app.modal_player);
    M.panel_req = engine_call("panel", args, true);
    M.panel_at = app_now_ms();
    M.panel_sid = app.modal_player;
}

typedef enum { BTN_NORMAL, BTN_RED, BTN_GREEN, BTN_YELLOW, BTN_INDIGO } BtnType;

/** components/ui/ActionButton.ts: icon over a label in a bordered tile. */
static bool action_button(int idx, Rect r, const char *icon, const char *label, BtnType type, bool disabled) {
    uint32_t id = ui_id("panel-act", idx);
    bool hover = ui_hover(id, r) && !disabled;
    static const unsigned text[] = {0, TW_RED_400, EMERALD_400, F59E0B, INDIGO_400};
    SDL_Color tc = type == BTN_NORMAL ? rgba(255, 255, 255, 230) : hex(text[type]);
    SDL_Color hb = type == BTN_NORMAL ? rgba(255, 255, 255, 26)
                   : type == BTN_RED ? hexa(TW_RED_500, 0.1f)
                   : type == BTN_GREEN ? hexa(EMERALD_500, 0.1f)
                   : type == BTN_YELLOW ? hexa(F59E0B, 0.1f)
                                        : hexa(INDIGO_500, 0.1f);
    float op = disabled ? 0.5f : 1;
    gfx_box(r, 8, hover ? hb : rgba(255, 255, 255, 10), 1, rgba(255, 255, 255, 26));
    float isz = hover ? 22 : 20;
    gfx_icon(icon, (Rect){r.x + r.w / 2 - isz / 2, r.y + 7 + 10 - isz / 2, isz, isz}, rgba(255, 255, 255, (int)(255 * op)), ICON_NORMAL);
    char lines[512];
    int n = gfx_wrap(FONT_600, 14, label, r.w - 12, lines, sizeof lines);
    const char *l = lines;
    for (int i = 0; i < n && i < 2; i++) {
        gfx_text(FONT_600, 14, l, r.x + r.w / 2, r.y + 31 + 20 * (float)i, 20, alpha(tc, op * tc.a / 255.0f), ALIGN_CENTER);
        l += strlen(l) + 1;
    }
    return ui_button(id, r) && !disabled;
}

static void panel_action(const char *action) {
    char args[96];
    snprintf(args, sizeof args, "{\"sid\":%d,\"action\":\"%s\"}", app.modal_player, action);
    engine_call("playerAction", args, false);
}

/** A row of equal-width action buttons (grid auto-cols-fr gap-1). */
typedef struct {
    const char *icon, *label, *action;
    BtnType type;
    bool disabled;
} Act;

/** Height of a row of action buttons: 58, or 78 when a label wraps. */
static float action_row_h(float w, const Act *acts, int n) {
    if (n == 0) return 0;
    float bw = (w - 4 * (float)(n - 1)) / (float)n;
    for (int i = 0; i < n; i++) {
        if (gfx_text_width(FONT_600, 14, acts[i].label) > bw - 12) return 78;
    }
    return 58;
}

static float action_row(float x, float y, float w, const Act *acts, int n, int base) {
    if (n == 0) return 0;
    float bw = (w - 4 * (float)(n - 1)) / (float)n;
    float h = action_row_h(w, acts, n);
    for (int i = 0; i < n; i++) {
        Rect r = {x + (bw + 4) * (float)i, y, bw, h};
        if (action_button(base + i, r, acts[i].icon, acts[i].label, acts[i].type, acts[i].disabled)) {
            const char *a = acts[i].action;
            if (!strcmp(a, "emoji")) app_open_modal(MODAL_EMOJI, app.modal_player);
            else if (!strcmp(a, "chat")) app_open_modal(MODAL_CHAT, app.modal_player);
            else if (!strcmp(a, "sendTroops") || !strcmp(a, "sendGold")) {
                app.send_troops = !strcmp(a, "sendTroops");
                M.send_amount = -1;
                app_open_modal(MODAL_SEND, app.modal_player);
            } else {
                panel_action(a);
                if (strcmp(a, "rocketDirection") != 0) app_open_modal(MODAL_NONE, -1);
            }
        }
    }
    return h;
}

static SDL_Color expiry_color(double secs) {
    if (secs <= 30) return hex(TW_RED_400);
    if (secs <= 60) return hex(TW_YELLOW_400);
    return hex(EMERALD_400);
}

/** Lays out (draw=false: measures) the panel body; returns its height. */
static float panel_body(const cJSON *p, float X, float Y, float W, bool draw) {
    float y = Y;
    // Identity: flag, name, type chip.
    const char *flag = jstr(p, "flag", "");
    float x = X;
    if (draw && *flag) {
        gfx_fill_circle(X + 20, y + 20, 20, hex(TW_ZINC_800));
        gfx_flag(flag, (Rect){X, y + 6, 40, 28});
    }
    if (*flag) x += 40 + 10;
    const char *type_s = jstr(p, "typeLabel", "");
    const char *ptype = jstr(p, "type", "HUMAN");
    bool is_nation = !strcmp(ptype, "NATION"), is_bot = !strcmp(ptype, "BOT");
    bool chip = is_nation || is_bot;
    float chip_w = chip ? 8 + gfx_text_width(FONT_600, 12, is_nation ? "🏛️" : "⚔️") + 6 + gfx_text_width(FONT_600, 12, type_s) + 8 + 2 : 0;
    char name[128];
    gfx_ellipsize(FONT_700, 20, jstr(p, "name", ""), X + W - x - (chip ? chip_w + 10 : 0), name, sizeof name);
    if (draw) {
        gfx_text(FONT_700, 20, name, x, y, 28, hex(ZINC_50), ALIGN_LEFT);
        if (chip) {
            Rect c = {X + W - chip_w, y + 3, chip_w, 22};
            unsigned base = is_nation ? INDIGO_500 : PURPLE_500;
            gfx_box(c, 11, hexa(base, 0.1f), 1, hexa(is_nation ? INDIGO_400 : PURPLE_400, 0.25f));
            SDL_Color tc = hex(is_nation ? INDIGO_200 : PURPLE_200);
            float cx = c.x + 9;
            cx += gfx_text(FONT_600, 12, is_nation ? "🏛️" : "⚔️", cx, c.y + 3, 16, tc, ALIGN_LEFT) + 6;
            gfx_text(FONT_600, 12, type_s, cx, c.y + 3, 16, tc, ALIGN_LEFT);
        }
    }
    y += *flag ? 40 : 28;
    const char *traitor = jstr(p, "traitor", NULL);
    if (traitor) {
        y += 4;
        if (draw) {
            const char *lbl = T("player_panel.traitor");
            float w = 10 + 18 + 8 + gfx_text_width(FONT_600, 14, lbl) + (*traitor ? 8 + 4 + 8 + gfx_text_width(FONT_700, 14, traitor) : 0) + 10;
            Rect b = {X, y, w, 26};
            gfx_box(b, 13, hexa(TW_RED_500, 0.1f), 1, hexa(TW_RED_400, 0.3f));
            float bx = X + 10;
            gfx_icon("TraitorIconLightRed", (Rect){bx, y + 4, 18, 18}, hex(TW_WHITE), ICON_NORMAL);
            bx += 26;
            bx += gfx_text(FONT_600, 14, lbl, bx, y + 3, 20, hex(TW_RED_200), ALIGN_LEFT) + 8;
            if (*traitor) {
                gfx_fill_circle(bx + 2, y + 13, 2, hexa(TW_RED_400, 0.7f));
                gfx_text(FONT_700, 14, traitor, bx + 12, y + 3, 20, hex(0xffe2e2), ALIGN_LEFT);
            }
        }
        y += 26;
    }
    const char *relation = jstr(p, "relation", NULL);
    if (relation) {
        y += 4;
        if (draw) {
            const char *lbl = jstr(p, "relationLabel", "");
            unsigned bc = TW_ZINC_400, bg = ZINC_500, tc = TW_ZINC_200;
            if (!strcmp(relation, "hostile")) bc = TW_RED_400, bg = TW_RED_500, tc = TW_RED_200;
            else if (!strcmp(relation, "distrustful")) bc = TW_RED_300, bg = TW_RED_300, tc = TW_RED_300;
            else if (!strcmp(relation, "friendly")) bc = EMERALD_400, bg = EMERALD_500, tc = EMERALD_200;
            float w = 20 + gfx_text_width(FONT_600, 14, lbl) + 2;
            gfx_box((Rect){X, y, w, 24}, 12, hexa(bg, 0.1f), 1, hexa(bc, 0.3f));
            gfx_text(FONT_600, 14, lbl, X + 11, y + 2, 20, hex(tc), ALIGN_LEFT);
        }
        y += 24;
    }
    y += 4 + 8;  // mb-1, gap-2
    if (draw) ui_divider(X, y, W);
    y += 9 + 8;
    // Resources: 💰 gold, 🛡️ troops.
    if (draw) {
        for (int i = 0; i < 2; i++) {
            Rect b = {i == 0 ? X : X + W - 140, y, 140, 36};
            gfx_fill_rrect(b, 8, rgba(255, 255, 255, 10));
            float bx = b.x + 12;
            bx += gfx_text(FONT_400, 14.5f, i == 0 ? "💰" : "🛡️", bx, y + 6, 24, hex(TW_WHITE), ALIGN_LEFT) + 2 + 6;
            gfx_text(FONT_600, 14.5f, jstr(p, i == 0 ? "gold" : "troops", "0"), bx, y + 6, 24, hex(TW_WHITE), ALIGN_LEFT);
            bx += 5 * 8.7f + 6;
            gfx_text(FONT_400, 14.5f, T(i == 0 ? "player_panel.gold" : "player_panel.troops"), bx, y + 6, 24, hex(TW_ZINC_200),
                     ALIGN_LEFT);
        }
    }
    y += 36 + 8;
    bool self = jbool(p, "self"), spectator = jbool(p, "spectator");
    if (self && !spectator) {
        // Rocket direction toggle.
        if (draw) ui_divider(X, y, W);
        y += 9 + 8;
        Rect b = {X, y, W, 48};
        uint32_t id = ui_id("panel-rocket", 0);
        bool hover = draw && ui_hover(id, b);
        if (draw) {
            gfx_fill_rrect(b, 12, rgba(255, 255, 255, hover ? 20 : 13));
            gfx_text(FONT_600, 14, T("player_panel.flip_rocket_trajectory"), X + 12, y + 6, 20, hex(TW_WHITE), ALIGN_LEFT);
            gfx_text(FONT_400, 12, T(jbool(p, "rocketUp") ? "player_panel.arc_up" : "player_panel.arc_down"), X + 12, y + 26, 16,
                     hex(TW_ZINC_300), ALIGN_LEFT);
            gfx_text(FONT_400, 18, "🔀", X + W - 12, y + 10, 28, hex(TW_WHITE), ALIGN_RIGHT);
            if (ui_button(id, b)) {
                panel_action("rocketDirection");
                request_panel();
            }
        }
        y += 48 + 8;
    }
    if (draw) ui_divider(X, y, W);
    y += 9 + 8;
    // Stats: betrayals, trading.
    if (draw) {
        gfx_text(FONT_500, 15, "⚠️", X, y + 1, 21, hex(TW_ZINC_100), ALIGN_LEFT);
        gfx_text(FONT_500, 15, T("player_panel.betrayals"), X + 27, y + 1, 21, hex(TW_ZINC_100), ALIGN_LEFT);
        char n[16];
        snprintf(n, sizeof n, "%d", (int)jnum(p, "betrayals", 0));
        gfx_text(FONT_600, 14, n, X + W, y + 1, 21, hex(TW_ZINC_200), ALIGN_RIGHT);
    }
    y += 23 + 8;
    if (draw) {
        gfx_text(FONT_500, 15, "⚓", X, y + 1, 21, hex(TW_ZINC_100), ALIGN_LEFT);
        gfx_text(FONT_500, 15, T("player_panel.trading"), X + 27, y + 1, 21, hex(TW_ZINC_100), ALIGN_LEFT);
        bool trading = jbool(p, "trading");
        gfx_text(FONT_600, 14, T(trading ? "player_panel.active" : "player_panel.stopped"), X + W, y + 1, 21,
                 trading ? hex(TW_BLUE_400) : hex(TW_AMBER_400), ALIGN_RIGHT);
    }
    y += 23 + 8;
    if (draw) ui_divider(X, y, W);
    y += 9 + 8;
    // Alliances.
    const cJSON *allies = jget(p, "allies");
    int na = jlen(allies);
    if (draw) {
        gfx_text(FONT_500, 15, T("player_panel.alliances"), X, y, 24, hex(TW_ZINC_200), ALIGN_LEFT);
        char n[16];
        snprintf(n, sizeof n, "%d", na);
        float nw = fmaxf(20, gfx_text_width(FONT_400, 12, n) + 12);
        gfx_box((Rect){X + W - nw, y + 3, nw, 20}, 10, rgba(255, 255, 255, 26), 1, rgba(255, 255, 255, 51));
        gfx_text(FONT_400, 12, n, X + W - nw / 2, y + 5, 16, hex(TW_ZINC_100), ALIGN_CENTER);
    }
    y += 24 + 8;
    // ul p-2 flex-wrap gap-1.5: chips of name (+ remaining time).
    float list_h = 16 + 23;
    if (na > 0) {
        float cx = X + 8, cy = y + 8;
        for (int i = 0; i < na; i++) {
            const cJSON *a = jat(allies, i);
            const char *t = jstr(a, "text", "");
            float w = 10 + gfx_text_width(FONT_400, 14, jstr(a, "name", "")) + (*t ? 6 + gfx_text_width(FONT_600, 11, t) : 0) + 10 + 2;
            w = fminf(w, W - 16);
            if (cx + w > X + W - 8 && cx > X + 8) cx = X + 8, cy += 30 + 6;
            cx += w + 6;
        }
        list_h = fminf(cy - y + 30 + 8, 192);
    }
    if (draw) {
        Rect box = {X, y, W, list_h};
        gfx_box(box, 8, hexa(TW_ZINC_800, 0.7f), 1, hexa(TW_ZINC_700, 0.6f));
        gfx_clip(&box);
        if (na == 0) {
            gfx_text(FONT_400, 14, T("common.none"), X + 12, y + 8, 23, hex(TW_ZINC_400), ALIGN_LEFT);
        } else {
            float cx = X + 8, cy = y + 8;
            for (int i = 0; i < na; i++) {
                const cJSON *a = jat(allies, i);
                const char *t = jstr(a, "text", "");
                char nm[96];
                float tw = *t ? 6 + gfx_text_width(FONT_600, 11, t) : 0;
                gfx_ellipsize(FONT_400, 14, jstr(a, "name", ""), W - 16 - 22 - tw, nm, sizeof nm);
                float w = 10 + gfx_text_width(FONT_400, 14, nm) + tw + 10 + 2;
                if (cx + w > X + W - 8 && cx > X + 8) cx = X + 8, cy += 30 + 6;
                Rect c = {cx, cy, w, 30};
                gfx_box(c, 6, rgba(255, 255, 255, 13), 1, rgba(255, 255, 255, 26));
                float tx = cx + 11 + gfx_text(FONT_400, 14, nm, cx + 11, cy + 4, 22, hex(TW_ZINC_100), ALIGN_LEFT);
                if (*t) gfx_text(FONT_600, 11, t, tx + 6, cy + 8, 14, expiry_color(jnum(a, "secs", 0)), ALIGN_LEFT);
                cx += w + 6;
            }
        }
        gfx_clip(NULL);
    }
    y += list_h;
    const char *expiry = jstr(p, "allianceExpiry", NULL);
    if (expiry) {
        y += 8;
        if (draw) {
            gfx_text(FONT_600, 16, T("player_panel.alliance_time_remaining"), X, y, 24, hex(TW_ZINC_300), ALIGN_LEFT);
            gfx_text(FONT_700, 14, expiry, X + W - 8, y + 2, 20, expiry_color(jnum(p, "allianceSecs", 0)), ALIGN_RIGHT);
        }
        y += 24;
    }
    if (spectator) return y - Y;
    y += 8;
    if (draw) ui_divider(X, y, W);
    y += 9 + 8;
    // Actions.
    const cJSON *can = jget(p, "can");
    Act row1[5];
    int n1 = 0;
    row1[n1++] = (Act){"ChatIconWhite", T("player_panel.chat"), "chat", BTN_NORMAL, false};
    if (jbool(can, "emoji")) row1[n1++] = (Act){"EmojiIconWhite", T("player_panel.emotes"), "emoji", BTN_NORMAL, false};
    if (jbool(can, "target")) row1[n1++] = (Act){"TargetIconWhite", T("player_panel.target"), "target", BTN_NORMAL, false};
    if (jbool(can, "donateTroops")) row1[n1++] = (Act){"DonateTroopIconWhite", T("player_panel.troops"), "sendTroops", BTN_NORMAL, false};
    if (jbool(can, "donateGold")) row1[n1++] = (Act){"DonateGoldIconWhite", T("player_panel.gold"), "sendGold", BTN_NORMAL, false};
    if (draw) action_row(X, y, W, row1, n1, 0);
    y += action_row_h(W, row1, n1) + 10;
    if (draw) ui_divider(X, y - 2, W);
    y += 9 + 10;
    Act row2[3];
    int n2 = 0;
    if (!self) {
        if (jbool(can, "embargo")) row2[n2++] = (Act){"StopIconWhite", T("player_panel.stop_trade"), "embargo", BTN_YELLOW, false};
        else row2[n2++] = (Act){"TradingIconWhite", T("player_panel.start_trade"), "stopEmbargo", BTN_GREEN, false};
        if (jbool(can, "breakAlliance"))
            row2[n2++] = (Act){"TraitorIconWhite", T("player_panel.break_alliance"), "breakAlliance", BTN_RED, false};
        if (jbool(can, "sendAlliance"))
            row2[n2++] = (Act){"AllianceIconWhite", T("player_panel.send_alliance"), "alliance", BTN_INDIGO, false};
    } else {
        bool ok = jbool(can, "embargoAll");
        static char l1[128], l2[128];
        snprintf(l1, sizeof l1, "%s%s", T("player_panel.stop_trade_enemies"), ok ? "" : " ⏳");
        snprintf(l2, sizeof l2, "%s%s", T("player_panel.start_trade_enemies"), ok ? "" : " ⏳");
        row2[n2++] = (Act){"StopIconWhite", l1, "embargoAll", BTN_YELLOW, !ok};
        row2[n2++] = (Act){"TradingIconWhite", l2, "stopEmbargoAll", BTN_GREEN, !ok};
    }
    if (draw) action_row(X, y, W, row2, n2, 10);
    y += action_row_h(W, row2, n2);
    return y - Y;
}

static void player_panel(void) {
    const cJSON *p = M.panel;
    if (backdrop("panel-bg", rgba(0, 0, 0, 38))) {
        app_open_modal(MODAL_NONE, -1);
        return;
    }
    if (!cJSON_IsObject(p) || M.panel_sid != app.modal_player) return;
    const float W = 368, inner = 320;
    float body = panel_body(p, 0, 0, inner, false);
    float h = 16 + 24 + body + 24;
    float maxh = PAGE_H * 0.9f - 16;
    if (h > maxh) h = maxh;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - h / 2, W, h};
    gfx_shadow(card, 16, 16, rgba(0, 0, 0, 120));
    gfx_fill_rrect(card, 16, hexa(TW_ZINC_900, 0.95f));
    if (jstr(p, "traitor", NULL)) gfx_stroke_rrect(card, 16, 2, rgba(239, 68, 68, 90));
    else gfx_stroke_rrect(card, 16, 1, rgba(255, 255, 255, 13));
    ui_block(card);
    gfx_clip(&card);
    panel_body(p, card.x + 24, card.y + 16 + 24, inner, true);
    gfx_clip(NULL);
    if (close_button("panel-close", card.x + W - 40, card.y + 12)) app_open_modal(MODAL_NONE, -1);
}

// --- SendResourceModal -----------------------------------------------------------------

static void send_modal(void) {
    const cJSON *p = M.panel;
    bool troops = app.send_troops;
    if (backdrop("send-bg", rgba(0, 0, 0, 153))) {
        app_open_modal(MODAL_PANEL, app.modal_player);
        return;
    }
    if (!cJSON_IsObject(p)) return;
    double total = troops ? jnum(p, "myTroops", 0) : jnum(p, "myGold", 0);
    double cap = troops ? jnum(p, "targetCap", -1) : -1;
    if (M.send_amount < 0) {
        M.send_pct = 50;
        M.send_amount = floor(total * 0.5);
    }
    char fmt_total[32], fmt_send[32], fmt_keep[32];
    double allowed = cap >= 0 ? fmin(M.send_amount, cap) : M.send_amount;
    if (allowed > total) allowed = total;
    if (troops) {
        format_troops(total, fmt_total, sizeof fmt_total);
        format_troops(allowed, fmt_send, sizeof fmt_send);
        format_troops(fmax(0, total - allowed), fmt_keep, sizeof fmt_keep);
    } else {
        format_number(total, fmt_total, sizeof fmt_total);
        format_number(allowed, fmt_send, sizeof fmt_send);
        format_number(fmax(0, total - allowed), fmt_keep, sizeof fmt_keep);
    }
    const float W = 540;
    float h = 20 + 28 + 12 + 24 + 12 + 16 + 40 + 32 + 16 + (cap >= 0 ? 24 : 0) + 12 + 20 + 20 + 40 + 20;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - h / 2, W, h};
    gfx_fill_rrect(card, 16, hex(TW_ZINC_900));
    gfx_stroke_rrect(card, 16, 1, hex(TW_ZINC_800));
    ui_block(card);
    float x = card.x + 20, y = card.y + 20, w = W - 40;
    gfx_text(FONT_600, 18, Tp(troops ? "send_troops_modal.title_with_name" : "send_gold_modal.title_with_name", "name", jstr(p, "name", "")),
             x, y, 28, hex(TW_ZINC_100), ALIGN_LEFT);
    if (close_button("send-close", card.x + W - 20 - 16, card.y + 8)) {
        app_open_modal(MODAL_PANEL, app.modal_player);
        return;
    }
    y += 28 + 12;
    // Available chip.
    const char *avail = T("common.available");
    float cw = 8 + gfx_text_width(FONT_400, 13, avail) + 4 + gfx_text_width(FONT_MONO, 13, fmt_total) + 8;
    gfx_box((Rect){x, y, cw, 22}, 11, hexa(INDIGO_600, 0.15f), 1, hexa(INDIGO_400, 0.4f));
    float ax = x + 8 + gfx_text(FONT_400, 13, avail, x + 8, y + 2, 18, hexa(INDIGO_100, 0.9f), ALIGN_LEFT) + 4;
    gfx_text(FONT_MONO, 13, fmt_total, ax, y + 2, 18, hex(INDIGO_100), ALIGN_LEFT);
    y += 24 + 12;
    gfx_fill_rect((Rect){x, y, w, 1}, hex(TW_ZINC_800));
    y += 16;
    // Presets 10 25 50 75 Max.
    static const int presets[5] = {10, 25, 50, 75, 100};
    float pw = (w - 8 * 4) / 5;
    for (int i = 0; i < 5; i++) {
        Rect b = {x + (pw + 8) * (float)i, y, pw, 36};
        uint32_t id = ui_id("send-preset", i);
        bool hover = ui_hover(id, b);
        bool active = M.send_pct == presets[i];
        gfx_box(b, 8, active ? hex(INDIGO_600) : hover ? hex(TW_ZINC_700) : hex(TW_ZINC_800), 1,
                active ? hexa(TW_INDIGO_300, 0.6f) : hex(TW_ZINC_700));
        char l[16];
        if (presets[i] == 100) snprintf(l, sizeof l, "%s", T("common.preset_max"));
        else snprintf(l, sizeof l, "%d%%", presets[i]);
        gfx_text(FONT_400, 14, l, b.x + pw / 2, y + 8, 20, active ? hex(TW_WHITE) : hex(TW_ZINC_200), ALIGN_CENTER);
        if (ui_button(id, b)) {
            M.send_pct = presets[i];
            M.send_amount = floor(total * presets[i] / 100.0);
        }
    }
    y += 40 + 32;
    // Slider with its tooltip.
    SDL_Color fill = troops ? rgba(168, 85, 247, 255) : rgba(234, 179, 8, 255);
    Rect track = {x + 4, y, w - 8, 8};
    float pct = total > 0 ? (float)(M.send_amount / total) : 0;
    uint32_t sid = ui_id("send-slider", 0);
    Rect hit = {track.x - 9, y - 12, track.w + 18, 32};
    bool hover = ui_hover(sid, hit);
    const Pointer *ptr = ui_pointer();
    if (hover && ptr->pressed) M.send_dragging = true;
    if (!ptr->down) M.send_dragging = false;
    if (M.send_dragging) {
        pct = fmaxf(0, fminf(1, (ptr->x - track.x) / track.w));
        double raw = floor(total * pct);
        M.send_pct = (int)lround(pct * 100);
        M.send_amount = cap >= 0 ? fmin(raw, cap) : raw;
        pct = total > 0 ? (float)(M.send_amount / total) : 0;
    }
    gfx_fill_rrect(track, 4, rgba(255, 255, 255, 56));
    gfx_fill_rrect((Rect){track.x, y, track.w * pct, 8}, 4, fill);
    float tx = track.x + track.w * pct;
    gfx_fill_circle(tx, y + 4, 9, hex(TW_ZINC_900));
    gfx_fill_circle(tx, y + 4, 6, fill);
    char tip[64];
    snprintf(tip, sizeof tip, "%d%% • %s", (int)lround(pct * 100), fmt_send);
    float tw = gfx_text_width(FONT_400, 12, tip) + 12;
    Rect tr = {tx - tw / 2, y - 26, tw, 20};
    gfx_box(tr, 4, hex(0x0f1116), 1, hex(TW_ZINC_700));
    gfx_text(FONT_400, 12, tip, tx, tr.y + 2, 16, hex(TW_ZINC_100), ALIGN_CENTER);
    if (cap >= 0 && total > 0) {
        float cp = (float)fmin(1, cap / total);
        float cx = track.x + track.w * cp;
        gfx_fill_rect((Rect){cx - 1, y - 2, 2, 12}, hexa(TW_AMBER_400, 0.8f));
        const char *cl = T("common.cap_label");
        float cw2 = gfx_text_width(FONT_400, 11, cl) + 8;
        gfx_box((Rect){cx - cw2 / 2, y + 14, cw2, 18}, 4, hex(0x0f1116), 1, hexa(TW_AMBER_400, 0.4f));
        gfx_text(FONT_400, 11, cl, cx, y + 16, 14, hex(AMBER_200), ALIGN_CENTER);
    }
    y += 16 + (cap >= 0 ? 24 : 0);
    if (troops && allowed < M.send_amount) {
        gfx_text(FONT_400, 12, Tp("send_troops_modal.capacity_note", "amount", fmt_send), x, y, 16, hex(AMBER_300), ALIGN_LEFT);
    }
    y += 12;
    // Summary: Send X · Keep Y.
    const char *ls = T("common.send"), *lk = T("common.summary_keep");
    float sw = gfx_text_width(FONT_400, 14, ls) + 4 + gfx_text_width(FONT_MONO_BOLD, 14, fmt_send) + 4 +
               gfx_text_width(FONT_400, 14, "·") + 4 + gfx_text_width(FONT_400, 14, lk) + 4 + gfx_text_width(FONT_MONO_BOLD, 14, fmt_keep);
    float sx = card.x + W / 2 - sw / 2;
    sx += gfx_text(FONT_400, 14, ls, sx, y, 20, hex(TW_ZINC_200), ALIGN_LEFT) + 4;
    sx += gfx_text(FONT_MONO_BOLD, 14, fmt_send, sx, y, 20, hex(INDIGO_400), ALIGN_LEFT) + 4;
    sx += gfx_text(FONT_400, 14, "·", sx, y, 20, hex(TW_ZINC_200), ALIGN_LEFT) + 4;
    sx += gfx_text(FONT_400, 14, lk, sx, y, 20, hex(TW_ZINC_200), ALIGN_LEFT) + 4;
    bool below = troops && total - allowed < floor(total * 0.3);
    gfx_text(FONT_MONO_BOLD, 14, fmt_keep, sx, y, 20, below ? hex(TW_AMBER_400) : hex(EMERALD_400), ALIGN_LEFT);
    y += 20 + 20;
    // Cancel / Send.
    const char *lc = T("common.cancel");
    float bw1 = fmaxf(96, gfx_text_width(FONT_600, 14, lc) + 24), bw2 = fmaxf(96, gfx_text_width(FONT_600, 14, ls) + 24);
    Rect send = {card.x + W - 20 - bw2, y, bw2, 40}, cancel = {send.x - 8 - bw1, y, bw1, 40};
    uint32_t c_id = ui_id("send-cancel", 0), s_id = ui_id("send-ok", 0);
    bool ch = ui_hover(c_id, cancel), sh = ui_hover(s_id, send);
    gfx_box(cancel, 8, ch ? hex(TW_ZINC_700) : hex(TW_ZINC_800), 1, hex(TW_ZINC_700));
    gfx_text(FONT_600, 14, lc, cancel.x + bw1 / 2, y + 10, 20, hex(TW_ZINC_100), ALIGN_CENTER);
    bool disabled = total <= 0 || allowed <= 0;
    gfx_fill_rrect(send, 8, alpha(sh && !disabled ? hex(INDIGO_500) : hex(INDIGO_600), disabled ? 0.5f : 1));
    gfx_text(FONT_600, 14, ls, send.x + bw2 / 2, y + 10, 20, rgba(255, 255, 255, disabled ? 128 : 255), ALIGN_CENTER);
    if (ui_button(c_id, cancel)) app_open_modal(MODAL_PANEL, app.modal_player);
    if (ui_button(s_id, send) && !disabled) {
        char args[128];
        snprintf(args, sizeof args, "{\"sid\":%d,\"action\":\"%s\",\"amount\":%.0f}", app.modal_player,
                 troops ? "donateTroops" : "donateGold", allowed);
        engine_call("playerAction", args, false);
        app_open_modal(MODAL_NONE, -1);
    }
}

// --- EmojiTable ------------------------------------------------------------------------

static void emoji_table(void) {
    if (backdrop("emoji-bg", rgba(0, 0, 0, 38))) {
        app_open_modal(MODAL_NONE, -1);
        return;
    }
    const cJSON *e = M.emojis;
    if (!cJSON_IsObject(e)) return;
    const cJSON *table = jget(e, "table"), *sections = jget(e, "sections");
    const float W = 620, H = 660;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - H / 2, W, H};
    gfx_shadow(card, 12, 16, rgba(0, 0, 0, 120));
    gfx_fill_rrect(card, 12, hexa(TW_ZINC_900, 0.95f));
    gfx_stroke_rrect(card, 12, 1, rgba(255, 255, 255, 26));
    ui_block(card);
    Rect inner = {card.x + 12, card.y + 12, W - 24, H - 24};
    // Content height, for scrolling.
    float content = 0;
    for (int s = 0; s < jlen(sections); s++) {
        const cJSON *sec = jat(sections, s);
        int n = jlen(jget(sec, "emojis"));
        int cols = (int)jnum(sec, "columns", 0);
        if (cols) continue;
        content += 17 + 4 + ceilf((float)n / 10) * 60 - 4 + 12;
    }
    content += 17 + 4 + 3 * 60;
    float max_scroll = fmaxf(0, content - inner.h);
    const Pointer *p = ui_pointer();
    if (in_rect(card, p->x, p->y)) M.emoji_scroll -= p->wheel * 60;
    M.emoji_scroll = fmaxf(0, fminf(max_scroll, M.emoji_scroll));
    gfx_clip(&inner);
    float y = inner.y - M.emoji_scroll;
    float side_x = inner.x;
    for (int s = 0; s < jlen(sections); s++) {
        const cJSON *sec = jat(sections, s);
        const cJSON *ems = jget(sec, "emojis");
        int cols = (int)jnum(sec, "columns", 0);
        float sx = cols ? side_x : inner.x;
        float sw = cols ? (float)cols * 60 - 4 : inner.w;
        char title[64];
        snprintf(title, sizeof title, "%s", jstr(sec, "title", ""));
        for (char *c = title; *c; c++) {
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        }
        gfx_text(FONT_600, 11, title, sx, y, 17, hex(TW_ZINC_400), ALIGN_LEFT);
        float gy = y + 17 + 4;
        int per_row = cols ? cols : 10;
        for (int i = 0; i < jlen(ems); i++) {
            int idx = (int)jat(ems, i)->valuedouble;
            Rect b = {sx + (float)(i % per_row) * 60, gy + (float)(i / per_row) * 60, 56, 56};
            uint32_t id = ui_id("emoji", idx);
            bool hover = ui_hover(id, b) && in_rect(inner, p->x, p->y);
            if (hover) gfx_fill_rrect(b, 8, rgba(255, 255, 255, 26));
            const cJSON *em = jat(table, idx);
            if (cJSON_IsString(em)) gfx_text(FONT_400, 36, em->valuestring, b.x + 28, b.y + 4, 48, hex(TW_WHITE), ALIGN_CENTER);
            if (ui_button(id, b) && in_rect(inner, p->x, p->y)) {
                char args[64];
                snprintf(args, sizeof args, "{\"sid\":%d,\"index\":%d}", app.modal_player, idx);
                engine_call("emoji", args, false);
                app_open_modal(MODAL_NONE, -1);
            }
        }
        float rows = ceilf((float)jlen(ems) / (float)per_row);
        if (cols) {
            side_x += sw + 24;
        } else {
            y = gy + rows * 60 - 4 + 12;
        }
        (void)sw;
    }
    gfx_clip(NULL);
    if (close_button("emoji-close", card.x + W - 16, card.y - 12)) app_open_modal(MODAL_NONE, -1);
}

// --- ChatModal (quick chat) --------------------------------------------------------------

static void chat_modal(void) {
    if (backdrop("chat-bg", rgba(0, 0, 0, 38))) {
        app_open_modal(MODAL_NONE, -1);
        return;
    }
    const cJSON *c = M.chat;
    if (!cJSON_IsObject(c) || M.chat_sid != app.modal_player) return;
    const float W = 704;
    const cJSON *cats = jget(c, "categories");
    // Measure: chips wrap inside the card (px-2.5 py-1 text-sm, gap-1).
    float inner_w = W - 24;
    float h = 12 + 24 + 12;
    for (int pass = 0; pass < 2; pass++) {
        float y = 0;
        for (int i = 0; i < jlen(cats); i++) {
            const cJSON *phr = jget(jat(cats, i), "phrases");
            y += 17 + 4;
            float x = 0, row = 28;
            for (int k = 0; k < jlen(phr); k++) {
                char buf[128];
                gfx_ellipsize(FONT_400, 14, jstr(jat(phr, k), "text", ""), 240 - 20, buf, sizeof buf);
                float w = gfx_text_width(FONT_400, 14, buf) + 20;
                if (x + w > inner_w && x > 0) x = 0, y += row + 4;
                x += w + 4;
            }
            y += row + 12;
        }
        if (pass == 0) h += y;
    }
    h = fminf(h, PAGE_H - 60);
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - h / 2, W, h};
    gfx_fill_rrect(card, 12, hexa(TW_ZINC_900, 0.95f));
    gfx_stroke_rrect(card, 12, 1, rgba(255, 255, 255, 26));
    ui_block(card);
    float x0 = card.x + 12, y = card.y + 12;
    float tw = gfx_text(FONT_600, 16, jstr(c, "title", ""), x0, y, 24, hex(TW_WHITE), ALIGN_LEFT);
    gfx_text(FONT_400, 14, jstr(c, "name", ""), x0 + tw + 8, y + 2, 20, hex(TW_ZINC_400), ALIGN_LEFT);
    y += 24 + 12;
    gfx_clip(&card);
    for (int i = 0; i < jlen(cats); i++) {
        const cJSON *cat = jat(cats, i);
        const cJSON *phr = jget(cat, "phrases");
        char title[64];
        snprintf(title, sizeof title, "%s", jstr(cat, "label", ""));
        for (char *ch = title; *ch; ch++) {
            if (*ch >= 'a' && *ch <= 'z') *ch = (char)(*ch - 32);
        }
        gfx_text(FONT_600, 11, title, x0, y, 17, hex(TW_ZINC_400), ALIGN_LEFT);
        y += 17 + 4;
        float x = 0;
        for (int k = 0; k < jlen(phr); k++) {
            const cJSON *ph = jat(phr, k);
            char buf[128];
            gfx_ellipsize(FONT_400, 14, jstr(ph, "text", ""), 240 - 20, buf, sizeof buf);
            float w = gfx_text_width(FONT_400, 14, buf) + 20;
            if (x + w > inner_w && x > 0) x = 0, y += 32;
            Rect b = {x0 + x, y, w, 28};
            uint32_t id = ui_id("chat-phrase", i * 64 + k);
            bool hover = ui_hover(id, b);
            gfx_fill_rrect(b, 6, rgba(255, 255, 255, hover ? 38 : 13));
            gfx_text(FONT_400, 14, buf, b.x + 10, y + 4, 20, hex(TW_WHITE), ALIGN_LEFT);
            if (ui_button(id, b)) {
                char args[160];
                snprintf(args, sizeof args, "{\"sid\":%d,\"key\":\"%s\"}", app.modal_player, jstr(ph, "key", ""));
                engine_call("sendQuickChat", args, false);
                app_open_modal(MODAL_NONE, -1);
            }
            x += w + 4;
        }
        y += 28 + 12;
    }
    gfx_clip(NULL);
    if (close_button("chat-close", card.x + W - 16, card.y - 12)) app_open_modal(MODAL_NONE, -1);
}

// --- In-game menu (SettingsModal) -----------------------------------------------------

static bool menu_row(int idx, float x, float y, float w, const char *icon, const char *title, const char *desc, const char *right,
                     SDL_Color title_c) {
    Rect r = {x, y, w, 68};
    uint32_t id = ui_id("menu-row", idx);
    bool hover = ui_hover(id, r);
    if (hover) gfx_fill_rrect(r, 4, hex(TW_SLATE_600));
    gfx_icon(icon, (Rect){x + 12, y + 24, 20, 20}, hex(TW_WHITE), ICON_NORMAL);
    gfx_text(FONT_500, 16, title, x + 44, y + 12, 24, title_c, ALIGN_LEFT);
    gfx_text(FONT_400, 14, desc, x + 44, y + 36, 20, hex(TW_SLATE_400), ALIGN_LEFT);
    if (right) gfx_text(FONT_400, 14, right, x + w - 12, y + 24, 20, hex(TW_SLATE_400), ALIGN_RIGHT);
    return ui_button(id, r);
}

static void settings_modal(void) {
    if (backdrop("menu-bg", rgba(0, 0, 0, 153))) {
        app_open_modal(MODAL_NONE, -1);
        return;
    }
    const cJSON *mods = jget(app.frame, "modifiers");
    bool has_mods = jlen(mods) > 0;
    const float W = 448;
    float h = 61 + 16 + 68 + 12 + 68 + (has_mods ? 12 + 16 + 62 : 0) + 12 + 16 + 13 + 68 + 16;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - h / 2, W, h};
    gfx_box(card, 8, hex(SLATE_800), 1, hex(TW_SLATE_600));
    ui_block(card);
    gfx_icon("SettingIconWhite", (Rect){card.x + 17, card.y + 19, 24, 24}, hex(TW_WHITE), ICON_NORMAL);
    gfx_text(FONT_600, 20, T("user_setting.game_menu_title"), card.x + 49, card.y + 17, 28, hex(TW_WHITE), ALIGN_LEFT);
    Rect x_btn = {card.x + W - 33, card.y + 18, 16, 24};
    uint32_t xid = ui_id("menu-x", 0);
    bool xh = ui_hover(xid, (Rect){x_btn.x - 8, x_btn.y - 8, 32, 40});
    gfx_text(FONT_700, 24, "×", x_btn.x + 8, x_btn.y, 24, xh ? hex(TW_WHITE) : hex(TW_SLATE_400), ALIGN_CENTER);
    if (ui_button(xid, (Rect){x_btn.x - 8, x_btn.y - 8, 32, 40})) {
        app_open_modal(MODAL_NONE, -1);
        return;
    }
    gfx_fill_rect((Rect){card.x + 1, card.y + 61, W - 2, 1}, hex(TW_SLATE_600));
    float x = card.x + 17, y = card.y + 62 + 16, w = W - 34;
    if (menu_row(0, x, y, w, "SettingIconWhite", T("user_setting.open_settings_label"), T("user_setting.open_settings_desc"), NULL,
                 hex(TW_WHITE))) {
        app_open_modal(MODAL_NONE, -1);
        app_toggle_controls_help();
        return;
    }
    y += 68 + 12;
    if (menu_row(1, x, y, w, "TreeIconWhite", T("user_setting.toggle_terrain"), T("user_setting.toggle_view_desc"),
                 T(app.alt_view ? "user_setting.on" : "user_setting.off"), hex(TW_WHITE))) {
        app.alt_view = !app.alt_view;
    }
    y += 68;
    if (has_mods) {
        y += 12 + 16;
        gfx_fill_rect((Rect){x, y - 13, w, 1}, hex(TW_SLATE_600));
        char title[64];
        snprintf(title, sizeof title, "%s", T("user_setting.modifiers"));
        for (char *c = title; *c; c++) {
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        }
        gfx_text(FONT_600, 12, title, x + 12, y, 24, hex(TW_SLATE_400), ALIGN_LEFT);
        float mx = x + 12;
        for (int i = 0; i < jlen(mods); i++) {
            const cJSON *m = jat(mods, i);
            char up[64];
            snprintf(up, sizeof up, "%s", cJSON_IsString(m) ? m->valuestring : "");
            for (char *c = up; *c; c++) {
                if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
            }
            float mw = gfx_text_width(FONT_700, 11, up) + 12;
            gfx_fill_rrect((Rect){mx, y + 28, mw, 21}, 4, hex(MALIBU_BLUE));
            gfx_text(FONT_700, 11, up, mx + 6, y + 30, 17, hex(TW_WHITE), ALIGN_LEFT);
            mx += mw + 4;
        }
        y += 62 - 16;
    }
    y += 12 + 13;
    gfx_fill_rect((Rect){x, y - 13, w, 1}, hex(TW_SLATE_600));
    if (menu_row(2, x, y, w, "ExitIconWhite", T("user_setting.exit_game_label"), T("user_setting.exit_game_info"), NULL,
                 hex(0xff6467))) {
        app_open_modal(MODAL_EXIT, -1);
    }
}

// --- ConfirmDialog (exit) ---------------------------------------------------------------

static void exit_confirm(void) {
    if (backdrop("exit-bg", rgba(0, 0, 0, 204))) {
        app_open_modal(MODAL_NONE, -1);
        return;
    }
    const float W = 384, H = 128;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - H / 2, W, H};
    gfx_box(card, 12, hex(0x0a1628), 1, hexa(TW_RED_500, 0.5f));
    ui_block(card);
    gfx_text(FONT_500, 14, T("help_modal.exit_confirmation"), card.x + 25, card.y + 24, 20, hex(TW_RED_300), ALIGN_LEFT);
    float bw = (W - 50 - 12) / 2;
    Rect cancel = {card.x + 25, card.y + 65, bw, 38}, ok = {cancel.x + bw + 12, cancel.y, bw, 38};
    uint32_t cid = ui_id("exit-cancel", 0), oid = ui_id("exit-ok", 0);
    bool ch = ui_hover(cid, cancel), oh = ui_hover(oid, ok);
    gfx_box(cancel, 12, rgba(255, 255, 255, ch ? 26 : 13), 1, rgba(255, 255, 255, 26));
    gfx_fill_rrect(ok, 12, hex(oh ? TW_RED_700 : RED_600));
    char l[64];
    snprintf(l, sizeof l, "%s", T("common.cancel"));
    for (char *c = l; *c; c++) {
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    }
    gfx_text(FONT_700, 12, l, cancel.x + bw / 2, cancel.y + 11, 16, rgba(255, 255, 255, ch ? 204 : 153), ALIGN_CENTER);
    snprintf(l, sizeof l, "%s", T("common.confirm"));
    for (char *c = l; *c; c++) {
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    }
    gfx_text(FONT_700, 12, l, ok.x + bw / 2, ok.y + 11, 16, hex(TW_WHITE), ALIGN_CENTER);
    if (ui_button(cid, cancel)) app_open_modal(MODAL_NONE, -1);
    if (ui_button(oid, ok)) {
        app_open_modal(MODAL_NONE, -1);
        app.quit_to_menu = true;
    }
}

// --- WinModal ---------------------------------------------------------------------------------

static void win_modal(void) {
    const float W = 700;
    float h = 24 + 34 + 16 + 44 + 24;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - h / 2, W, h};
    gfx_shadow(card, 8, 16, rgba(0, 0, 0, 120));
    gfx_fill_rrect(card, 8, hexa(TW_GRAY_800, 0.7f));
    ui_block(card);
    gfx_text(FONT_400, 26, M.win_text, card.x + W / 2, card.y + 24, 34, hex(TW_WHITE), ALIGN_CENTER);
    float y = card.y + 24 + 34 + 16;
    const char *exit_l = T("win_modal.exit");
    bool alive = jbool(jget(app.frame, "me"), "alive");
    const char *keep_l = T(alive ? "win_modal.keep" : "win_modal.spectate");
    float bw = 200;
    Rect ex = {card.x + 24, y, bw, 44}, keep = {card.x + W - 24 - bw, y, bw, 44};
    uint32_t eid = ui_id("win-exit", 0), kid = ui_id("win-keep", 0);
    bool eh = ui_hover(eid, ex), kh = ui_hover(kid, keep);
    gfx_box(ex, 8, rgba(255, 255, 255, eh ? 38 : 20), 1, rgba(255, 255, 255, 51));
    gfx_fill_rrect(keep, 8, hex(kh ? TW_BLUE_500 : TW_BLUE_600));
    gfx_text(FONT_600, 16, exit_l, ex.x + bw / 2, y + 10, 24, hex(TW_WHITE), ALIGN_CENTER);
    gfx_text(FONT_600, 16, keep_l, keep.x + bw / 2, y + 10, 24, hex(TW_WHITE), ALIGN_CENTER);
    if (ui_button(eid, ex)) {
        M.win_open = false;
        app.quit_to_menu = true;
    }
    if (ui_button(kid, keep)) M.win_open = false;
}

// --- Entry points ---------------------------------------------------------------------------------

void menus_reset(void) {
    cJSON_Delete(M.hover);
    cJSON_Delete(M.panel);
    M.hover = M.panel = NULL;
    M.hover_req = M.panel_req = 0;
    M.hover_visible = false;
    M.win_open = false;
    M.win_id_seen = 0;
    M.emoji_scroll = 0;
}

void menus_on_modal_opened(void) {
    if (app.modal == MODAL_PANEL || app.modal == MODAL_SEND) {
        if (M.panel_sid != app.modal_player) {
            cJSON_Delete(M.panel);
            M.panel = NULL;
        }
        request_panel();
    }
    if (app.modal == MODAL_SEND) M.send_amount = -1;
    if (app.modal == MODAL_CHAT) {
        char args[32];
        snprintf(args, sizeof args, "{\"sid\":%d}", app.modal_player);
        M.chat_req = engine_call("quickChat", args, true);
        M.chat_sid = app.modal_player;
    }
    if (app.modal == MODAL_EMOJI) {
        M.emoji_scroll = 0;
        if (!M.emojis && !M.emoji_req) M.emoji_req = engine_call("emojis", "{}", true);
    }
}

const cJSON *menus_emoji_data(void) { return M.emojis; }

void menus_poll(void) {
    char *r;
    if (M.hover_req && (r = engine_result(M.hover_req))) {
        M.hover_req = 0;
        cJSON_Delete(M.hover);
        M.hover = cJSON_Parse(r);
        free(r);
    }
    if (M.panel_req && (r = engine_result(M.panel_req))) {
        M.panel_req = 0;
        cJSON *p = cJSON_Parse(r);
        free(r);
        if (cJSON_IsObject(p)) {
            cJSON_Delete(M.panel);
            M.panel = p;
        } else {
            cJSON_Delete(p);
            if (app.modal == MODAL_PANEL) app_open_modal(MODAL_NONE, -1);
        }
    }
    if (M.chat_req && (r = engine_result(M.chat_req))) {
        M.chat_req = 0;
        cJSON_Delete(M.chat);
        M.chat = cJSON_Parse(r);
        free(r);
    }
    if (M.emoji_req && (r = engine_result(M.emoji_req))) {
        M.emoji_req = 0;
        cJSON_Delete(M.emojis);
        M.emojis = cJSON_Parse(r);
        free(r);
        map_set_emojis(jget(M.emojis, "table"));
    }
    if ((app.modal == MODAL_PANEL || app.modal == MODAL_SEND) && !M.panel_req && app_now_ms() - M.panel_at > 500) request_panel();
    // WinModal opens when the adapter reports a new result.
    const cJSON *w = jget(app.frame, "winner");
    if (cJSON_IsObject(w) && (int)jnum(w, "id", 0) != M.win_id_seen) {
        M.win_id_seen = (int)jnum(w, "id", 0);
        snprintf(M.win_text, sizeof M.win_text, "%s", jstr(w, "text", ""));
        M.win_open = true;
    }
}

void menus_draw_overlay(void) {
    if (app.modal == MODAL_NONE) hover_card();
}

void menus_draw_modals(void) {
    switch (app.modal) {
        case MODAL_PANEL: player_panel(); break;
        case MODAL_SEND: send_modal(); break;
        case MODAL_EMOJI: emoji_table(); break;
        case MODAL_SETTINGS: settings_modal(); break;
        case MODAL_EXIT: exit_confirm(); break;
        case MODAL_CHAT: chat_modal(); break;
        default: break;
    }
    if (M.win_open && app.modal == MODAL_NONE) win_modal();
}

bool menus_win_open(void) { return M.win_open; }

bool menus_back(void) {
    if (app.modal == MODAL_SEND) {
        app_open_modal(MODAL_PANEL, app.modal_player);
        return true;
    }
    if (app.modal != MODAL_NONE) {
        app_open_modal(MODAL_NONE, -1);
        return true;
    }
    if (M.win_open) {
        M.win_open = false;
        return true;
    }
    return false;
}
