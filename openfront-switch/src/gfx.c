#include "gfx.h"

#include <SDL_image.h>
#include <SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define ATLAS 2048
#define MAX_PAGES 4
#define GLYPH_SLOTS 16384
#define MAX_FACES 96
#define MAX_ICONS 512
#define MAX_VERTS 4096
#define PI_F 3.14159265f

static SDL_Renderer *ren;
static float scale = 1;
static char root[256];

// --- Geometry batching ------------------------------------------------------------

static SDL_Vertex verts[MAX_VERTS];
static int nverts;

static void flush_geom(void) {
    if (nverts) SDL_RenderGeometry(ren, NULL, verts, nverts, NULL, 0);
    nverts = 0;
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color c) {
    if (nverts + 3 > MAX_VERTS) flush_geom();
    SDL_Vertex *v = &verts[nverts];
    v[0] = (SDL_Vertex){{x0 * scale, y0 * scale}, c, {0, 0}};
    v[1] = (SDL_Vertex){{x1 * scale, y1 * scale}, c, {0, 0}};
    v[2] = (SDL_Vertex){{x2 * scale, y2 * scale}, c, {0, 0}};
    nverts += 3;
}

SDL_Renderer *gfx_renderer(void) { return ren; }
float gfx_scale(void) { return scale; }

void gfx_begin_frame(void) {
    int w, h;
    SDL_GetRendererOutputSize(ren, &w, &h);
    scale = (float)h / PAGE_H;
    if ((float)w / PAGE_W < scale) scale = (float)w / PAGE_W;
}

void gfx_fill_rect(Rect r, SDL_Color c) {
    tri(r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, c);
    tri(r.x, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h, c);
    flush_geom();
}

/** Points of a corner arc, from angle a0 to a0+90°, appended to a polygon. */
static int corner(float *px, float *py, int n, float cx, float cy, float rad, float a0, int steps) {
    for (int i = 0; i <= steps; i++) {
        float a = a0 + (PI_F / 2) * (float)i / (float)steps;
        px[n] = cx + cosf(a) * rad;
        py[n] = cy + sinf(a) * rad;
        n++;
    }
    return n;
}

static int corner_steps(float rad) { return rad * scale < 2 ? 1 : rad * scale < 8 ? 4 : 8; }

static int rrect_points(Rect r, float tl, float tr, float br, float bl, float *px, float *py, int steps) {
    float m = fminf(r.w, r.h) / 2;
    tl = fminf(fmaxf(tl, 0), m), tr = fminf(fmaxf(tr, 0), m), br = fminf(fmaxf(br, 0), m), bl = fminf(fmaxf(bl, 0), m);
    int n = 0;
    n = corner(px, py, n, r.x + tl, r.y + tl, tl, PI_F, steps);
    n = corner(px, py, n, r.x + r.w - tr, r.y + tr, tr, -PI_F / 2, steps);
    n = corner(px, py, n, r.x + r.w - br, r.y + r.h - br, br, 0, steps);
    n = corner(px, py, n, r.x + bl, r.y + r.h - bl, bl, PI_F / 2, steps);
    return n;
}

void gfx_fill_rrect4(Rect r, float tl, float tr, float br, float bl, SDL_Color c) {
    if (r.w <= 0 || r.h <= 0 || c.a == 0) return;
    float px[40], py[40];
    int steps = corner_steps(fmaxf(fmaxf(tl, tr), fmaxf(br, bl)));
    int n = rrect_points(r, tl, tr, br, bl, px, py, steps);
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        tri(cx, cy, px[i], py[i], px[j], py[j], c);
    }
    flush_geom();
}

void gfx_fill_rrect(Rect r, float radius, SDL_Color c) { gfx_fill_rrect4(r, radius, radius, radius, radius, c); }

