// The map, drawn after the web client's WebGL passes (src/client/render/gl/)
// with their default render-settings.json values. Territory, borders and
// fallout are composited per tile on the CPU into one texture; structures,
// units, names and overlays are sprites over it.
#include "map.h"

#include <SDL_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "gfx.h"
#include "json.h"
#include "units.h"

#define OWNER_MASK 0x0fff
#define FALLOUT_BIT (1 << 13)
#define PI_F 3.14159265f
#define STRIDE 8  // colors.ts: territory rgb, valid, border rgb, is-me

// render-settings.json (mapOverlay, structure, unit, name)
#define TERRITORY_ALPHA 0.588f
#define TERRITORY_SATURATION 0.85f
#define TERRITORY_DEFENSE_DARKEN 0.85f
#define DEFENSE_CHECKER_DARKEN 0.7f
#define HIGHLIGHT_BRIGHTEN 0.25f
#define HIGHLIGHT_FILL_BRIGHTEN 0.15f
#define TINT_RATIO 0.35f
#define DEFENSE_POST_RANGE 30
#define ICON_SIZE 60.0f
#define DOTS_ZOOM 1.2f
#define DOT_SCALE 0.3f
#define ICON_SCALE_ZOOMED_OUT 3.0f
#define ICON_GROW_ZOOM 7.0f
#define FILL_DARKEN 0.65f
#define BORDER_DARKEN 0.1f
#define UNIT_SIZE 13.0f
#define HBOMB_GLOW_SCALE 2.2f
#define NAME_SCALE_FACTOR 0.4f
#define NAME_SCALE_CAP 3.0f
#define TROOP_SIZE 0.6f
#define NAME_OUTLINE 1.4f
#define ROW_OFFSET 1.4f

static char root[256];

// --- Territory texture -------------------------------------------------------------

static SDL_Texture *map_tex;
static uint32_t *pixels;
static int map_w, map_h;
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static uint16_t *coverage;  // owner of a defense post covering the tile, 0 none
static uint32_t coverage_hash;
static int relations_version = -1;
static int highlight;       // hovered owner
static bool alt_view;       // "Toggle Terrain": territories in relation colours
static int local_sid;
static bool view_dirty;

typedef struct {
    bool valid;
    float fill[3];      // territory colour after saturation (0..1)
    float fill_def[3];  // with the defense darken
    float fill_hl[3];   // hovered: contrast boost
    float fill_hl_def[3];
    float border[3];
    uint8_t raw[3];     // territory colour as the table has it
    uint8_t raw_border[3];
    bool me;
} Pal;

static Pal pal[MAX_PLAYERS];
static uint8_t pal_src[MAX_PLAYERS * STRIDE];

// ColorUtils.encodeTerrainTile() with the default terrain colours.
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

static void saturate(const float in[3], float out[3]) {
    float l = in[0] * 0.299f + in[1] * 0.587f + in[2] * 0.114f;
    for (int k = 0; k < 3; k++) out[k] = l + (in[k] - l) * TERRITORY_SATURATION;
}

static void build_palette(const Snapshot *s) {
    if (memcmp(pal_src, s->colors, sizeof pal_src) == 0) return;
    memcpy(pal_src, s->colors, sizeof pal_src);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const uint8_t *c = &s->colors[i * STRIDE];
        Pal *p = &pal[i];
        p->valid = c[3] != 0;
        if (!p->valid) continue;
        float f[3] = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f}, t[3];
        saturate(f, p->fill);
        for (int k = 0; k < 3; k++) t[k] = f[k] * TERRITORY_DEFENSE_DARKEN;
        saturate(t, p->fill_def);
        for (int k = 0; k < 3; k++) t[k] = fminf(1, fmaxf(0, (f[k] - 0.5f) * (1 + HIGHLIGHT_FILL_BRIGHTEN) + 0.5f));
        saturate(t, p->fill_hl);
        for (int k = 0; k < 3; k++) t[k] *= TERRITORY_DEFENSE_DARKEN;
        saturate(t, p->fill_hl_def);
        for (int k = 0; k < 3; k++) {
            p->border[k] = c[4 + k] / 255.0f;
            p->raw[k] = c[k];
            p->raw_border[k] = c[4 + k];
        }
        p->me = c[7] != 0;
    }
}

static inline int owner_at(const Snapshot *s, int x, int y) {
    if (x < 0 || y < 0 || x >= map_w || y >= map_h) return 0;
    return s->state[y * map_w + x] & OWNER_MASK;
}

