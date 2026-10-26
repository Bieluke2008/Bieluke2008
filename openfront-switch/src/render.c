#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "text.h"
#include "units.h"

#define OWNER_MASK 0x0fff
#define FALLOUT_BIT (1 << 13)

static SDL_Renderer *ren;
static SDL_Texture *map_tex;
static uint32_t *pixels;  // ARGB8888, one per tile
static int map_w, map_h;
static int dirty_y0, dirty_y1;  // rows to upload, [y0, y1)

// Terrain colours, as the web client's ColorUtils.encodeTerrainTile with the
// default render settings (ocean #4785b5, background #3c3c3c).
static void terrain_rgb(uint8_t tb, int *r, int *g, int *b) {
    int land = tb & 0x80, shore = tb & 0x40, mag = tb & 0x1f;
    if (land && mag == 31) {
        *r = 0x3c, *g = 0x3c, *b = 0x3c;
    } else if (land && shore) {
        *r = 204, *g = 203, *b = 158;
    } else if (land) {
        if (mag < 10) {
            *r = 190, *g = 220 - 2 * mag, *b = 138;
        } else if (mag < 20) {
            int m = mag - 10;
            *r = 200 + 2 * m, *g = 183 + 2 * m, *b = 138 + 2 * m;
        } else {
            int m = mag / 2;
            *r = 230 + m, *g = 230 + m, *b = 230 + m;
            if (*r > 255) *r = *g = *b = 255;
        }
    } else if (shore) {
        *r = (int)lroundf(0.7f * 0x47 + 76.5f);
        *g = (int)lroundf(0.7f * 0x85 + 76.5f);
        *b = (int)lroundf(0.7f * 0xb5 + 76.5f);
    } else {
        int m = mag < 10 ? mag : 10;
        *r = 0x47 - m, *g = 0x85 - m, *b = 0xb5 - m;
    }
}

