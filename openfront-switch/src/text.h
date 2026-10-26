// Text drawing from a glyph cache: each glyph is rendered once at a base
// size and scaled, so names on the map can be drawn at any zoom.
#pragma once

#include <SDL.h>
#include <stdbool.h>

bool text_init(SDL_Renderer *r, const char *romfs);
void text_shutdown(void);

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } Align;

/** Draws UTF-8 `s` with its top at y and returns the width drawn. */
float text_draw(const char *s, float x, float y, float px, SDL_Color c, Align a);
/** Same, with a 1px dark (or light) outline for readability on the map. */
float text_draw_outlined(const char *s, float x, float y, float px, SDL_Color c,
                         SDL_Color outline, Align a);
float text_width(const char *s, float px);