void gfx_stroke_rrect(Rect r, float radius, float width, SDL_Color c) {
    if (r.w <= 0 || r.h <= 0 || c.a == 0 || width <= 0) return;
    float ox[40], oy[40], ix[40], iy[40];
    int steps = corner_steps(radius);
    int n = rrect_points(r, radius, radius, radius, radius, ox, oy, steps);
    float ir = fmaxf(0, radius - width);
    rrect_points((Rect){r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width}, ir, ir, ir, ir, ix, iy, steps);
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        tri(ox[i], oy[i], ox[j], oy[j], ix[j], iy[j], c);
        tri(ox[i], oy[i], ix[j], iy[j], ix[i], iy[i], c);
    }
    flush_geom();
}

void gfx_box(Rect r, float radius, SDL_Color bg, float border, SDL_Color bc) {
    if (bg.a) gfx_fill_rrect(r, radius, bg);
    if (border > 0 && bc.a) gfx_stroke_rrect(r, radius, border, bc);
}

void gfx_shadow(Rect r, float radius, float spread, SDL_Color c) {
    int steps = 6;
    for (int i = steps; i >= 1; i--) {
        float g = spread * (float)i / (float)steps;
        SDL_Color cc = c;
        cc.a = (Uint8)((float)c.a / (float)steps);
        gfx_fill_rrect((Rect){r.x - g, r.y - g + spread / 3, r.w + 2 * g, r.h + 2 * g}, radius + g, cc);
    }
}

static int circle_steps(float r) {
    int s = (int)(r * scale * 0.9f);
    if (s < 12) s = 12;
    if (s > 96) s = 96;
    return s;
}

void gfx_fill_circle(float cx, float cy, float r, SDL_Color c) {
    int n = circle_steps(r);
    for (int i = 0; i < n; i++) {
        float a0 = 2 * PI_F * (float)i / (float)n, a1 = 2 * PI_F * (float)(i + 1) / (float)n;
        tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, c);
    }
    flush_geom();
}

void gfx_stroke_circle(float cx, float cy, float r, float width, SDL_Color c) {
    gfx_fill_arc(cx, cy, r - width, r, 0, 2 * PI_F, c);
}

void gfx_fill_arc(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c) {
    if (a1 < a0) {
        float t = a0;
        a0 = a1;
        a1 = t;
    }
    int n = (int)(circle_steps(r1) * (a1 - a0) / (2 * PI_F)) + 2;
    for (int i = 0; i < n; i++) {
        // d3 convention: 0 = up, clockwise.
        float t0 = a0 + (a1 - a0) * (float)i / (float)n - PI_F / 2;
        float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)n - PI_F / 2;
        float ax = cx + cosf(t0) * r0, ay = cy + sinf(t0) * r0;
        float bx = cx + cosf(t0) * r1, by = cy + sinf(t0) * r1;
        float dx = cx + cosf(t1) * r0, dy = cy + sinf(t1) * r0;
        float ex = cx + cosf(t1) * r1, ey = cy + sinf(t1) * r1;
        tri(ax, ay, bx, by, ex, ey, c);
        tri(ax, ay, ex, ey, dx, dy, c);
    }
    flush_geom();
}

void gfx_fill_ngon(float cx, float cy, float r, int n, float rot, SDL_Color c) {
    for (int i = 0; i < n; i++) {
        float a0 = rot + 2 * PI_F * (float)i / (float)n, a1 = rot + 2 * PI_F * (float)(i + 1) / (float)n;
        tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, c);
    }
    flush_geom();
}

void gfx_line(float x0, float y0, float x1, float y1, float width, SDL_Color c) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
    flush_geom();
}

void gfx_fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color c) {
    tri(x0, y0, x1, y1, x2, y2, c);
    flush_geom();
}

void gfx_clip(const Rect *r) {
    if (!r) {
        SDL_RenderSetClipRect(ren, NULL);
        return;
    }
    SDL_Rect c = {(int)floorf(r->x * scale), (int)floorf(r->y * scale), (int)ceilf(r->w * scale), (int)ceilf(r->h * scale)};
    SDL_RenderSetClipRect(ren, &c);
}

// --- Images ----------------------------------------------------------------------

typedef struct {
    char name[64];
    IconMode mode;
    SDL_Texture *tex;
    int w, h;
    bool missing;
} Icon;

static Icon icons[MAX_ICONS];
static int nicons;