static inline uint32_t pack(float r, float g, float b) {
    int ri = (int)(r * 255 + 0.5f), gi = (int)(g * 255 + 0.5f), bi = (int)(b * 255 + 0.5f);
    ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
    gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
    bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
    return 0xff000000u | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

/**
 * Affiliation.ts: the alt view's colour of an owner: self green, ally
 * yellow, embargo red, else neutral grey (units: neutral counts as enemy).
 */
static void affiliation(const Snapshot *s, int owner, bool units, float out[3]) {
    int rel = 0;
    if (s->relations && local_sid > 0 && owner > 0 && owner < MAX_REL && local_sid < MAX_REL) {
        rel = s->relations[local_sid * MAX_REL + owner];
    }
    if (owner == local_sid && owner > 0) out[0] = 0, out[1] = 1, out[2] = 0;
    else if (rel == 1) out[0] = 1, out[1] = 1, out[2] = 0;
    else if (rel == 2 || units) out[0] = 1, out[1] = 0, out[2] = 0;
    else out[0] = out[1] = out[2] = 0.502f;
}

static uint32_t tile_color(const Snapshot *s, int x, int y) {
    int i = y * map_w + x;
    uint16_t st = s->state[i];
    int owner = st & OWNER_MASK;
    if (st & FALLOUT_BIT) {
        // Stale-nuke ground: dark green with per-tile noise.
        float h = sinf((float)x * 12.9898f + (float)y * 78.233f) * 43758.5453f;
        h -= floorf(h);
        float n = h * 0.05f;
        return pack(0.05f + n, 0.55f + n, 0.07f + n);
    }
    int tr, tg, tb;
    terrain_rgb(s->terrain[i], &tr, &tg, &tb);
    if (owner == 0 || !pal[owner].valid) return 0xff000000u | ((uint32_t)tr << 16) | ((uint32_t)tg << 8) | (uint32_t)tb;
    const Pal *p = &pal[owner];
    int n = owner_at(s, x, y - 1), so = owner_at(s, x, y + 1), w = owner_at(s, x - 1, y), e = owner_at(s, x + 1, y);
    bool border = n != owner || so != owner || w != owner || e != owner;
    if (alt_view) {
        float ac[3];
        affiliation(s, owner, false, ac);
        if (border) return pack(ac[0], ac[1], ac[2]);
        const float fa = 0.15f;
        return pack(tr / 255.0f * (1 - fa) + ac[0] * fa, tg / 255.0f * (1 - fa) + ac[1] * fa, tb / 255.0f * (1 - fa) + ac[2] * fa);
    }
    bool hl = highlight != 0 && owner == highlight;
    bool hl_border = false;
    if (hl && !border) {
        // highlightThicken = 1: the ring one tile in also outlines.
        for (int dy = -1; dy <= 1 && !hl_border; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (owner_at(s, x + dx, y + dy) != owner) {
                    hl_border = true;
                    break;
                }
            }
        }
    }
    bool defended = coverage && coverage[i] == owner;
    if (border || hl_border) {
        float bc[3] = {p->border[0], p->border[1], p->border[2]};
        if (hl) {
            for (int k = 0; k < 3; k++) bc[k] += (1 - bc[k]) * HIGHLIGHT_BRIGHTEN;
        }
        int rel = 0;
        if (s->relations && owner < MAX_REL) {
            int nb[4] = {n, so, w, e};
            for (int k = 0; k < 4; k++) {
                if (nb[k] != owner && nb[k] != 0 && nb[k] < MAX_REL) {
                    int r = s->relations[owner * MAX_REL + nb[k]];
                    if (r > rel) rel = r;
                }
            }
        }
        if (rel == 2) {
            bc[0] += (1 - bc[0]) * TINT_RATIO, bc[1] *= 1 - TINT_RATIO, bc[2] *= 1 - TINT_RATIO;
        } else if (rel == 1) {
            bc[0] *= 1 - TINT_RATIO, bc[1] += (1 - bc[1]) * TINT_RATIO, bc[2] *= 1 - TINT_RATIO;
        }
        if (defended && ((x + y) & 1)) {
            for (int k = 0; k < 3; k++) bc[k] *= DEFENSE_CHECKER_DARKEN;
        }
        return pack(bc[0], bc[1], bc[2]);
    }
    const float *f = hl ? (defended ? p->fill_hl_def : p->fill_hl) : (defended ? p->fill_def : p->fill);
    const float a = TERRITORY_ALPHA;
    return pack(tr / 255.0f * (1 - a) + f[0] * a, tg / 255.0f * (1 - a) + f[1] * a, tb / 255.0f * (1 - a) + f[2] * a);
}

static void mark(int x0, int y0, int x1, int y1) {
    if (x0 < dirty_x0) dirty_x0 = x0;
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (x1 > dirty_x1) dirty_x1 = x1;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

/** Recolours a tile and its 8 neighbours (borders depend on them). */
static void touch_block(const Snapshot *s, int x, int y) {
    int x0 = x > 0 ? x - 1 : 0, x1 = x < map_w - 1 ? x + 1 : x;
    int y0 = y > 0 ? y - 1 : 0, y1 = y < map_h - 1 ? y + 1 : y;
    for (int yy = y0; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) pixels[yy * map_w + xx] = tile_color(s, xx, yy);
    }
    mark(x0, y0, x1 + 1, y1 + 1);
}

/** DefenseCoveragePass: tiles within range of a finished defense post. */
static bool update_coverage(const Snapshot *s) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        if (u->kind != UNIT_DEFENSE_POST || (u->flags & UNIT_UNDER_CONSTRUCTION)) continue;
        h = (h ^ (uint32_t)u->id) * 16777619u;
        h = (h ^ (uint32_t)(u->x * 65536 + u->y)) * 16777619u;
        h = (h ^ (uint32_t)u->owner) * 16777619u;
    }
    if (coverage && h == coverage_hash) return false;
    coverage_hash = h;
    if (!coverage) coverage = malloc(sizeof(uint16_t) * (size_t)map_w * (size_t)map_h);
    memset(coverage, 0, sizeof(uint16_t) * (size_t)map_w * (size_t)map_h);
    const int r = DEFENSE_POST_RANGE;
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        if (u->kind != UNIT_DEFENSE_POST || (u->flags & UNIT_UNDER_CONSTRUCTION)) continue;
        for (int dy = -r; dy <= r; dy++) {
            int y = u->y + dy;
            if (y < 0 || y >= map_h) continue;
            for (int dx = -r; dx <= r; dx++) {
                int x = u->x + dx;
                if (x < 0 || x >= map_w || dx * dx + dy * dy > r * r) continue;
                coverage[y * map_w + x] = (uint16_t)u->owner;
            }
        }
    }
    return true;
}

void map_reset(void) {
    if (map_tex) SDL_DestroyTexture(map_tex);
    map_tex = NULL;
    free(pixels);
    pixels = NULL;
    free(coverage);
    coverage = NULL;
    coverage_hash = 0;
    relations_version = -1;
    highlight = 0;
    memset(pal_src, 0, sizeof pal_src);
    memset(pal, 0, sizeof pal);
    map_w = map_h = 0;
}

void map_set_view(bool alt, int my_sid) {
    if (alt != alt_view || my_sid != local_sid) view_dirty = true;
    alt_view = alt;
    local_sid = my_sid;
}

