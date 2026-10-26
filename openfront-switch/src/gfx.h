// Drawing toolkit for the HUD and the map: shapes with rounded corners, arcs,
// OpenFront's icons and flags, and text in Inter (the web client's UI font)
// and Overpass (its map labels).
//
// Coordinates are CSS pixels of a 1280x720 page, like the web client's
// layout; gfx scales them to the real output (1920x1080 when docked) and
// rasterizes text at the real size so it stays sharp.
#pragma once

#include <SDL.h>
#include <stdbool.h>

#define PAGE_W 1280
#define PAGE_H 720

typedef struct {
    float x, y, w, h;
} Rect;

typedef enum {
    FONT_400,  // Inter Regular
    FONT_500,  // Medium
    FONT_600,  // SemiBold
    FONT_700,  // Bold
    FONT_800,  // ExtraBold
    FONT_MAP,  // Overpass Bold (names on the map)
    FONT_MONO,       // DejaVu Sans Mono (the web UI's font-mono)
    FONT_MONO_BOLD,
    FONT_COUNT,
} Font;

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } Align;

bool gfx_init(SDL_Renderer *r, const char *romfs);
void gfx_shutdown(void);
/** Call once per frame with the renderer's output size. */
void gfx_begin_frame(void);
float gfx_scale(void);
SDL_Renderer *gfx_renderer(void);

static inline SDL_Color rgba(int r, int g, int b, int a) {
    SDL_Color c = {(Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a};
    return c;
}
static inline SDL_Color hex(unsigned v) { return rgba((int)(v >> 16) & 255, (int)(v >> 8) & 255, (int)v & 255, 255); }
static inline SDL_Color alpha(SDL_Color c, float a) {
    c.a = (Uint8)(a * 255 + 0.5f);
    return c;
}
static inline Rect rect(float x, float y, float w, float h) {
    Rect r = {x, y, w, h};
    return r;
}
static inline bool in_rect(Rect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// --- Shapes ----------------------------------------------------------------
void gfx_fill_rect(Rect r, SDL_Color c);
/** Rounded rectangle; radii are top-left, top-right, bottom-right, bottom-left. */
void gfx_fill_rrect4(Rect r, float tl, float tr, float br, float bl, SDL_Color c);
void gfx_fill_rrect(Rect r, float radius, SDL_Color c);
void gfx_stroke_rrect(Rect r, float radius, float width, SDL_Color c);
/** CSS box: background, then a border drawn inside the box. */
void gfx_box(Rect r, float radius, SDL_Color bg, float border, SDL_Color border_color);
/** A soft drop shadow under a box (tailwind shadow-lg-ish). */
void gfx_shadow(Rect r, float radius, float spread, SDL_Color c);
void gfx_fill_circle(float cx, float cy, float r, SDL_Color c);
void gfx_stroke_circle(float cx, float cy, float r, float width, SDL_Color c);
/** Annulus sector from angle a0 to a1 (radians, 0 = up, clockwise like d3). */
void gfx_fill_arc(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c);
/** Regular polygon: n sides, circumradius r, rotation rot (radians). */
void gfx_fill_ngon(float cx, float cy, float r, int n, float rot, SDL_Color c);
void gfx_line(float x0, float y0, float x1, float y1, float width, SDL_Color c);
void gfx_fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color c);

void gfx_clip(const Rect *r);  // NULL clears

// --- Images ----------------------------------------------------------------
typedef enum {
    ICON_NORMAL,      // the image's own colors (times tint)
    ICON_SILHOUETTE,  // every opaque pixel in the tint color (CSS brightness(0) invert())
} IconMode;

/** One of OpenFront's images by file name without extension, e.g. "SwordIconWhite". */
SDL_Texture *gfx_icon_tex(const char *name, IconMode mode, int *w, int *h);
/** Draws an icon fitted into r (keeping its aspect ratio, centered). */
void gfx_icon(const char *name, Rect r, SDL_Color tint, IconMode mode);
/** A nation's flag by code (resources/flags/<code>.svg); false if unknown. */
bool gfx_flag(const char *code, Rect r);
/** Whole-texture or sub-rectangle draw with tint. */
void gfx_image(SDL_Texture *t, const SDL_Rect *src, Rect dst, SDL_Color tint);
SDL_Texture *gfx_load_texture(const char *path, int *w, int *h);

// --- Text --------------------------------------------------------------------
/**
 * Draws one line of UTF-8 text. `x` is the left/center/right anchor and `y`
 * the top of a CSS line box of `line_h` pixels (0 = font size * 1.2); the
 * glyphs are centered in it the way browsers do. Returns the width.
 */
float gfx_text(Font f, float px, const char *s, float x, float y, float line_h, SDL_Color c, Align a);
/** The same with a 1px drop shadow (tailwind drop-shadow-[0_1px_1px_rgba(0,0,0,0.8)]). */
float gfx_text_shadow(Font f, float px, const char *s, float x, float y, float line_h, SDL_Color c, Align a);
/** Text with a halo all around (names on the map). */
float gfx_text_outline(Font f, float px, const char *s, float x, float y, SDL_Color c, SDL_Color outline, float ow, Align a);
/** Outlined text placed by its baseline (map labels, like the web's MSDF names). */
float gfx_text_baseline(Font f, float px, const char *s, float x, float baseline, SDL_Color c, SDL_Color outline,
                        float ow, Align a);
float gfx_text_width(Font f, float px, const char *s);
/** Truncates `s` with an ellipsis to fit `max_w`; returns `out`. */
const char *gfx_ellipsize(Font f, float px, const char *s, float max_w, char *out, size_t n);
/** Wraps into lines no wider than max_w; returns the line count (lines are NUL-separated in out). */
int gfx_wrap(Font f, float px, const char *s, float max_w, char *out, size_t n);