SDL_Texture *gfx_load_texture(const char *path, int *w, int *h) {
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    if (w) *w = s->w;
    if (h) *h = s->h;
    SDL_FreeSurface(s);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    return t;
}

static SDL_Texture *load_icon(const char *dir, const char *name, IconMode mode, int *w, int *h) {
    char path[512];
    snprintf(path, sizeof path, "%s%s/%s.png", root, dir, name);
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c) return NULL;
    if (mode == ICON_SILHOUETTE) {
        SDL_LockSurface(c);
        for (int y = 0; y < c->h; y++) {
            Uint32 *row = (Uint32 *)((Uint8 *)c->pixels + y * c->pitch);
            for (int x = 0; x < c->w; x++) row[x] = (row[x] & 0xff000000u) | 0x00ffffffu;
        }
        SDL_UnlockSurface(c);
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, c);
    *w = c->w;
    *h = c->h;
    SDL_FreeSurface(c);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}

static Icon *find_icon(const char *dir, const char *name, IconMode mode) {
    for (int i = 0; i < nicons; i++) {
        if (icons[i].mode == mode && strcmp(icons[i].name, name) == 0) return &icons[i];
    }
    if (nicons == MAX_ICONS) return NULL;
    Icon *ic = &icons[nicons++];
    snprintf(ic->name, sizeof ic->name, "%s", name);
    ic->mode = mode;
    ic->tex = load_icon(dir, name, mode, &ic->w, &ic->h);
    ic->missing = ic->tex == NULL;
    if (ic->missing) fprintf(stderr, "missing image %s/%s\n", dir, name);
    return ic;
}

SDL_Texture *gfx_icon_tex(const char *name, IconMode mode, int *w, int *h) {
    Icon *ic = find_icon("icons", name, mode);
    if (!ic || ic->missing) return NULL;
    if (w) *w = ic->w;
    if (h) *h = ic->h;
    return ic->tex;
}

void gfx_image(SDL_Texture *t, const SDL_Rect *src, Rect r, SDL_Color tint) {
    if (!t) return;
    SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(t, tint.a);
    SDL_FRect d = {r.x * scale, r.y * scale, r.w * scale, r.h * scale};
    SDL_RenderCopyF(ren, t, src, &d);
}

static void fit(Rect r, int w, int h, Rect *out) {
    float s = fminf(r.w / (float)w, r.h / (float)h);
    out->w = (float)w * s;
    out->h = (float)h * s;
    out->x = r.x + (r.w - out->w) / 2;
    out->y = r.y + (r.h - out->h) / 2;
}

void gfx_icon(const char *name, Rect r, SDL_Color tint, IconMode mode) {
    Icon *ic = find_icon("icons", name, mode);
    if (!ic || ic->missing) return;
    Rect d;
    fit(r, ic->w, ic->h, &d);
    gfx_image(ic->tex, NULL, d, tint);
}

bool gfx_flag(const char *code, Rect r) {
    if (!code || !code[0]) return false;
    Icon *ic = find_icon("flags", code, ICON_NORMAL);
    if (!ic || ic->missing) return false;
    Rect d;
    fit(r, ic->w, ic->h, &d);
    gfx_image(ic->tex, NULL, d, rgba(255, 255, 255, 255));
    return true;
}

// --- Text ------------------------------------------------------------------------

static const char *FONT_FILES[FONT_COUNT] = {
    "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "Inter-Bold.ttf", "Inter-ExtraBold.ttf",
    "overpass-bold.ttf", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
};

typedef struct {
    Font font;
    int px;
    TTF_Font *ttf;
    int ascent, descent, height;
} Face;

typedef struct {
    bool used;
    Font font;
    int px;
    Uint32 cp;
    int page, x, y, w, h, advance;
} Glyph;

static Face faces[MAX_FACES];
static int nfaces;
static Glyph glyphs[GLYPH_SLOTS];
static SDL_Texture *pages[MAX_PAGES];
static int npages;
static int shelf_x, shelf_y, shelf_h;
#ifdef __SWITCH__
static void *fallback_data;
static size_t fallback_size;
#endif