void map_update(const Snapshot *s, int hover_owner) {
    uint32_t *refs = NULL;
    int full = 0;
    int n = engine_take_dirty(&refs, &full);
    if (s->status != GAME_RUNNING || !s->terrain) {
        free(refs);
        return;
    }
    if (!map_tex || map_w != s->width || map_h != s->height) {
        map_reset();
        map_w = s->width;
        map_h = s->height;
        map_tex = SDL_CreateTexture(gfx_renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, map_w, map_h);
        SDL_SetTextureScaleMode(map_tex, SDL_ScaleModeNearest);
        pixels = malloc(sizeof(uint32_t) * (size_t)map_w * (size_t)map_h);
        full = 1;
    }
    uint8_t old_src[sizeof pal_src];
    memcpy(old_src, pal_src, sizeof pal_src);
    build_palette(s);
    if (memcmp(old_src, pal_src, sizeof pal_src) != 0) full = 1;
    if (update_coverage(s)) full = 1;
    if (s->relations_version != relations_version) {
        relations_version = s->relations_version;
        full = 1;
    }
    if (view_dirty) {
        view_dirty = false;
        full = 1;
    }
    dirty_x0 = map_w, dirty_y0 = map_h, dirty_x1 = 0, dirty_y1 = 0;
    int old_hl = highlight;
    highlight = hover_owner;
    if (full) {
        for (int y = 0; y < map_h; y++) {
            for (int x = 0; x < map_w; x++) pixels[y * map_w + x] = tile_color(s, x, y);
        }
        mark(0, 0, map_w, map_h);
    } else {
        for (int k = 0; k < n; k++) {
            int ref = (int)refs[k];
            int x = ref % map_w, y = ref / map_w;
            if (y < map_h) touch_block(s, x, y);
        }
        if (old_hl != highlight) {
            // Only the old and new hovered owners' tiles change.
            for (int y = 0; y < map_h; y++) {
                const uint16_t *row = &s->state[y * map_w];
                for (int x = 0; x < map_w; x++) {
                    int o = row[x] & OWNER_MASK;
                    if (o != 0 && (o == old_hl || o == highlight)) {
                        pixels[y * map_w + x] = tile_color(s, x, y);
                        mark(x, y, x + 1, y + 1);
                    }
                }
            }
        }
    }
    free(refs);
    if (dirty_x1 > dirty_x0 && dirty_y1 > dirty_y0) {
        SDL_Rect r = {dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0};
        SDL_UpdateTexture(map_tex, &r, pixels + dirty_y0 * map_w + dirty_x0, map_w * 4);
    }
}

// --- Camera ---------------------------------------------------------------------------

void cam_page_to_tile(const Camera *c, float px, float py, float *tx, float *ty) {
    *tx = c->cx + (px - PAGE_W / 2.0f) / c->zoom;
    *ty = c->cy + (py - PAGE_H / 2.0f) / c->zoom;
}

void cam_tile_to_page(const Camera *c, float tx, float ty, float *px, float *py) {
    *px = (tx - c->cx) * c->zoom + PAGE_W / 2.0f;
    *py = (ty - c->cy) * c->zoom + PAGE_H / 2.0f;
}

void cam_fit(Camera *c, int mw, int mh) {
    c->cx = mw / 2.0f;
    c->cy = mh / 2.0f;
    c->zoom = fminf((float)PAGE_W / (float)mw, (float)PAGE_H / (float)mh) * 0.9f;
}

void cam_clamp(Camera *c, int mw, int mh) {
    if (c->zoom < CAM_MIN_ZOOM) c->zoom = CAM_MIN_ZOOM;
    if (c->zoom > CAM_MAX_ZOOM) c->zoom = CAM_MAX_ZOOM;
    if (c->cx < 0) c->cx = 0;
    if (c->cy < 0) c->cy = 0;
    if (c->cx > (float)mw) c->cx = (float)mw;
    if (c->cy > (float)mh) c->cy = (float)mh;
}

void cam_zoom_at(Camera *c, float factor, float px, float py) {
    float tx, ty;
    cam_page_to_tile(c, px, py, &tx, &ty);
    c->zoom = fmaxf(CAM_MIN_ZOOM, fminf(CAM_MAX_ZOOM, c->zoom * factor));
    // Keep the tile under the point where it was.
    c->cx = tx - (px - PAGE_W / 2.0f) / c->zoom;
    c->cy = ty - (py - PAGE_H / 2.0f) / c->zoom;
}

// --- Sprites ------------------------------------------------------------------------------

#define SHAPE_TEX 128
#define STRUCT_KINDS 6

// Atlas column (icon-atlas.png / StructurePass) of each structure kind.
static const int STRUCT_COL[STRUCT_KINDS] = {
    [UNIT_CITY] = 0, [UNIT_PORT] = 1, [UNIT_FACTORY] = 2,
    [UNIT_DEFENSE_POST] = 3, [UNIT_SAM_LAUNCHER] = 4, [UNIT_MISSILE_SILO] = 5,
};
// structure.shapes in render-settings.json, by atlas column.
static const float SHAPE_SCALE[6] = {1.0f, 1.08f, 1.08f, 1.0f, 1.4f, 1.55f};
static const float ICON_FILL[6] = {0.85f, 0.85f, 0.85f, 0.8f, 1.0f, 0.85f};

static SDL_Texture *shape_outer[6], *shape_inner[6], *shape_icon[6], *shape_cross;
static SDL_Texture *unit_band[4];  // unit-atlas split into its grey bands
static SDL_Texture *status_tex, *emoji_tex;
static int emoji_cells[256];  // flattened emoji index -> atlas cell, -1 none
static int nemoji;

static float sd_polygon(float px, float py, float r, float n, float rot) {
    float an = PI_F / n;
    float a = atan2f(py, px) - rot;
    a = fmodf(a + an, 2 * an);
    if (a < 0) a += 2 * an;
    a -= an;
    return sqrtf(px * px + py * py) * cosf(a) - r * cosf(an);
}

/** structure.frag shapeSDF(): circle, pentagon, hexagon, octagon, square, triangle. */
static float shape_sdf(int col, float x, float y, float r) {
    switch (col) {
        case 0: return sqrtf(x * x + y * y) - r;
        case 1: return sd_polygon(x, y, r, 5, PI_F * 0.5f);
        case 2: return sd_polygon(x, y, r, 6, PI_F / 6);
        case 3: return sd_polygon(x, y, r, 8, 0);
        case 4: return sd_polygon(x, y, r, 4, 0);
        default: return sd_polygon(x, y, r, 3, PI_F * 0.5f);
    }
}

