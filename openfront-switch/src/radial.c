// The web client's radial menu (RadialMenu.ts with MainRadialMenu's
// items): d3 pie rings with the same radii, padding, colours and opacities,
// a centre button that attacks / donates / spawns or goes back, nested
// build, attack and upgrade-amount rings, and the hover tooltip.
#include "radial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "json.h"
#include "sound.h"
#include "ui.h"

#define PI_F 3.14159265f
#define PAD_ANGLE 0.03f
#define CENTER_R 30.0f
#define ICON 32.0f
#define CENTER_ICON 48.0f
#define REFRESH_MS 400

static struct {
    bool open;
    int tx, ty;              // the clicked tile
    float ax, ay;            // anchor (the click point)
    cJSON *model;
    int pending;             // request id of a refresh / first load
    int select_pending;      // radialSelect / radialCenter in flight
    double last_refresh;
    int path[4];             // selected index per level
    int level;               // current level
    double opened_at;
} R;

static float inner_r(int level) { return level == 0 ? 40 : 75; }
static float outer_r(int level) { return inner_r(level) + (level == 0 ? 55 : 65); }

bool radial_is_open(void) { return R.open; }

void radial_close(void) {
    R.open = false;
    cJSON_Delete(R.model);
    R.model = NULL;
    R.pending = 0;
    R.level = 0;
}

void radial_open(int tx, int ty, float px, float py) {
    radial_close();
    R.open = true;
    R.tx = tx, R.ty = ty;
    R.ax = px, R.ay = py;
    R.level = 0;
    R.opened_at = app_now_ms();
    char args[64];
    snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", tx, ty);
    R.pending = engine_call("radial", args, true);
    R.last_refresh = app_now_ms();
}

/** The item list shown at `level` given the selection path. */
static const cJSON *items_at(int level) {
    const cJSON *items = jget(R.model, "items");
    for (int l = 0; l < level; l++) {
        const cJSON *it = jat(items, R.path[l]);
        items = jget(it, "sub");
    }
    return items;
}

static void handle_action_result(const char *json) {
    cJSON *r = cJSON_Parse(json);
    const char *open = jstr(r, "open", NULL);
    int player = (int)jnum(r, "player", -1);
    if (open && player >= 0) {
        if (!strcmp(open, "panel")) app_open_modal(MODAL_PANEL, player);
        else if (!strcmp(open, "sendGold")) {
            app.send_troops = false;
            app_open_modal(MODAL_SEND, player);
        } else if (!strcmp(open, "sendTroops")) {
            app.send_troops = true;
            app_open_modal(MODAL_SEND, player);
        } else if (!strcmp(open, "emoji")) app_open_modal(MODAL_EMOJI, player);
    }
    cJSON_Delete(r);
}

void radial_poll(void) {
    if (R.select_pending) {
        char *res = engine_result(R.select_pending);
        if (res) {
            R.select_pending = 0;
            handle_action_result(res);
            free(res);
        }
    }
    if (!R.open) return;
    if (R.pending) {
        char *res = engine_result(R.pending);
        if (!res) return;
        R.pending = 0;
        cJSON *m = cJSON_Parse(res);
        free(res);
        if (!m || cJSON_IsNull(m)) {
            cJSON_Delete(m);
            if (!R.model) radial_close();
            return;
        }
        int spectate = (int)jnum(m, "spectate", -1);
        if (spectate >= 0) {
            cJSON_Delete(m);
            radial_close();
            app_open_modal(MODAL_PANEL, spectate);
            return;
        }
        cJSON_Delete(R.model);
        R.model = m;
        // Keep the path only while it still leads somewhere.
        for (int l = 0; l < R.level; l++) {
            const cJSON *items = items_at(l);
            if (R.path[l] >= jlen(items) || jlen(jget(jat(items, R.path[l]), "sub")) == 0) {
                R.level = l;
                break;
            }
        }
    } else if (app_now_ms() - R.last_refresh > REFRESH_MS) {
        // RadialMenu.refresh(): the menu tracks the game while it is open.
        char args[64];
        snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", R.tx, R.ty);
        R.pending = engine_call("radial", args, true);
        R.last_refresh = app_now_ms();
    }
}

// --- Drawing -------------------------------------------------------------------------

/** clampAndSetMenuPositionForLevel(): keep the current ring on screen. */
static void center_for(int level, float *cx, float *cy) {
    float margin = fmaxf(outer_r(level), CENTER_R) + 10;
    *cx = 2 * margin > PAGE_W ? PAGE_W / 2.0f : fminf(fmaxf(R.ax, margin), PAGE_W - margin);
    *cy = 2 * margin > PAGE_H ? PAGE_H / 2.0f : fminf(fmaxf(R.ay, margin), PAGE_H - margin);
}

