// The map view: terrain + territory texture, units, names, and the camera.
#pragma once

#include <SDL.h>
#include <stdbool.h>

#include "engine.h"

typedef struct {
    float cx, cy;  // map tile at the screen centre
    float zoom;    // screen pixels per tile
} Camera;

void render_init(SDL_Renderer *r);
void render_shutdown(void);

/** Applies tile changes from the engine to the map texture. */
void render_update_map(const Snapshot *s);
/** Forgets the current map (new game). */
void render_reset(void);

void render_map(const Snapshot *s, const Camera *cam, int sw, int sh);

void cam_screen_to_tile(const Camera *cam, int sw, int sh, float sx, float sy, float *tx, float *ty);
void cam_tile_to_screen(const Camera *cam, int sw, int sh, float tx, float ty, float *sx, float *sy);
void cam_clamp(Camera *cam, int map_w, int map_h, int sw, int sh);

/** A small terrain preview of a map, for the menu (NULL if unavailable). */
SDL_Texture *render_map_preview(const char *romfs, const char *folder, int *w, int *h);

/** Renders troop counts like the web client (troops are stored x10). */
void format_troops(double troops, char *out, size_t n);
void format_number(double num, char *out, size_t n);