static SDL_Texture *texture_from(uint32_t *px, int w, int h, bool nearest) {
    SDL_Texture *t = SDL_CreateTexture(gfx_renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, nearest ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    return t;
}

static SDL_Surface *load_rgba(const char *rel) {
    char path[512];
    snprintf(path, sizeof path, "%s%s", root, rel);
    SDL_Surface *s = IMG_Load(path);
    if (!s) {
        fprintf(stderr, "map: %s: %s\n", path, IMG_GetError());
        return NULL;
    }
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    return c;
}

static uint32_t pixel(SDL_Surface *s, int x, int y) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return ((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x];
}

static void build_structure_sprites(void) {
    SDL_Surface *atlas = load_rgba("atlases/icon-atlas.png");
    uint32_t *outer = malloc(SHAPE_TEX * SHAPE_TEX * 4), *inner = malloc(SHAPE_TEX * SHAPE_TEX * 4);
    uint32_t *icon = malloc(SHAPE_TEX * SHAPE_TEX * 4);
    const float texel = 1.0f / SHAPE_TEX;
    for (int col = 0; col < 6; col++) {
        float bw = 0.06f / SHAPE_SCALE[col];
        float expand = SHAPE_SCALE[col] / ICON_FILL[col];
        int cell = atlas ? atlas->h : 64;
        for (int y = 0; y < SHAPE_TEX; y++) {
            for (int x = 0; x < SHAPE_TEX; x++) {
                float lx = (x + 0.5f) * texel - 0.5f, ly = (y + 0.5f) * texel - 0.5f;
                float sdf = shape_sdf(col, lx, ly, 0.45f);
                float ao = fminf(1, fmaxf(0, 0.5f - sdf / texel));
                float ai = fminf(1, fmaxf(0, 0.5f - (sdf + bw) / texel));
                outer[y * SHAPE_TEX + x] = ((uint32_t)(ao * 255) << 24) | 0xffffff;
                inner[y * SHAPE_TEX + x] = ((uint32_t)(ai * 255) << 24) | 0xffffff;
                // The glyph, scaled like structure.vert's uvExpand, inside the fill.
                float u = 0.5f + lx * expand, v = 0.5f + ly * expand;
                float ia = 0;
                if (atlas && u >= 0 && u < 1 && v >= 0 && v < 1) {
                    // Bilinear sample of the column.
                    float sx = u * cell - 0.5f, sy = v * cell - 0.5f;
                    int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
                    float fx = sx - x0, fy = sy - y0;
                    float acc = 0;
                    for (int k = 0; k < 4; k++) {
                        int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                        if (xx < 0 || yy < 0 || xx >= cell || yy >= cell) continue;
                        float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                        acc += wgt * (float)(pixel(atlas, col * cell + xx, yy) >> 24) / 255.0f;
                    }
                    ia = acc;
                }
                ia *= ai;
                icon[y * SHAPE_TEX + x] = ((uint32_t)(ia * 255) << 24) | 0xffffff;
            }
        }
        shape_outer[col] = texture_from(outer, SHAPE_TEX, SHAPE_TEX, false);
        shape_inner[col] = texture_from(inner, SHAPE_TEX, SHAPE_TEX, false);
        shape_icon[col] = texture_from(icon, SHAPE_TEX, SHAPE_TEX, false);
    }
    // The red X over structures marked for deletion.
    for (int y = 0; y < SHAPE_TEX; y++) {
        for (int x = 0; x < SHAPE_TEX; x++) {
            float lx = (x + 0.5f) * texel - 0.5f, ly = (y + 0.5f) * texel - 0.5f;
            float d = fminf(fabsf(lx - ly), fabsf(lx + ly)) * 0.7071f;
            float dist = sqrtf(lx * lx + ly * ly);
            float a = fminf(1, fmaxf(0, 0.5f - (d - 0.025f) / texel)) * (dist < 0.45f * 1.55f * 0.5f + 0.2f ? 1.f : 0.f);
            outer[y * SHAPE_TEX + x] = ((uint32_t)(a * 0.95f * 255) << 24) | 0xff4040;
        }
    }
    shape_cross = texture_from(outer, SHAPE_TEX, SHAPE_TEX, false);
    free(outer);
    free(inner);
    free(icon);
    if (atlas) SDL_FreeSurface(atlas);
}

static void build_unit_sprites(void) {
    SDL_Surface *atlas = load_rgba("atlases/unit-atlas.png");
    if (!atlas) return;
    int w = atlas->w, h = atlas->h;
    uint32_t *band = malloc((size_t)w * (size_t)h * 4);
    for (int b = 0; b < 4; b++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t p = pixel(atlas, x, y);
                float a = (float)(p >> 24) / 255.0f, g = (float)((p >> 16) & 255) / 255.0f;
                // unit.frag: >0.6 territory, >0.45 mid, >0.34 centre, else border.
                int which = g > 0.6f ? 0 : g > 0.45f ? 1 : g > 0.34f ? 2 : 3;
                band[y * w + x] = (a >= 0.01f && which == b) ? (p & 0xff000000u) | 0xffffff : 0;
            }
        }
        unit_band[b] = texture_from(band, w, h, true);
    }
    free(band);
    SDL_FreeSurface(atlas);
}

void map_set_emojis(const cJSON *list) {
    // emoji-atlas-meta.json maps each emoji to its cell.
    char path[512];
    snprintf(path, sizeof path, "%satlases/emoji-atlas-meta.json", root);
    FILE *f = fopen(path, "rb");
    nemoji = 0;
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    buf[fread(buf, 1, (size_t)n, f)] = 0;
    fclose(f);
    cJSON *meta = cJSON_Parse(buf);
    free(buf);
    const cJSON *cells = jget(meta, "emojis");
    for (int i = 0; i < jlen(list) && i < 256; i++) {
        const cJSON *e = jat(list, i);
        const cJSON *c = cJSON_IsString(e) ? jget(cells, e->valuestring) : NULL;
        emoji_cells[i] = cJSON_IsNumber(c) ? c->valueint : -1;
        nemoji = i + 1;
    }
    cJSON_Delete(meta);
}

bool map_init(const char *romfs) {
    snprintf(root, sizeof root, "%s", romfs);
    build_structure_sprites();
    build_unit_sprites();
    char path[512];
    snprintf(path, sizeof path, "%satlases/status-atlas.png", root);
    status_tex = gfx_load_texture(path, NULL, NULL);
    snprintf(path, sizeof path, "%satlases/emoji-atlas.png", root);
    emoji_tex = gfx_load_texture(path, NULL, NULL);
    return shape_outer[0] != NULL;
}

void map_shutdown(void) {
    map_reset();
    for (int i = 0; i < 6; i++) {
        if (shape_outer[i]) SDL_DestroyTexture(shape_outer[i]);
        if (shape_inner[i]) SDL_DestroyTexture(shape_inner[i]);
        if (shape_icon[i]) SDL_DestroyTexture(shape_icon[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (unit_band[i]) SDL_DestroyTexture(unit_band[i]);
    }
    if (shape_cross) SDL_DestroyTexture(shape_cross);
    if (status_tex) SDL_DestroyTexture(status_tex);
    if (emoji_tex) SDL_DestroyTexture(emoji_tex);
}

// --- Colour helpers ---------------------------------------------------------------------

static void rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v) {
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) *h = 0;
    else if (mx == r) *h = fmodf((g - b) / d + 6, 6) / 6;
    else if (mx == g) *h = ((b - r) / d + 2) / 6;
    else *h = ((r - g) / d + 4) / 6;
}

