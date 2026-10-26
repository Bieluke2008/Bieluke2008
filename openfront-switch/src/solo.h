// The solo game setup, styled after the web client's Solo modal.
#pragma once

#include "engine.h"
#include "input.h"

typedef enum { SOLO_NONE, SOLO_START, SOLO_QUIT } SoloResult;

void solo_init(const char *romfs);
void solo_shutdown(void);
/** Draws the screen and edits `o`; between ui_begin() and ui_end(). */
SoloResult solo_draw(StartOptions *o, int *speed_idx, const Input *in);