static int real_px(Font f, float px) {
    float r = px * scale;
    int p = (int)(r + 0.5f);
    if (f == FONT_MAP && p > 24) p = (p + 1) & ~1;  // map labels take any size
    if (p < 4) p = 4;
    if (p > 120) p = 120;
    return p;
}

static Face *face(Font f, int px) {
    for (int i = 0; i < nfaces; i++) {
        if (faces[i].font == f && faces[i].px == px) return &faces[i];
    }
    if (nfaces == MAX_FACES) {
        // Evict the oldest map-label size (UI sizes are few and stay).
        for (int i = 0; i < nfaces; i++) {
            if (faces[i].font == FONT_MAP) {
                TTF_CloseFont(faces[i].ttf);
                faces[i] = faces[--nfaces];
                break;
            }
        }
        if (nfaces == MAX_FACES) return &faces[0];
    }
    char path[512];
    snprintf(path, sizeof path, "%sfonts/%s", root, FONT_FILES[f]);
    TTF_Font *t = TTF_OpenFont(path, px);
    if (!t) {
        fprintf(stderr, "font %s: %s\n", path, TTF_GetError());
        return nfaces ? &faces[0] : NULL;
    }
    Face *fc = &faces[nfaces++];
    fc->font = f;
    fc->px = px;
    fc->ttf = t;
    fc->ascent = TTF_FontAscent(t);
    fc->descent = -TTF_FontDescent(t);
    fc->height = TTF_FontHeight(t);
    return fc;
}

static TTF_Font *fallback_font(int px) {
#ifdef __SWITCH__
    // The console's own font covers CJK and other scripts Inter lacks.
    static TTF_Font *cache[128];
    if (!fallback_data) return NULL;
    if (px >= 128) px = 127;
    if (!cache[px]) cache[px] = TTF_OpenFontRW(SDL_RWFromConstMem(fallback_data, (int)fallback_size), 1, px);
    return cache[px];
#else
    (void)px;
    return NULL;
#endif
}

static bool new_page(void) {
    if (npages == MAX_PAGES) return false;
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
    if (!t) return false;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    Uint32 *clear = calloc(ATLAS * 64, sizeof(Uint32));
    for (int y = 0; y < ATLAS; y += 64) {
        SDL_Rect band = {0, y, ATLAS, 64};
        SDL_UpdateTexture(t, &band, clear, ATLAS * 4);
    }
    free(clear);
    pages[npages++] = t;
    shelf_x = shelf_y = shelf_h = 0;
    return true;
}

static void reset_glyphs(void) {
    // Atlas full: start over (glyphs are re-rendered on demand).
    memset(glyphs, 0, sizeof glyphs);
    shelf_x = shelf_y = shelf_h = 0;
    npages = npages ? 1 : 0;
}

static bool pack(SDL_Surface *s, Glyph *g) {
    if (npages == 0 && !new_page()) return false;
    if (shelf_x + s->w + 1 > ATLAS) {
        shelf_x = 0;
        shelf_y += shelf_h + 1;
        shelf_h = 0;
    }
    if (shelf_y + s->h > ATLAS) {
        if (!new_page()) {
            reset_glyphs();
            return false;
        }
    }
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!c) return false;
    SDL_Rect r = {shelf_x, shelf_y, c->w, c->h};
    SDL_UpdateTexture(pages[npages - 1], &r, c->pixels, c->pitch);
    SDL_FreeSurface(c);
    g->page = npages - 1;
    g->x = shelf_x;
    g->y = shelf_y;
    g->w = s->w;
    g->h = s->h;
    shelf_x += s->w + 1;
    if (s->h > shelf_h) shelf_h = s->h;
    return true;
}

static int to_utf8(Uint32 cp, char *o) {
    if (cp < 0x80) return o[0] = (char)cp, 1;
    if (cp < 0x800) return o[0] = (char)(0xc0 | (cp >> 6)), o[1] = (char)(0x80 | (cp & 63)), 2;
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)), o[1] = (char)(0x80 | ((cp >> 6) & 63)), o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)), o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)), o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static Uint32 next_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    Uint32 c = *s++;
    if (c >= 0xf0 && s[0] && s[1] && s[2]) {
        c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
        s += 3;
    } else if (c >= 0xe0 && s[0] && s[1]) {
        c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u);
        s += 2;
    } else if (c >= 0xc0 && s[0]) {
        c = ((c & 31) << 6) | (s[0] & 63u);
        s += 1;
    }
    *p = (const char *)s;
    return c;
}

