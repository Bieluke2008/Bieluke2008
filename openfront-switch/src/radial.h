// The radial (context) menu of the web client.
#pragma once

#include <stdbool.h>

/** Opens the menu for map tile tx,ty at page point px,py. */
void radial_open(int tx, int ty, float px, float py);
void radial_close(void);
bool radial_is_open(void);
/** Collects RPC results; call every frame. */
void radial_poll(void);
/** Draws and handles input; call after the HUD so it sits on top. */
void radial_draw(void);
/** Picks item `index` of the current ring, as a click would. */
void radial_pick(int index);
/** B / Escape: back one level, or close. False if it was not open. */
bool radial_back(void);