static uint32_t tile_color(const Snapshot *s, int x, int y) {
    int i = y * map_w + x;
    int r, g, b;
    terrain_rgb(s->terrain[i], &r, &g, &b);
    uint16_t st = s->state[i];
    int owner = st & OWNER_MASK;
    if (owner != 0 && s->colors[owner * 4 + 3]) {
        int pr = s->colors[owner * 4], pg = s->colors[owner * 4 + 1], pb = s->colors[owner * 4 + 2];
        // Border: a neighbour with another owner (or the map edge).
        bool border = false;
        if (x == 0 || (s->state[i - 1] & OWNER_MASK) != owner) border = true;
        else if (x == map_w - 1 || (s->state[i + 1] & OWNER_MASK) != owner) border = true;
        else if (y == 0 || (s->state[i - map_w] & OWNER_MASK) != owner) border = true;
        else if (y == map_h - 1 || (s->state[i + map_w] & OWNER_MASK) != owner) border = true;
        if (border) {
            r = pr * 7 / 10, g = pg * 7 / 10, b = pb * 7 / 10;
        } else {
            // territoryAlpha 0.588, as the web client.
            r = (r * 412 + pr * 588) / 1000;
            g = (g * 412 + pg * 588) / 1000;
            b = (b * 412 + pb * 588) / 1000;
        }
    }
    if (st & FALLOUT_BIT) {
        r = (r + 120) / 2, g = (g + 200) / 2, b = (b + 60) / 2;
    }
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void render_init(SDL_Renderer *r) { ren = r; }

void render_reset(void) {
    if (map_tex) SDL_DestroyTexture(map_tex);
    map_tex = NULL;
    free(pixels);
    pixels = NULL;
    map_w = map_h = 0;
}

void render_shutdown(void) { render_reset(); }

static void mark_rows(int y0, int y1) {
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

void render_update_map(const Snapshot *s) {
    uint32_t *refs = NULL;
    int full = 0;
    int n = engine_take_dirty(&refs, &full);
    if (s->status != GAME_RUNNING || !s->terrain) {
        free(refs);
        return;
    }
    if (!map_tex || map_w != s->width || map_h != s->height) {
        render_reset();
        map_w = s->width;
        map_h = s->height;
        map_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, map_w, map_h);
        SDL_SetTextureScaleMode(map_tex, SDL_ScaleModeNearest);
        pixels = malloc(sizeof(uint32_t) * (size_t)map_w * (size_t)map_h);
        full = 1;
    }
    dirty_y0 = map_h;
    dirty_y1 = 0;
    if (full) {
        for (int y = 0; y < map_h; y++) {
            for (int x = 0; x < map_w; x++) pixels[y * map_w + x] = tile_color(s, x, y);
        }
        mark_rows(0, map_h);
    } else {
        for (int k = 0; k < n; k++) {
            int ref = (int)refs[k];
            int x = ref % map_w, y = ref / map_w;
            if (y >= map_h) continue;
            // The tile and its neighbours, whose border status may change.
            pixels[ref] = tile_color(s, x, y);
            if (x > 0) pixels[ref - 1] = tile_color(s, x - 1, y);
            if (x < map_w - 1) pixels[ref + 1] = tile_color(s, x + 1, y);
            if (y > 0) pixels[ref - map_w] = tile_color(s, x, y - 1);
            if (y < map_h - 1) pixels[ref + map_w] = tile_color(s, x, y + 1);
            mark_rows(y > 0 ? y - 1 : 0, y + 2 < map_h ? y + 2 : map_h);
        }
    }
    free(refs);
    if (dirty_y1 > dirty_y0) {
        SDL_Rect rect = {0, dirty_y0, map_w, dirty_y1 - dirty_y0};
        SDL_UpdateTexture(map_tex, &rect, pixels + dirty_y0 * map_w, map_w * 4);
    }
}

void cam_screen_to_tile(const Camera *c, int sw, int sh, float sx, float sy, float *tx, float *ty) {
    *tx = c->cx + (sx - (float)sw / 2) / c->zoom;
    *ty = c->cy + (sy - (float)sh / 2) / c->zoom;
}

void cam_tile_to_screen(const Camera *c, int sw, int sh, float tx, float ty, float *sx, float *sy) {
    *sx = (tx - c->cx) * c->zoom + (float)sw / 2;
    *sy = (ty - c->cy) * c->zoom + (float)sh / 2;
}

void cam_clamp(Camera *c, int mw, int mh, int sw, int sh) {
    float fit = fminf((float)sw / (float)mw, (float)sh / (float)mh);
    if (c->zoom < fit * 0.8f) c->zoom = fit * 0.8f;
    if (c->zoom > 24) c->zoom = 24;
    if (c->cx < 0) c->cx = 0;
    if (c->cy < 0) c->cy = 0;
    if (c->cx > (float)mw) c->cx = (float)mw;
    if (c->cy > (float)mh) c->cy = (float)mh;
}

static void fill_rect(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_FRect r = {x, y, w, h};
    SDL_RenderFillRectF(ren, &r);
}

static void outline_rect(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_FRect r = {x, y, w, h};
    SDL_RenderDrawRectF(ren, &r);
}

/** A filled regular polygon (n=3 triangle, 4 diamond, 16 ≈ circle). */
static void fill_poly(float cx, float cy, float rad, int n, float rot, SDL_Color c) {
    SDL_Vertex v[17 * 3];
    int k = 0;
    for (int i = 0; i < n; i++) {
        float a0 = rot + (float)i * 6.2831853f / (float)n;
        float a1 = rot + (float)(i + 1) * 6.2831853f / (float)n;
        SDL_Vertex center = {{cx, cy}, c, {0, 0}};
        SDL_Vertex p0 = {{cx + cosf(a0) * rad, cy + sinf(a0) * rad}, c, {0, 0}};
        SDL_Vertex p1 = {{cx + cosf(a1) * rad, cy + sinf(a1) * rad}, c, {0, 0}};
        v[k++] = center;
        v[k++] = p0;
        v[k++] = p1;
    }
    SDL_RenderGeometry(ren, NULL, v, k, NULL, 0);
}

static SDL_Color owner_color(const Snapshot *s, int owner) {
    SDL_Color c = {200, 200, 200, 255};
    if (s->colors && owner > 0 && owner < MAX_PLAYERS && s->colors[owner * 4 + 3]) {
        c.r = s->colors[owner * 4];
        c.g = s->colors[owner * 4 + 1];
        c.b = s->colors[owner * 4 + 2];
    }
    return c;
}

static void draw_units(const Snapshot *s, const Camera *cam, int sw, int sh) {
    const SDL_Color white = {255, 255, 255, 255};
    const SDL_Color dark = {30, 30, 30, 255};
    float zs = cam->zoom;
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        float x, y;
        cam_tile_to_screen(cam, sw, sh, (float)u->x + 0.5f, (float)u->y + 0.5f, &x, &y);
        if (x < -40 || y < -40 || x > (float)sw + 40 || y > (float)sh + 40) continue;
        SDL_Color oc = owner_color(s, u->owner);
        const UnitStyle *st = unit_style(u->kind);
        if (!st) continue;
        float size = st->tiles * zs;
        if (size < st->min_px) size = st->min_px;
        if (size > st->max_px) size = st->max_px;
        float h = size / 2;
        switch (st->shape) {
            case SHAPE_SQUARE:
                fill_rect(x - h, y - h, size, size, white);
                fill_rect(x - h + 2, y - h + 2, size - 4, size - 4, oc);
                break;
            case SHAPE_CIRCLE:
                fill_poly(x, y, h + 1.5f, 16, 0, white);
                fill_poly(x, y, h, 16, 0, oc);
                break;
            case SHAPE_DIAMOND:
                fill_poly(x, y, h + 1.5f, 4, 0, white);
                fill_poly(x, y, h, 4, 0, oc);
                break;
            case SHAPE_TRIANGLE:
                fill_poly(x, y, h + 1.5f, 3, -1.5708f, white);
                fill_poly(x, y, h, 3, -1.5708f, oc);
                break;
            case SHAPE_SHIP:
                fill_poly(x, y, h + 1, 3, -1.5708f, dark);
                fill_poly(x, y, h - 0.5f, 3, -1.5708f, st->fixed ? st->color : oc);
                break;
            case SHAPE_DOT:
                fill_poly(x, y, h, 8, 0, st->fixed ? st->color : oc);
                break;
        }
        if (st->letter && size >= 14) {
            char l[2] = {st->letter, 0};
            text_draw(l, x, y - size * 0.42f, size * 0.8f, dark, ALIGN_CENTER);
        }
        if (u->level > 1 && size >= 14) {
            char lv[16];
            snprintf(lv, sizeof lv, "%d", u->level);
            text_draw_outlined(lv, x + h, y - h - 6, 14, white, dark, ALIGN_CENTER);
        }
    }
}