static SDL_Color brighten(SDL_Color c, float k) {
    return rgba((int)fminf(255, c.r * k), (int)fminf(255, c.g * k), (int)fminf(255, c.b * k), c.a);
}

/**
 * A d3 arc with padAngle: the gap between slices has a constant width of
 * padRadius * padAngle, padRadius = sqrt(inner² + outer²).
 */
static void pie_slice(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c) {
    float pad = sqrtf(r0 * r0 + r1 * r1) * PAD_ANGLE / 2;
    float i0 = r0 > 0 ? asinf(fminf(1, pad / r0)) : 0, i1 = asinf(fminf(1, pad / r1));
    float b0 = a0 + i0, b1 = a1 - i0, c0 = a0 + i1, c1 = a1 - i1;
    if (b1 < b0) b0 = b1 = (a0 + a1) / 2;
    int n = (int)ceilf((a1 - a0) / 0.05f);
    if (n < 2) n = 2;
    for (int k = 0; k < n; k++) {
        float t0 = (float)k / n, t1 = (float)(k + 1) / n;
        float ia = b0 + (b1 - b0) * t0, ib = b0 + (b1 - b0) * t1;
        float oa = c0 + (c1 - c0) * t0, ob = c0 + (c1 - c0) * t1;
        // d3 angles: 0 = up, clockwise.
        float x0 = cx + sinf(ia) * r0, y0 = cy - cosf(ia) * r0;
        float x1 = cx + sinf(ib) * r0, y1 = cy - cosf(ib) * r0;
        float x2 = cx + sinf(ob) * r1, y2 = cy - cosf(ob) * r1;
        float x3 = cx + sinf(oa) * r1, y3 = cy - cosf(oa) * r1;
        gfx_fill_triangle(x0, y0, x1, y1, x2, y2, c);
        gfx_fill_triangle(x0, y0, x2, y2, x3, y3, c);
    }
}

/** Bounding box of a slice (for the timer gradient, which is objectBoundingBox). */
static void slice_bounds(float cy, float r0, float r1, float a0, float a1, float *top, float *bottom) {
    *top = 1e9f, *bottom = -1e9f;
    for (int k = 0; k <= 32; k++) {
        float a = a0 + (a1 - a0) * (float)k / 32;
        float y0 = cy - cosf(a) * r0, y1 = cy - cosf(a) * r1;
        *top = fminf(*top, fminf(y0, y1));
        *bottom = fmaxf(*bottom, fmaxf(y0, y1));
    }
}

typedef struct {
    float cx, cy, scale, opacity;
    int level;
} Ring;

static int hit_item(const Ring *g, int n, float px, float py) {
    float dx = px - g->cx, dy = py - g->cy;
    float d = sqrtf(dx * dx + dy * dy) / g->scale;
    if (d < inner_r(g->level) || d > outer_r(g->level)) return -1;
    float a = atan2f(dx, -dy);  // 0 = up, clockwise
    float off = -PI_F / (float)n;
    float t = a - off;
    while (t < 0) t += 2 * PI_F;
    while (t >= 2 * PI_F) t -= 2 * PI_F;
    return (int)(t / (2 * PI_F / (float)n)) % n;
}

static void tooltip(const cJSON *lines, float px, float py) {
    int n = jlen(lines);
    if (n == 0) return;
    // padding 6px 10px, max-width 250, title bold 14 + mb 4, cost mt 4.
    float w = 0, h = 12;
    for (int i = 0; i < n; i++) {
        const cJSON *l = jat(lines, i);
        const char *cls = jstr(l, "cls", "");
        bool title = !strcmp(cls, "title");
        float tw = gfx_text_width(title ? FONT_700 : FONT_400, title ? 14 : 12, jstr(l, "text", ""));
        if (tw > w) w = tw;
        h += title ? 17 + 4 : 15;
        if (!strcmp(cls, "cost")) h += 4;
    }
    w = fminf(w, 230) + 20;
    Rect r = {px + 10, py + 10, w, h};
    if (r.x + r.w > PAGE_W) r.x = PAGE_W - r.w;
    if (r.y + r.h > PAGE_H) r.y = PAGE_H - r.h;
    gfx_fill_rrect(r, 6, rgba(12, 35, 64, 224));
    float y = r.y + 6;
    for (int i = 0; i < n; i++) {
        const cJSON *l = jat(lines, i);
        const char *cls = jstr(l, "cls", "");
        char buf[256];
        if (!strcmp(cls, "title")) {
            gfx_text(FONT_700, 14, gfx_ellipsize(FONT_700, 14, jstr(l, "text", ""), 230, buf, sizeof buf), r.x + 10, y, 17,
                     hex(TW_WHITE), ALIGN_LEFT);
            y += 21;
        } else {
            SDL_Color c = !strcmp(cls, "cost") ? hex(0xf59e0b) : !strcmp(cls, "count") ? hex(0x94a3b8) : hex(TW_WHITE);
            if (!strcmp(cls, "cost")) y += 4;
            gfx_text(FONT_400, 12, gfx_ellipsize(FONT_400, 12, jstr(l, "text", ""), 230, buf, sizeof buf), r.x + 10, y, 15,
                     c, ALIGN_LEFT);
            y += 15;
        }
    }
}

