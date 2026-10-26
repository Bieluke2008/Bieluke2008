#include "text.h"

#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define BASE_PX 40
#define CACHE_SIZE 4096
#define ATLAS 2048
#define MAX_QUADS 1024

typedef struct {
    Uint32 cp;
    bool used;
    int x, y, w, h;  // in the atlas; w == 0 for blank glyphs
    int advance;
} Glyph;

static SDL_Renderer *ren;
static TTF_Font *font;      // Overpass Bold, like the web client
static TTF_Font *fallback;  // Switch system font: CJK, Cyrillic, symbols
static SDL_Texture *atlas;
static int shelf_x, shelf_y, shelf_h;  // shelf packer cursor
static Glyph cache[CACHE_SIZE];
static int line_h;

// One string's quads, drawn with a single SDL_RenderGeometry call.
static SDL_Vertex verts[MAX_QUADS * 4];
static int indices[MAX_QUADS * 6];
static int nquads;

bool text_init(SDL_Renderer *r, const char *romfs) {
    ren = r;
    if (TTF_Init() != 0) return false;
    char path[512];
    snprintf(path, sizeof path, "%sfonts/overpass-bold.ttf", romfs);
    font = TTF_OpenFont(path, BASE_PX);
    if (!font) {
        fprintf(stderr, "font: %s\n", TTF_GetError());
        return false;
    }
#ifdef __SWITCH__
    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
        SDL_RWops *rw = SDL_RWFromConstMem(fd.address, (int)fd.size);
        fallback = TTF_OpenFontRW(rw, 1, BASE_PX);
    }
#endif
    line_h = TTF_FontHeight(font);
    atlas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
    if (!atlas) return false;
    SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas, SDL_ScaleModeLinear);
    // Start transparent.
    Uint32 *clear = calloc(ATLAS * 64, sizeof(Uint32));
    for (int y = 0; y < ATLAS; y += 64) {
        SDL_Rect band = {0, y, ATLAS, 64};
        SDL_UpdateTexture(atlas, &band, clear, ATLAS * 4);
    }
    free(clear);
    for (int i = 0; i < MAX_QUADS; i++) {
        int v = i * 4, k = i * 6;
        indices[k] = v, indices[k + 1] = v + 1, indices[k + 2] = v + 2;
        indices[k + 3] = v, indices[k + 4] = v + 2, indices[k + 5] = v + 3;
    }
    return true;
}

void text_shutdown(void) {
    if (atlas) SDL_DestroyTexture(atlas);
    atlas = NULL;
    memset(cache, 0, sizeof cache);
    if (fallback) TTF_CloseFont(fallback);
    if (font) TTF_CloseFont(font);
    TTF_Quit();
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

static int to_utf8(Uint32 cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63));
    out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/** Packs a rendered glyph into the atlas; false when the atlas is full. */
static bool pack(SDL_Surface *s, Glyph *g) {
    if (shelf_x + s->w + 1 > ATLAS) {
        shelf_x = 0;
        shelf_y += shelf_h + 1;
        shelf_h = 0;
    }
    if (shelf_y + s->h > ATLAS) return false;
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!conv) return false;
    SDL_Rect r = {shelf_x, shelf_y, conv->w, conv->h};
    SDL_UpdateTexture(atlas, &r, conv->pixels, conv->pitch);
    SDL_FreeSurface(conv);
    g->x = shelf_x;
    g->y = shelf_y;
    g->w = s->w;
    g->h = s->h;
    shelf_x += s->w + 1;
    if (s->h > shelf_h) shelf_h = s->h;
    return true;
}

static Glyph *glyph(Uint32 cp) {
    unsigned h = (cp * 2654435761u) % CACHE_SIZE;
    for (int i = 0; i < CACHE_SIZE; i++) {
        Glyph *g = &cache[(h + (unsigned)i) % CACHE_SIZE];
        if (g->used && g->cp == cp) return g;
        if (g->used) continue;
        TTF_Font *f = font;
        if (!TTF_GlyphIsProvided32(font, cp) && fallback && TTF_GlyphIsProvided32(fallback, cp)) {
            f = fallback;
        }
        int minx, maxx, miny, maxy, adv = BASE_PX / 3;
        TTF_GlyphMetrics32(f, cp, &minx, &maxx, &miny, &maxy, &adv);
        g->used = true;
        g->cp = cp;
        g->advance = adv;
        g->w = g->h = 0;
        if (cp != ' ') {
            // A one-character string keeps the glyph on the font's baseline.
            char utf8[5] = {0};
            to_utf8(cp, utf8);
            SDL_Color white = {255, 255, 255, 255};
            SDL_Surface *s = TTF_RenderUTF8_Blended(f, utf8, white);
            if (s) {
                if (!pack(s, g)) g->w = g->h = 0;
                SDL_FreeSurface(s);
            }
        }
        return g;
    }
    return NULL;
}

float text_width(const char *s, float px) {
    float scale = px / (float)line_h;
    float w = 0;
    while (*s) {
        Glyph *g = glyph(next_cp(&s));
        if (g) w += (float)g->advance * scale;
    }
    return w;
}

static void flush(void) {
    if (nquads == 0) return;
    SDL_RenderGeometry(ren, atlas, verts, nquads * 4, indices, nquads * 6);
    nquads = 0;
}

/** Adds one pass of `s` to the quad batch. */
static void layout(const char *s, float x, float y, float px, SDL_Color c) {
    float scale = px / (float)line_h;
    while (*s) {
        Glyph *g = glyph(next_cp(&s));
        if (!g) continue;
        if (g->w > 0) {
            if (nquads == MAX_QUADS) flush();
            float x0 = x, y0 = y, x1 = x + (float)g->w * scale, y1 = y + (float)g->h * scale;
            float u0 = (float)g->x / ATLAS, v0 = (float)g->y / ATLAS;
            float u1 = (float)(g->x + g->w) / ATLAS, v1 = (float)(g->y + g->h) / ATLAS;
            SDL_Vertex *v = &verts[nquads * 4];
            v[0] = (SDL_Vertex){{x0, y0}, c, {u0, v0}};
            v[1] = (SDL_Vertex){{x1, y0}, c, {u1, v0}};
            v[2] = (SDL_Vertex){{x1, y1}, c, {u1, v1}};
            v[3] = (SDL_Vertex){{x0, y1}, c, {u0, v1}};
            nquads++;
        }
        x += (float)g->advance * scale;
    }
}

static float aligned_x(const char *s, float x, float px, Align a, float *w) {
    *w = text_width(s, px);
    if (a == ALIGN_CENTER) return x - *w / 2;
    if (a == ALIGN_RIGHT) return x - *w;
    return x;
}

float text_draw(const char *s, float x, float y, float px, SDL_Color c, Align a) {
    float w;
    x = aligned_x(s, x, px, a, &w);
    layout(s, x, y, px, c);
    flush();
    return w;
}

float text_draw_outlined(const char *s, float x, float y, float px, SDL_Color c,
                         SDL_Color outline, Align a) {
    float w;
    x = aligned_x(s, x, px, a, &w);
    float o = px > 24 ? 2.0f : 1.0f;
    layout(s, x - o, y, px, outline);
    layout(s, x + o, y, px, outline);
    layout(s, x, y - o, px, outline);
    layout(s, x, y + o, px, outline);
    layout(s, x, y, px, c);
    flush();
    return w;
}