static void draw_labels(const Snapshot *s, const Camera *cam, int sw, int sh) {
    const SDL_Color ink = {20, 20, 20, 255};
    const SDL_Color halo = {255, 255, 255, 140};
    for (int i = 0; i < s->nlabels; i++) {
        const Label *l = &s->labels[i];
        // NameViewData.size is the font size in tiles.
        float px = l->size * cam->zoom;
        if (px < 9) continue;
        if (px > 72) px = 72;
        float x, y;
        cam_tile_to_screen(cam, sw, sh, l->x, l->y, &x, &y);
        if (x < -200 || y < -60 || x > (float)sw + 200 || y > (float)sh + 60) continue;
        const char *name = engine_player_name(l->id);
        SDL_Color c = ink;
        if (l->id == s->me) c = (SDL_Color){0, 40, 140, 255};
        text_draw_outlined(name, x, y - px * 0.6f, px, c, halo, ALIGN_CENTER);
        char troops[32];
        format_troops(l->troops, troops, sizeof troops);
        text_draw_outlined(troops, x, y + px * 0.4f, px * 0.75f, c, halo, ALIGN_CENTER);
    }
}

void render_map(const Snapshot *s, const Camera *cam, int sw, int sh) {
    if (!map_tex) return;
    float x0, y0;
    cam_tile_to_screen(cam, sw, sh, 0, 0, &x0, &y0);
    SDL_FRect dst = {x0, y0, (float)map_w * cam->zoom, (float)map_h * cam->zoom};
    SDL_RenderCopyF(ren, map_tex, NULL, &dst);
    outline_rect(dst.x - 1, dst.y - 1, dst.w + 2, dst.h + 2, (SDL_Color){20, 20, 20, 255});
    draw_units(s, cam, sw, sh);
    draw_labels(s, cam, sw, sh);
}

SDL_Texture *render_map_preview(const char *romfs, const char *folder, int *w, int *h) {
    char path[512];
    snprintf(path, sizeof path, "%smaps/%s/manifest.json", romfs, folder);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    // The manifest's "map16x": {"height": H, ..., "width": W} block.
    const char *m = strstr(buf, "\"map16x\"");
    if (!m) return NULL;
    const char *hp = strstr(m, "\"height\"");
    const char *wp = strstr(m, "\"width\"");
    if (!hp || !wp) return NULL;
    int mh = atoi(strchr(hp, ':') + 1), mw = atoi(strchr(wp, ':') + 1);
    if (mw <= 0 || mh <= 0) return NULL;

    snprintf(path, sizeof path, "%smaps/%s/map16x.bin.gz", romfs, folder);
    gzFile gz = gzopen(path, "rb");
    if (!gz) {
        snprintf(path, sizeof path, "%smaps/%s/map16x.bin", romfs, folder);
        gz = gzopen(path, "rb");  // zlib reads plain files too
    }
    if (!gz) return NULL;
    uint8_t *terrain = malloc((size_t)mw * (size_t)mh);
    int got = gzread(gz, terrain, (unsigned)(mw * mh));
    gzclose(gz);
    if (got != mw * mh) {
        free(terrain);
        return NULL;
    }
    uint32_t *px = malloc(sizeof(uint32_t) * (size_t)mw * (size_t)mh);
    for (int i = 0; i < mw * mh; i++) {
        int r, g, b;
        terrain_rgb(terrain[i], &r, &g, &b);
        px[i] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, mw, mh);
    SDL_UpdateTexture(t, NULL, px, mw * 4);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    free(px);
    free(terrain);
    *w = mw;
    *h = mh;
    return t;
}

void format_number(double num, char *out, size_t n) {
    if (num < 0) num = 0;
    if (num >= 10e9) snprintf(out, n, "%.1fB", floor(num / 1e8) / 10);
    else if (num >= 1e9) snprintf(out, n, "%.2fB", floor(num / 1e7) / 100);
    else if (num >= 10e6) snprintf(out, n, "%.1fM", floor(num / 1e5) / 10);
    else if (num >= 1e6) snprintf(out, n, "%.2fM", floor(num / 1e4) / 100);
    else if (num >= 1e5) snprintf(out, n, "%dK", (int)floor(num / 1000));
    else if (num >= 1e4) snprintf(out, n, "%.1fK", floor(num / 100) / 10);
    else if (num >= 1e3) snprintf(out, n, "%.2fK", floor(num / 10) / 100);
    else snprintf(out, n, "%d", (int)floor(num));
}

void format_troops(double troops, char *out, size_t n) { format_number(troops / 10, out, n); }