static void select_item(int level, int index) {
    const cJSON *items = items_at(level);
    const cJSON *it = jat(items, index);
    if (!it || jbool(it, "disabled")) return;
    sound_play("click");
    if (jlen(jget(it, "sub")) > 0) {
        R.path[level] = index;
        R.level = level + 1;
        return;
    }
    if (!jbool(it, "action")) return;
    R.path[level] = index;
    char args[128];
    int len = snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"path\":[", R.tx, R.ty);
    for (int l = 0; l <= level; l++) len += snprintf(args + len, sizeof args - (size_t)len, "%s%d", l ? "," : "", R.path[l]);
    snprintf(args + len, sizeof args - (size_t)len, "]}");
    R.select_pending = engine_call("radialSelect", args, true);
    radial_close();
}

static void draw_ring(const Ring *g, const cJSON *items, bool interactive, int selected, const cJSON **hover_tip) {
    int n = jlen(items);
    if (n == 0) return;
    float r0 = inner_r(g->level) * g->scale, r1 = outer_r(g->level) * g->scale;
    float off = -PI_F / (float)n, step = 2 * PI_F / (float)n;
    const Pointer *p = ui_pointer();
    int hovered = interactive ? hit_item(g, n, p->x, p->y) : -1;
    bool over = ui_hover(ui_id("radial-ring", g->level), (Rect){g->cx - r1, g->cy - r1, 2 * r1, 2 * r1});
    if (!over) hovered = -1;
    for (int i = 0; i < n; i++) {
        const cJSON *it = jat(items, i);
        bool disabled = jbool(it, "disabled");
        SDL_Color c = disabled ? rgba(128, 128, 128, 255) : jcolor(jstr(it, "color", "#1e3a5f"));
        float op = disabled ? 0.4f * 0.5f : 0.82f;
        if (i == selected) op = 1;  // the item whose submenu is open
        if (i == hovered && !disabled) c = brighten(c, 1.5f);
        c.a = (Uint8)(255 * op * g->opacity);
        float a0 = off + step * (float)i, a1 = a0 + step;
        float timer = (float)jnum(it, "timer", -1);
        if (timer >= 0 && timer <= 1) {
            // Top (1 - fraction) faded towards white, the rest normal.
            float top, bottom;
            slice_bounds(g->cy, r0, r1, a0, a1, &top, &bottom);
            float split = top + (bottom - top) * (1 - timer);
            SDL_Color faded = rgba(c.r + (int)((255 - c.r) * 0.4f), c.g + (int)((255 - c.g) * 0.4f),
                                   c.b + (int)((255 - c.b) * 0.4f), c.a);
            Rect upper = {0, 0, PAGE_W, split}, lower = {0, split, PAGE_W, PAGE_H};
            gfx_clip(&upper);
            pie_slice(g->cx, g->cy, r0, r1, a0, a1, faded);
            gfx_clip(&lower);
            pie_slice(g->cx, g->cy, r0, r1, a0, a1, c);
            gfx_clip(NULL);
        } else {
            pie_slice(g->cx, g->cy, r0, r1, a0, a1, c);
        }
        if (i == selected) {
            // url(#glow): a soft halo around the open item.
            pie_slice(g->cx, g->cy, r0 - 2, r1 + 2, a0 - 0.02f, a1 + 0.02f, alpha(c, 0.25f * g->opacity));
        }
        float mid = (a0 + a1) / 2, rc = (r0 + r1) / 2;
        float ix = g->cx + sinf(mid) * rc, iy = g->cy - cosf(mid) * rc;
        float ia = (disabled ? 0.5f : 1) * g->opacity;
        const char *text = jstr(it, "text", "");
        float sz = ICON * g->scale;
        if (*text) {
            float fs = (float)jnum(it, "font", 12) * g->scale;
            gfx_text(FONT_400, fs, text, ix, iy - fs * 0.6f, fs * 1.2f, rgba(255, 255, 255, (int)(255 * ia)), ALIGN_CENTER);
        } else {
            // getSvgAspectRatio(): wide icons keep the height and grow wider.
            int tw = 1, th = 1;
            const char *icon = jstr(it, "icon", "");
            float w = sz, h = sz;
            if (gfx_icon_tex(icon, ICON_NORMAL, &tw, &th) && th > 0) {
                float aspect = (float)tw / (float)th;
                if (aspect > 1) w = roundf(sz * aspect);
                else if (aspect < 1) h = roundf(sz * aspect);
            }
            gfx_icon(icon, (Rect){ix - w / 2, iy - h / 2, w, h}, rgba(255, 255, 255, (int)(255 * ia)), ICON_NORMAL);
            double cd = jnum(it, "cooldown", 0);
            if (cd > 0) {
                char t[16];
                snprintf(t, sizeof t, "%ds", (int)ceil(cd));
                gfx_text_baseline(FONT_700, 14 * g->scale, t, ix - sz / 4, iy + sz / 2 + 7, rgba(255, 255, 255, (int)(255 * ia)),
                                  rgba(0, 0, 0, 0), 0, ALIGN_LEFT);
            }
        }
    }
    if (hovered >= 0) {
        const cJSON *it = jat(items, hovered);
        if (jlen(jget(it, "tooltip")) > 0) *hover_tip = jget(it, "tooltip");
        if (ui_button(ui_id("radial-ring", g->level), (Rect){g->cx - r1, g->cy - r1, 2 * r1, 2 * r1})) {
            select_item(g->level, hovered);
        }
    }
}