static SDL_Color hsv_darken(const uint8_t rgb[3], float vscale) {
    float h, s, v;
    rgb_to_hsv(rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, &h, &s, &v);
    v *= vscale;
    float r = 0, g = 0, b = 0;
    float k[3] = {fmodf(h * 6 + 5, 6), fmodf(h * 6 + 3, 6), fmodf(h * 6 + 1, 6)};
    float out[3];
    for (int i = 0; i < 3; i++) {
        float t = fminf(k[i], fminf(4 - k[i], 1));
        out[i] = v - v * s * fmaxf(0, t);
    }
    r = out[0], g = out[1], b = out[2];
    return rgba((int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f), 255);
}

static SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    return rgba((int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t), (int)(a.b + (b.b - a.b) * t), a.a);
}

static SDL_Color territory(int owner) {
    if (owner <= 0 || owner >= MAX_PLAYERS || !pal[owner].valid) return rgba(200, 200, 200, 255);
    return rgba(pal[owner].raw[0], pal[owner].raw[1], pal[owner].raw[2], 255);
}

static SDL_Color border_of(int owner) {
    if (owner <= 0 || owner >= MAX_PLAYERS || !pal[owner].valid) return rgba(120, 120, 120, 255);
    return rgba(pal[owner].raw_border[0], pal[owner].raw_border[1], pal[owner].raw_border[2], 255);
}

// --- Structures ----------------------------------------------------------------------------

static const Snapshot *cur_snap;  // the snapshot being drawn

static float icon_scale(float zoom) {
    if (zoom <= DOTS_ZOOM) return DOT_SCALE;
    if (zoom >= ICON_GROW_ZOOM) return zoom / ICON_GROW_ZOOM;
    return fminf(1, zoom / ICON_SCALE_ZOOMED_OUT);
}

static void draw_structure(int col, float cx, float cy, float zoom, int owner, bool constructing, bool deleting,
                           float alpha, SDL_Color tint, float tint_amount) {
    float size = ICON_SIZE * icon_scale(zoom) * SHAPE_SCALE[col];
    if (size < 1.5f) return;
    Rect r = {cx - size / 2, cy - size / 2, size, size};
    if (r.x > PAGE_W || r.y > PAGE_H || r.x + size < 0 || r.y + size < 0) return;
    SDL_Color fill, border;
    if (constructing) {
        fill = rgba(198, 198, 198, 255);
        border = rgba(127, 127, 127, 255);
    } else if (alt_view && cur_snap) {
        float ac[3];
        affiliation(cur_snap, owner, true, ac);
        uint8_t a8[3] = {(uint8_t)(ac[0] * 255), (uint8_t)(ac[1] * 255), (uint8_t)(ac[2] * 255)};
        fill = hsv_darken(a8, FILL_DARKEN);
        border = hsv_darken(a8, BORDER_DARKEN);
    } else {
        const Pal *p = owner > 0 && owner < MAX_PLAYERS && pal[owner].valid ? &pal[owner] : NULL;
        static const uint8_t grey[3] = {200, 200, 200};
        fill = hsv_darken(p ? p->raw : grey, FILL_DARKEN);
        // The local player's structures take its territory colour as border.
        border = hsv_darken(p ? (p->me ? p->raw : p->raw_border) : grey, BORDER_DARKEN);
    }
    SDL_Color white = rgba(255, 255, 255, 255);
    if (tint_amount > 0) {
        fill = mix(fill, tint, tint_amount);
        border = mix(border, tint, tint_amount);
        white = mix(white, tint, tint_amount);
    }
    Uint8 a = (Uint8)(255 * alpha);
    fill.a = border.a = white.a = a;
    gfx_image(shape_outer[col], NULL, r, border);
    gfx_image(shape_inner[col], NULL, r, fill);
    if (zoom > DOTS_ZOOM) gfx_image(shape_icon[col], NULL, r, white);
    if (deleting) gfx_image(shape_cross, NULL, r, rgba(255, 255, 255, a));
}

static void draw_structures(const Snapshot *s, const Camera *cam) {
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        if (u->kind < 0 || u->kind >= STRUCT_KINDS) continue;
        float x, y;
        cam_tile_to_page(cam, u->x + 0.5f, u->y + 0.5f, &x, &y);
        draw_structure(STRUCT_COL[u->kind], x, y, cam->zoom, u->owner, u->flags & UNIT_UNDER_CONSTRUCTION,
                       u->flags & UNIT_MARKED_FOR_DELETION, 1, rgba(0, 0, 0, 255), 0);
    }
    // StructureLevelPass: the level above structures past level 1.
    if (cam->zoom <= DOTS_ZOOM) return;
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        if (u->kind < 0 || u->kind >= STRUCT_KINDS || u->level <= 1) continue;
        float x, y;
        cam_tile_to_page(cam, u->x + 0.5f, u->y + 0.5f, &x, &y);
        float half = ICON_SIZE * icon_scale(cam->zoom) * 0.5f;
        if (x < -half || y < -half || x > PAGE_W + half || y > PAGE_H + half) continue;
        char lv[16];
        snprintf(lv, sizeof lv, "%d", u->level);
        float px = half * 0.9f;
        gfx_text_baseline(FONT_800, px, lv, x, y - half * 0.6f + px * 0.1f, rgba(255, 255, 255, 255),
                          rgba(0, 0, 0, 220), fmaxf(1, px / 10), ALIGN_CENTER);
    }
}

// --- Units -----------------------------------------------------------------------------------

// UnitPass atlas columns, by our unit kind.
static int unit_col(int kind) {
    switch (kind) {
        case UNIT_TRANSPORT: return 0;
        case UNIT_TRADE_SHIP: return 1;
        case UNIT_WARSHIP: return 2;
        case UNIT_ATOM_BOMB: return 3;
        case UNIT_HYDROGEN_BOMB: return 4;
        case UNIT_MIRV: return 5;
        case UNIT_SAM_MISSILE: return 6;
        case UNIT_SHELL: return 7;
        case UNIT_MIRV_WARHEAD: return 8;
        case UNIT_TRAIN: return 10;
        default: return -1;
    }
}