static Glyph *glyph(Font f, int px, Uint32 cp) {
    unsigned h = ((cp * 2654435761u) ^ (unsigned)(px * 97 + (int)f * 7919)) % GLYPH_SLOTS;
    for (int i = 0; i < 64; i++) {
        Glyph *g = &glyphs[(h + (unsigned)i) % GLYPH_SLOTS];
        if (g->used && g->cp == cp && g->px == px && g->font == f) return g;
        if (g->used) continue;
        Face *fc = face(f, px);
        if (!fc) return NULL;
        TTF_Font *t = fc->ttf;
        if (!TTF_GlyphIsProvided32(t, cp)) {
            TTF_Font *fb = fallback_font(px);
            if (fb && TTF_GlyphIsProvided32(fb, cp)) t = fb;
        }
        int minx, maxx, miny, maxy, adv = px / 3;
        TTF_GlyphMetrics32(t, cp, &minx, &maxx, &miny, &maxy, &adv);
        g->used = true;
        g->font = f;
        g->px = px;
        g->cp = cp;
        g->advance = adv;
        g->w = g->h = 0;
        if (cp != ' ') {
            char u[5] = {0};
            to_utf8(cp, u);
            SDL_Surface *s = TTF_RenderUTF8_Blended(t, u, (SDL_Color){255, 255, 255, 255});
            if (s) {
                // A fallback font's line box differs: align baselines.
                if (t != fc->ttf) {
                    SDL_Surface *aligned = SDL_CreateRGBSurfaceWithFormat(0, s->w, fc->height, 32, SDL_PIXELFORMAT_ARGB8888);
                    SDL_Rect dst = {0, fc->ascent - TTF_FontAscent(t), s->w, s->h};
                    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
                    SDL_BlitSurface(s, NULL, aligned, &dst);
                    SDL_FreeSurface(s);
                    s = aligned;
                }
                if (!pack(s, g)) g->w = g->h = 0;
                SDL_FreeSurface(s);
            }
        }
        return g;
    }
    return NULL;
}

// --- Emoji in text ------------------------------------------------------------------
// The HUD's strings carry emoji (💰, ⚔️...). They are drawn from OpenFront's
// emoji atlas, or the port's small supplement (assets/ui-emoji.png).

#define MAX_EMOJI 128

typedef struct {
    char utf8[32];
    int len;
    SDL_Texture *tex;
    SDL_Rect src;
} Emoji;

static Emoji emojis[MAX_EMOJI];
static int nemojis;

/** Reads an atlas meta file ({cellSize, cols, emojis: {"😀": 0}}) and its image. */
static void load_emoji_atlas(const char *meta_rel, const char *png_rel) {
    char path[512];
    snprintf(path, sizeof path, "%s%s", root, meta_rel);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    snprintf(path, sizeof path, "%s%s", root, png_rel);
    SDL_Texture *tex = gfx_load_texture(path, NULL, NULL);
    if (!tex) return;
    const char *cs = strstr(buf, "\"cellSize\"");
    const char *cl = strstr(buf, "\"cols\"");
    const char *em = strstr(buf, "\"emojis\"");
    if (!cs || !cl || !em) return;
    int cell = atoi(strchr(cs, ':') + 1), cols = atoi(strchr(cl, ':') + 1);
    const char *p = strchr(em, '{');
    while (p && nemojis < MAX_EMOJI) {
        const char *q0 = strchr(p, '"');
        if (!q0) break;
        const char *q1 = strchr(q0 + 1, '"');
        if (!q1) break;
        const char *colon = strchr(q1, ':');
        if (!colon) break;
        int idx = atoi(colon + 1);
        Emoji *e = &emojis[nemojis++];
        int len = (int)(q1 - q0 - 1);
        if (len >= (int)sizeof e->utf8) len = (int)sizeof e->utf8 - 1;
        memcpy(e->utf8, q0 + 1, (size_t)len);
        e->utf8[len] = 0;
        e->len = len;
        e->tex = tex;
        e->src = (SDL_Rect){(idx % cols) * cell, (idx / cols) * cell, cell, cell};
        p = strchr(colon, ',');
    }
}