void radial_draw(void) {
    if (!R.open || !R.model) {
        if (R.open) {
            // Still loading: swallow clicks where the menu will be.
            float cx, cy;
            center_for(0, &cx, &cy);
            ui_block((Rect){cx - 95, cy - 95, 190, 190});
        }
        return;
    }
    const cJSON *tip = NULL;
    float cx, cy;
    center_for(R.level, &cx, &cy);
    // Previous level shrinks behind the current one (scale 0.65, or 0.5 two deep).
    if (R.level > 0) {
        Ring prev = {cx, cy, R.level == 1 ? 0.65f : 0.5f, 0.8f, R.level - 1};
        draw_ring(&prev, items_at(R.level - 1), false, R.path[R.level - 1], &tip);
    }
    Ring cur = {cx, cy, 1, 1, R.level};
    draw_ring(&cur, items_at(R.level), true, -1, &tip);

    // Centre button.
    const cJSON *center = jget(R.model, "center");
    bool back = R.level > 0;
    bool enabled = back || !jbool(center, "disabled");
    float rr = back ? CENTER_R * 0.8f : CENTER_R;
    SDL_Color col = back ? hex(0x0f2744) : jcolor(jstr(center, "color", "#0f2744"));
    if (!enabled) col = hex(0x999999);
    uint32_t id = ui_id("radial-center", 0);
    Rect cr = {cx - rr, cy - rr, 2 * rr, 2 * rr};
    bool hover = ui_hover(id, cr) && hypotf(ui_pointer()->x - cx, ui_pointer()->y - cy) <= rr;
    if (hover && enabled) col = brighten(col, 1.2f);
    gfx_fill_circle(cx, cy, rr, col);
    float isz = back ? CENTER_ICON * 0.8f : CENTER_ICON * (float)jnum(center, "size", 1);
    gfx_icon(back ? "BackIconWhite" : jstr(center, "icon", "SwordIconWhite"), (Rect){cx - isz / 2, cy - isz / 2, isz, isz},
             rgba(255, 255, 255, enabled ? 255 : 128), ICON_NORMAL);
    if (ui_button(id, cr) && hover) {
        if (back) {
            R.level--;
        } else if (enabled) {
            char args[64];
            snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d}", R.tx, R.ty);
            R.select_pending = engine_call("radialCenter", args, true);
            radial_close();
            return;
        }
    }
    if (tip) tooltip(tip, ui_pointer()->x, ui_pointer()->y);
}

void radial_pick(int index) {
    if (R.open && R.model) select_item(R.level, index);
}

bool radial_back(void) {
    if (!R.open) return false;
    if (R.level > 0) R.level--;
    else radial_close();
    return true;
}