static bool is_missile(int kind) {
    return kind == UNIT_ATOM_BOMB || kind == UNIT_HYDROGEN_BOMB || kind == UNIT_MIRV || kind == UNIT_MIRV_WARHEAD ||
           kind == UNIT_SHELL || kind == UNIT_SAM_MISSILE;
}

static void draw_units(const Snapshot *s, const Camera *cam, bool missiles) {
    static const SDL_Color flicker[4] = {{255, 0, 0, 255}, {255, 128, 0, 255}, {255, 255, 0, 255}, {255, 255, 255, 255}};
    double now = SDL_GetTicks() / 1000.0;
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        int col = unit_col(u->kind);
        if (col < 0 || is_missile(u->kind) != missiles) continue;
        float x, y;
        cam_tile_to_page(cam, u->x + 0.5f, u->y + 0.5f, &x, &y);
        float size = UNIT_SIZE * cam->zoom;
        if (x < -size || y < -size || x > PAGE_W + size || y > PAGE_H + size) continue;
        SDL_Color t = territory(u->owner), b = border_of(u->owner);
        float blink = 0;
        if (u->kind == UNIT_ATOM_BOMB || u->kind == UNIT_HYDROGEN_BOMB || u->kind == UNIT_MIRV ||
            u->kind == UNIT_MIRV_WARHEAD) {
            float hash = (float)((u->id * 37) & 255) / 255.0f;
            float phase = (float)s->tick * 0.3f + hash;
            int idx = (int)((phase - floorf(phase)) * 4) % 4;
            t = flicker[idx];
            b = flicker[(idx + 2) % 4];
        } else if (u->flags & UNIT_RETREATING) {
            float ph = (float)s->tick * 0.07f;
            blink = ph - floorf(ph) >= 0.5f ? 1 : 0;
        }
        if (u->kind == UNIT_HYDROGEN_BOMB) {
            // The bomb's warm glow halo.
            float gr = size * HBOMB_GLOW_SCALE / 2;
            for (int k = 6; k >= 1; k--) {
                float rr = gr * (0.45f + 0.55f * (float)k / 6);
                gfx_fill_circle(x, y, rr, rgba(255, 184, 38, (int)(255 * 0.5f / 6)));
            }
        }
        if (alt_view) {
            float ac[3];
            affiliation(s, u->owner, true, ac);
            if (u->kind == UNIT_TRADE_SHIP && (u->flags & UNIT_TRADE_SELF)) ac[0] = 0, ac[1] = 1, ac[2] = 0;
            else if (u->kind == UNIT_TRADE_SHIP && (u->flags & UNIT_TRADE_FRIENDLY)) ac[0] = 1, ac[1] = 1, ac[2] = 0;
            t = b = rgba((int)(ac[0] * 255), (int)(ac[1] * 255), (int)(ac[2] * 255), 255);
            blink = 0;
        }
        SDL_Color mid = mix(t, b, 0.5f), centre = mix(t, rgba(0, 0, 0, 255), blink);
        SDL_Color bands[4] = {t, mid, centre, b};
        SDL_Rect src = {col * 13, 0, 13, 13};
        Rect dst = {x - size / 2, y - size / 2, size, size};
        for (int k = 0; k < 4; k++) gfx_image(unit_band[k], &src, dst, bands[k]);
        (void)now;
    }
}

// --- Nuke telegraphs, ghost, selection -----------------------------------------------------

/** A dashed ring: dashes of `dash` and gaps of `gap` page px, moving by `offset`. */
static void dashed_ring(float cx, float cy, float r, float width, float dash, float gap, float offset, SDL_Color c) {
    if (r < 1) return;
    float circ = 2 * PI_F * r;
    float period = dash + gap;
    int n = (int)(circ / period);
    if (n < 1 || n > 720) {
        gfx_fill_arc(cx, cy, r - width / 2, r + width / 2, 0, 2 * PI_F, c);
        return;
    }
    period = circ / (float)n;
    float da = dash / r, pa = period / r, oa = fmodf(offset / r, pa);
    for (int i = 0; i < n; i++) {
        float a0 = oa + pa * (float)i;
        gfx_fill_arc(cx, cy, r - width / 2, r + width / 2, a0, a0 + da, c);
    }
}

static void draw_telegraphs(const Snapshot *s, const Camera *cam, int me) {
    float t = (float)SDL_GetTicks() / 1000.0f;
    float base = 0.85f + 0.1f * sinf(t * 3);
    for (int i = 0; i < s->nunits; i++) {
        const UnitRec *u = &s->units[i];
        float inner, outer;
        if (u->kind == UNIT_ATOM_BOMB) inner = 12, outer = 30;
        else if (u->kind == UNIT_HYDROGEN_BOMB) inner = 80, outer = 100;
        else if (u->kind == UNIT_MIRV_WARHEAD) inner = 12, outer = 18;
        else continue;
        if (u->tx < 0) continue;
        SDL_Color c = rgba(255, 0, 0, 255);
        if (me > 0 && u->owner == me) c = rgba(0, 255, 0, 255);
        else if (me > 0 && s->relations && me < MAX_REL && u->owner < MAX_REL && s->relations[me * MAX_REL + u->owner] == 1)
            c = rgba(255, 255, 0, 255);
        float x, y;
        cam_tile_to_page(cam, u->tx + 0.5f, u->ty + 0.5f, &x, &y);
        float z = cam->zoom;
        if (x + outer * z < 0 || y + outer * z < 0 || x - outer * z > PAGE_W || y - outer * z > PAGE_H) continue;
        gfx_fill_circle(x, y, inner * z, alpha(c, fmaxf(0, base - 0.6f)));
        // strokeWidth, dash and gap are world units in nuke-telegraph.frag.
        float sw = 1.5f * z;
        gfx_fill_arc(x, y, fmaxf(0, inner * z - sw), inner * z + sw, 0, 2 * PI_F, alpha(c, base));
        dashed_ring(x, y, outer * z, 2 * sw, 12 * z, 6 * z, t * 20 * z, alpha(c, base));
    }
}

/** A cubic Bézier point. */
static void bezier(const float p[8], float t, float *x, float *y) {
    float u = 1 - t;
    *x = u * u * u * p[0] + 3 * u * u * t * p[2] + 3 * u * t * t * p[4] + t * t * t * p[6];
    *y = u * u * u * p[1] + 3 * u * u * t * p[3] + 3 * u * t * t * p[5] + t * t * t * p[7];
}