/** The emoji `s` starts with (the longest match; U+FE0F optional), and its byte length. */
static const Emoji *emoji_at(const char *s, int *len) {
    const unsigned char c = (unsigned char)*s;
    if (c < 0xe2) return NULL;  // every emoji here starts at U+2190 or above
    const Emoji *best = NULL;
    int best_len = 0;
    for (int i = 0; i < nemojis; i++) {
        const Emoji *e = &emojis[i];
        // Compare skipping variation selectors on both sides.
        const char *a = e->utf8, *b = s;
        while (*a) {
            if (!strncmp(a, "\xef\xb8\x8f", 3)) {
                a += 3;
                continue;
            }
            if (!strncmp(b, "\xef\xb8\x8f", 3)) {
                b += 3;
                continue;
            }
            if (*a != *b) break;
            a++, b++;
        }
        if (*a) continue;
        while (!strncmp(b, "\xef\xb8\x8f", 3)) b += 3;
        int l = (int)(b - s);
        if (l > best_len) best = e, best_len = l;
    }
    *len = best_len;
    return best;
}

#define EMOJI_ADVANCE 1.25f
#define EMOJI_SIZE 1.15f

float gfx_text_width(Font f, float px, const char *s) {
    int p = real_px(f, px);
    float k = px * scale / (float)p;
    float w = 0;
    while (s && *s) {
        int el;
        if (emoji_at(s, &el)) {
            w += px * scale * EMOJI_ADVANCE;
            s += el;
            continue;
        }
        Glyph *g = glyph(f, p, next_cp(&s));
        if (g) w += (float)g->advance * k;
    }
    return w / scale;
}

#define MAX_TEXT_QUADS 512
static SDL_Vertex tverts[MAX_PAGES][MAX_TEXT_QUADS * 4];
static int tquads[MAX_PAGES];
static int tindices[MAX_TEXT_QUADS * 6];

static void text_flush(void) {
    for (int p = 0; p < npages; p++) {
        if (!tquads[p]) continue;
        SDL_RenderGeometry(ren, pages[p], tverts[p], tquads[p] * 4, tindices, tquads[p] * 6);
        tquads[p] = 0;
    }
}

/** Queues glyph quads; x,y in output pixels (top of the font's line box). */
static void text_layout(Font f, int p, float k, const char *s, float x, float y, SDL_Color c, bool fill) {
    while (*s) {
        int el;
        const Emoji *em = emoji_at(s, &el);
        if (em) {
            float px = (float)p * k, size = px * EMOJI_SIZE;
            if (fill) {
                Face *fc = face(f, p);
                float box = fc ? (float)fc->height * k : px * 1.2f;
                SDL_SetTextureColorMod(em->tex, 255, 255, 255);
                SDL_SetTextureAlphaMod(em->tex, c.a);
                SDL_FRect d = {x + (px * EMOJI_ADVANCE - size) / 2, y + (box - size) / 2, size, size};
                SDL_RenderCopyF(ren, em->tex, &em->src, &d);
            }
            x += px * EMOJI_ADVANCE;
            s += el;
            continue;
        }
        Glyph *g = glyph(f, p, next_cp(&s));
        if (!g) continue;
        if (g->w > 0) {
            if (tquads[g->page] == MAX_TEXT_QUADS) text_flush();
            float x1 = x + (float)g->w * k, y1 = y + (float)g->h * k;
            float u0 = (float)g->x / ATLAS, v0 = (float)g->y / ATLAS;
            float u1 = (float)(g->x + g->w) / ATLAS, v1 = (float)(g->y + g->h) / ATLAS;
            SDL_Vertex *v = &tverts[g->page][tquads[g->page] * 4];
            v[0] = (SDL_Vertex){{x, y}, c, {u0, v0}};
            v[1] = (SDL_Vertex){{x1, y}, c, {u1, v0}};
            v[2] = (SDL_Vertex){{x1, y1}, c, {u1, v1}};
            v[3] = (SDL_Vertex){{x, y1}, c, {u0, v1}};
            tquads[g->page]++;
        }
        x += (float)g->advance * k;
    }
}

