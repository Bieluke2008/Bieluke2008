// The web client's settings "Audio" tab: a volume slider per category,
// Test buttons and Reset. Shared by the in-game menu and the home screen.
#pragma once

#include <stdbool.h>

/** Draws the panel as a modal; between ui_begin() and ui_end(). True when closed. */
bool audio_panel_draw(void);