static void draw_trajectory(const cJSON *tr, const Camera *cam) {
    if (!cJSON_IsObject(tr)) return;
    float p[8];
    static const char *keys[8] = {"p0x", "p0y", "p1x", "p1y", "p2x", "p2y", "p3x", "p3y"};
    for (int k = 0; k < 8; k++) p[k] = (float)jnum(tr, keys[k], 0) + 0.5f;
    float us = (float)jnum(tr, "tUntargetableStart", -1), ue = (float)jnum(tr, "tUntargetableEnd", -1);
    float intercept = (float)jnum(tr, "tSamIntercept", 1);
    // Walk the curve in screen space, dash by dash.
    const int steps = 400;
    float px[401], py[401], len[401];
    for (int i = 0; i <= steps; i++) {
        float x, y;
        bezier(p, (float)i / steps, &x, &y);
        cam_tile_to_page(cam, x, y, &px[i], &py[i]);
        len[i] = i ? len[i - 1] + hypotf(px[i] - px[i - 1], py[i] - py[i - 1]) : 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < steps; i++) {
            float t = ((float)i + 0.5f) / steps;
            bool untargetable = us >= 0 && t >= us && t <= ue;
            bool red = t > intercept;
            float dash = untargetable ? 2 : 8, gap = untargetable ? 6 : 4;
            float m = fmodf(len[i], dash + gap);
            if (m > dash) continue;
            SDL_Color line = red ? rgba(255, 80, 80, 255) : rgba(255, 255, 255, 255);
            SDL_Color outline = red ? rgba(150, 90, 90, 255) : rgba(140, 140, 140, 255);
            if (pass == 0) gfx_line(px[i], py[i], px[i + 1], py[i + 1], 2.5f + 3, outline);
            else gfx_line(px[i], py[i], px[i + 1], py[i + 1], 2.5f, line);
        }
    }
    float ex = px[steps], ey = py[steps];
    gfx_stroke_circle(ex, ey, 6, 2, rgba(255, 255, 255, 255));
    if (intercept < 1) {
        int k = (int)(intercept * steps);
        float ix = px[k], iy = py[k], r = 6;
        gfx_line(ix - r, iy - r, ix + r, iy + r, 3, rgba(255, 80, 80, 255));
        gfx_line(ix - r, iy + r, ix + r, iy - r, 3, rgba(255, 80, 80, 255));
    }
}

static int struct_col_for(const char *type) {
    if (!strcmp(type, "City")) return 0;
    if (!strcmp(type, "Port")) return 1;
    if (!strcmp(type, "Factory")) return 2;
    if (!strcmp(type, "Defense Post")) return 3;
    if (!strcmp(type, "SAM Launcher")) return 4;
    if (!strcmp(type, "Missile Silo")) return 5;
    return -1;
}

static void draw_ghost(const MapOverlay *ov, const Camera *cam) {
    const cJSON *g = ov->ghost;
    if (!g) return;
    draw_trajectory(jget(g, "trajectory"), cam);
    if (!jbool(g, "valid")) return;
    float z = cam->zoom;
    float range = (float)jnum(g, "range", 0);
    if (range > 0) {
        float rx, ry;
        cam_tile_to_page(cam, (float)jnum(g, "rx", 0) + 0.5f, (float)jnum(g, "ry", 0) + 0.5f, &rx, &ry);
        SDL_Color c = rgba(255, 255, 255, 255);
        gfx_fill_circle(rx, ry, range * z, alpha(c, 0.2f));
        gfx_fill_arc(rx, ry, (range - 1) * z, range * z, 0, 2 * PI_F, alpha(c, 0.5f));
    }
    int col = struct_col_for(jstr(g, "type", ""));
    float gx, gy;
    cam_tile_to_page(cam, (float)jnum(g, "x", 0) + 0.5f, (float)jnum(g, "y", 0) + 0.5f, &gx, &gy);
    if (col >= 0) {
        bool up = jbool(g, "canUpgrade"), build = jbool(g, "canBuild");
        if (up && jnum(g, "upgradeX", -1) >= 0) {
            float ux, uy;
            cam_tile_to_page(cam, (float)jnum(g, "upgradeX", 0) + 0.5f, (float)jnum(g, "upgradeY", 0) + 0.5f, &ux, &uy);
            draw_structure(col, ux, uy, z, ov->my_sid, false, false, 0.6f, rgba(0, 204, 0, 255), 0.5f);
        }
        SDL_Color tint = up ? rgba(0, 204, 0, 255) : build ? rgba(0, 0, 0, 255) : rgba(204, 51, 51, 255);
        draw_structure(col, gx, gy, z, ov->my_sid, false, false, 0.5f, tint, up || !build ? 0.5f : 0);
    }
    // ghostCost: the price under the preview.
    const char *cost = jstr(g, "cost", NULL);
    if (cost) {
        bool afford = jbool(g, "canAfford");
        float fs = 14;
        float w = gfx_text_width(FONT_700, fs, cost) + 16;
        float cy = gy + 25;
        gfx_icon("GoldCoinIcon", (Rect){gx - w / 2, cy - 7, 13, 13}, rgba(255, 255, 255, 255), ICON_NORMAL);
        gfx_text_baseline(FONT_700, fs, cost, gx - w / 2 + 16, cy + 5, afford ? rgba(255, 255, 255, 255) : rgba(255, 100, 103, 255),
                          rgba(0, 0, 0, 200), 1.5f, ALIGN_LEFT);
    }
}

static void draw_selection(const Snapshot *s, const Camera *cam, const MapOverlay *ov) {
    for (int k = 0; k < ov->nwarships; k++) {
        for (int i = 0; i < s->nunits; i++) {
            const UnitRec *u = &s->units[i];
            if (u->id != ov->warships[k]) continue;
            float x, y;
            cam_tile_to_page(cam, u->x + 0.5f, u->y + 0.5f, &x, &y);
            float r = fmaxf(UNIT_SIZE * cam->zoom * 0.5f, 8);
            gfx_stroke_circle(x, y, r, 2, rgba(255, 255, 255, 230));
            break;
        }
    }
}

// --- Names -------------------------------------------------------------------------------------