/** Output-pixel origin of the text (left x, top of the font box). */
static void text_origin(Font f, float px, const char *s, float x, float y, float line_h, Align a, int *p,
                        float *k, float *ox, float *oy, float *w) {
    *p = real_px(f, px);
    *k = px * scale / (float)*p;
    Face *fc = face(f, *p);
    *w = gfx_text_width(f, px, s);
    if (a == ALIGN_CENTER) x -= *w / 2;
    if (a == ALIGN_RIGHT) x -= *w;
    if (line_h <= 0) line_h = px * 1.2f;
    float content = fc ? (float)(fc->ascent + fc->descent) * *k : px * scale;
    float box = fc ? (float)fc->height * *k : content;
    // Browsers center the ascent+descent area in the line box.
    *ox = x * scale;
    *oy = y * scale + (line_h * scale - content) / 2 - (box - content) / 2;
}

float gfx_text(Font f, float px, const char *s, float x, float y, float line_h, SDL_Color c, Align a) {
    if (!s || !*s) return 0;
    int p;
    float k, ox, oy, w;
    text_origin(f, px, s, x, y, line_h, a, &p, &k, &ox, &oy, &w);
    text_layout(f, p, k, s, roundf(ox), roundf(oy), c, true);
    text_flush();
    return w;
}

float gfx_text_shadow(Font f, float px, const char *s, float x, float y, float line_h, SDL_Color c, Align a) {
    if (!s || !*s) return 0;
    int p;
    float k, ox, oy, w;
    text_origin(f, px, s, x, y, line_h, a, &p, &k, &ox, &oy, &w);
    text_layout(f, p, k, s, roundf(ox), roundf(oy + scale), rgba(0, 0, 0, (int)(0.6f * c.a)), false);
    text_layout(f, p, k, s, roundf(ox), roundf(oy), c, true);
    text_flush();
    return w;
}

float gfx_text_outline(Font f, float px, const char *s, float x, float y, SDL_Color c, SDL_Color o, float ow,
                       Align a) {
    if (!s || !*s) return 0;
    int p;
    float k, ox, oy, w;
    text_origin(f, px, s, x, y, 0, a, &p, &k, &ox, &oy, &w);
    float d = ow * scale;
    if (d > 0 && o.a) {
        static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        for (int i = 0; i < 8; i++) text_layout(f, p, k, s, ox + dirs[i][0] * d, oy + dirs[i][1] * d, o, false);
    }
    text_layout(f, p, k, s, ox, oy, c, true);
    text_flush();
    return w;
}

float gfx_text_baseline(Font f, float px, const char *s, float x, float baseline, SDL_Color c, SDL_Color o,
                        float ow, Align a) {
    if (!s || !*s) return 0;
    int p = real_px(f, px);
    float k = px * scale / (float)p;
    Face *fc = face(f, p);
    float w = gfx_text_width(f, px, s);
    if (a == ALIGN_CENTER) x -= w / 2;
    if (a == ALIGN_RIGHT) x -= w;
    float ox = x * scale;
    float oy = baseline * scale - (fc ? (float)fc->ascent * k : px * scale * 0.8f);
    float d = ow * scale;
    if (d > 0 && o.a) {
        static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        for (int i = 0; i < 8; i++) text_layout(f, p, k, s, ox + dirs[i][0] * d, oy + dirs[i][1] * d, o, false);
    }
    text_layout(f, p, k, s, ox, oy, c, true);
    text_flush();
    return w;
}

const char *gfx_ellipsize(Font f, float px, const char *s, float max_w, char *out, size_t n) {
    snprintf(out, n, "%s", s ? s : "");
    if (gfx_text_width(f, px, out) <= max_w) return out;
    size_t len = strlen(out);
    while (len > 0) {
        // Drop one UTF-8 character.
        do {
            len--;
        } while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80);
        if (len + 4 >= n) continue;
        memcpy(out + len, "...", 4);
        if (gfx_text_width(f, px, out) <= max_w) return out;
    }
    return out;
}

