// The home page, after the web client's main page: the background art, the
// title, the player name, SOLO, and the port's Audio, Controls and Quit.
#pragma once

#include "engine.h"
#include "gfx.h"
#include "input.h"

typedef enum { HOME_NONE, HOME_SOLO, HOME_MULTI, HOME_AUDIO, HOME_CONTROLS, HOME_QUIT } HomeResult;

void home_init(const char *romfs);
void home_shutdown(void);
/** Draws the page and edits the name in `o`; between ui_begin() and ui_end(). */
HomeResult home_draw(StartOptions *o, const Input *in);
/** renderSmallActionCard: a rounded, uppercase action button. */
bool home_button(const char *id, Rect r, const char *label, bool primary, float px);
/** The page's background and frame, also behind the Solo screen. */
void home_backdrop(void);