static void status_icon(int idx, Rect r, float a) {
    if (!status_tex) return;
    SDL_Rect src = {(idx % 3) * 256 + 16, (idx / 3) * 256 + 16, 224, 224};
    gfx_image(status_tex, &src, r, rgba(255, 255, 255, (int)(255 * a)));
}

static void emoji_icon(int emoji, Rect r) {
    if (!emoji_tex || emoji <= 0 || emoji > nemoji) return;
    int cell = emoji_cells[emoji - 1];
    if (cell < 0) return;
    SDL_Rect src = {(cell % 10) * 128, (cell / 10) * 128, 128, 128};
    gfx_image(emoji_tex, &src, r, rgba(255, 255, 255, 255));
}

static void draw_names(const Snapshot *s, const Camera *cam) {
    const float cull = 0.004f * PAGE_W;  // cullThreshold 0.008 in clip units
    for (int i = 0; i < s->nlabels; i++) {
        const Label *l = &s->labels[i];
        float base = fmaxf(1, floorf(l->size));
        float name_size = fmaxf(4, floorf(base * NAME_SCALE_FACTOR));
        float name_scale = fminf(base * 0.25f, NAME_SCALE_CAP);
        float F = name_size * name_scale * cam->zoom;  // font size, page px
        if (F * 0.75f < cull) {
            if (!(l->icons & ICON_TARGET)) continue;
            F = cull / 0.75f;
        }
        float x, y;
        cam_tile_to_page(cam, l->x, l->y, &x, &y);
        if (x < -F * 8 || y < -F * 3 || x > PAGE_W + F * 8 || y > PAGE_H + F * 3) continue;
        const PlayerInfo *pi = engine_player(l->id);
        SDL_Color pc = territory(l->id);
        int shade = pi->type == 1 ? 77 : pi->type == 2 ? 102 : 0;
        SDL_Color fill = rgba(shade, shade, shade, 255);
        float ow = fminf(NAME_OUTLINE, fmaxf(0.4f, F / 16));
        float w = gfx_text_baseline(FONT_MAP, F, pi->name, x, y + 0.375f * F, fill, pc, ow, ALIGN_CENTER);
        char troops[32];
        format_troops(l->troops, troops, sizeof troops);
        gfx_text_baseline(FONT_MAP, F * TROOP_SIZE, troops, x, y + 1.05f * F, fill, pc, ow, ALIGN_CENTER);
        if (pi->flag[0]) {
            float fh = 0.9f * F, fw = fh * 1.5f;
            gfx_flag(pi->flag, (Rect){x - w / 2 - fw, y - fh / 2, fw, fh});
        }
        // Status row (crown, traitor, ... in atlas order) then the emoji.
        int slots[9], n = 0;
        static const int bits[9] = {ICON_CROWN, ICON_TRAITOR, ICON_DISCONNECTED, ICON_ALLIANCE,
                                    ICON_ALLIANCE_REQUEST, ICON_TARGET, ICON_EMBARGO, ICON_NUKE_RED, ICON_NUKE_WHITE};
        static const int atlas_idx[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
        for (int k = 0; k < 9; k++) {
            if (l->icons & bits[k]) {
                if (k == 8 && (l->icons & ICON_NUKE_RED)) continue;
                slots[n++] = atlas_idx[k];
            }
        }
        int items = n + (l->emoji > 0 ? 1 : 0);
        if (items == 0) continue;
        float sz = 0.75f * F * 1.1f, gap = sz * 0.15f;
        float total = (float)items * sz + (float)(items - 1) * gap;
        float sx = x - total / 2, sy = y - 0.75f * F * ROW_OFFSET;
        for (int k = 0; k < n; k++) status_icon(slots[k], (Rect){sx + (float)k * (sz + gap), sy, sz, sz}, 1);
        if (l->emoji > 0) emoji_icon(l->emoji, (Rect){sx + (float)n * (sz + gap), sy, sz, sz});
    }
}

// --- Spawn phase ---------------------------------------------------------------------------------

static void draw_spawn(const Snapshot *s, const Camera *cam, int me) {
    // SpawnOverlayPass: a pulsing ring around the local player's spawn.
    for (int i = 0; i < s->nlabels; i++) {
        const Label *l = &s->labels[i];
        if (l->id != me) continue;
        float x, y;
        cam_tile_to_page(cam, l->x, l->y, &x, &y);
        float t = (float)SDL_GetTicks() * 0.008f / 6.2832f;
        float k = 0.5f + 0.5f * sinf(t * 6.2832f);
        float r = (10 + 20 * k) * cam->zoom;
        gfx_fill_arc(x, y, r * 0.9f, r, 0, 2 * PI_F, rgba(255, 255, 255, 200));
        gfx_fill_circle(x, y, r * 0.9f, rgba(255, 255, 255, 40));
    }
}

void map_draw(const Snapshot *s, const Camera *cam, const MapOverlay *ov) {
    SDL_Renderer *ren = gfx_renderer();
    cur_snap = s;
    SDL_SetRenderDrawColor(ren, 0x3c, 0x3c, 0x3c, 255);
    SDL_RenderClear(ren);
    if (!map_tex) return;
    float x0, y0;
    cam_tile_to_page(cam, 0, 0, &x0, &y0);
    gfx_image(map_tex, NULL, (Rect){x0, y0, (float)map_w * cam->zoom, (float)map_h * cam->zoom}, rgba(255, 255, 255, 255));
    if (ov->spawn_phase) draw_spawn(s, cam, ov->my_sid);
    draw_units(s, cam, false);
    draw_structures(s, cam);
    draw_units(s, cam, true);
    draw_telegraphs(s, cam, ov->my_sid);
    draw_selection(s, cam, ov);
    draw_names(s, cam);
    draw_ghost(ov, cam);
}

// --- Map picker preview ------------------------------------------------------------------------

SDL_Texture *map_preview(const char *romfs, const char *folder, int *w, int *h) {
    char path[512];
    snprintf(path, sizeof path, "%smaps/%s/manifest.json", romfs, folder);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *m = cJSON_Parse(buf);
    const cJSON *m16 = jget(m, "map16x");
    int mw = (int)jnum(m16, "width", 0), mh = (int)jnum(m16, "height", 0);
    cJSON_Delete(m);
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
    SDL_Texture *t = texture_from(px, mw, mh, false);
    free(px);
    free(terrain);
    *w = mw;
    *h = mh;
    return t;
}

// --- Numbers ---------------------------------------------------------------------------------------

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