int gfx_wrap(Font f, float px, const char *s, float max_w, char *out, size_t n) {
    size_t o = 0;
    int lines = 1;
    char line[512] = {0};
    size_t ll = 0;
    const char *p = s;
    while (*p) {
        const char *ws = p;
        while (*p && *p != ' ') p++;
        char word[256];
        size_t wl = (size_t)(p - ws) < sizeof word - 1 ? (size_t)(p - ws) : sizeof word - 1;
        memcpy(word, ws, wl);
        word[wl] = 0;
        char trial[768];
        snprintf(trial, sizeof trial, "%s%s%s", line, ll ? " " : "", word);
        if (ll && gfx_text_width(f, px, trial) > max_w) {
            size_t l = strlen(line);
            if (o + l + 1 < n) {
                memcpy(out + o, line, l + 1);
                o += l + 1;
            }
            lines++;
            snprintf(line, sizeof line, "%s", word);
        } else {
            snprintf(line, sizeof line, "%.511s", trial);
        }
        ll = strlen(line);
        while (*p == ' ') p++;
    }
    size_t l = strlen(line);
    if (o + l + 1 < n) memcpy(out + o, line, l + 1);
    return lines;
}

bool gfx_init(SDL_Renderer *r, const char *romfs) {
    ren = r;
    snprintf(root, sizeof root, "%s", romfs);
    if (TTF_Init() != 0) return false;
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) fprintf(stderr, "IMG_Init: %s\n", IMG_GetError());
    for (int i = 0; i < MAX_TEXT_QUADS; i++) {
        int v = i * 4, k = i * 6;
        tindices[k] = v, tindices[k + 1] = v + 1, tindices[k + 2] = v + 2;
        tindices[k + 3] = v, tindices[k + 4] = v + 2, tindices[k + 5] = v + 3;
    }
#ifdef __SWITCH__
    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
        fallback_data = fd.address;
        fallback_size = fd.size;
    }
#endif
    load_emoji_atlas("atlases/emoji-atlas-meta.json", "atlases/emoji-atlas.png");
    load_emoji_atlas("atlases/ui-emoji.json", "atlases/ui-emoji.png");
    gfx_begin_frame();
    return face(FONT_400, 14) != NULL;
}

void gfx_shutdown(void) {
    for (int i = 0; i < nicons; i++) {
        if (icons[i].tex) SDL_DestroyTexture(icons[i].tex);
    }
    nicons = 0;
    for (int i = 0; i < npages; i++) SDL_DestroyTexture(pages[i]);
    npages = 0;
    for (int i = 0; i < nfaces; i++) TTF_CloseFont(faces[i].ttf);
    nfaces = 0;
    IMG_Quit();
    TTF_Quit();
}

void gfx_upper(char *s) {
    for (unsigned char *c = (unsigned char *)s; *c; c++) {
        if (*c >= 'a' && *c <= 'z') {
            *c = (unsigned char)(*c - 32);
        } else if (*c == 0xC3 && c[1] >= 0xA0 && c[1] <= 0xBE && c[1] != 0xB7) {
            c[1] = (unsigned char)(c[1] - 0x20);  // à..þ -> À..Þ (not ÷)
            c++;
        } else if ((*c == 0xC4 || *c == 0xC5) && c[1] >= 0x80 && c[1] <= 0xBF) {
            // Latin Extended-A pairs: upper even, lower odd (ı, ĸ, ŉ and ſ aside).
            unsigned cp = ((unsigned)(*c & 0x1F) << 6) | (c[1] & 0x3F);
            bool odd_lower = (cp >= 0x100 && cp <= 0x137 && cp != 0x131) || (cp >= 0x14A && cp <= 0x177);
            bool even_lower = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
            if ((odd_lower && (cp & 1)) || (even_lower && !(cp & 1))) {
                cp -= 1;
                c[0] = (unsigned char)(0xC0 | (cp >> 6));
                c[1] = (unsigned char)(0x80 | (cp & 0x3F));
            }
            c++;
        }
    }
}
