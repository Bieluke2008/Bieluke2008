// The web client's overlays and modals: the player info card, PlayerPanel,
// send gold / troops, the emoji table, quick chat, the in-game menu, the
// exit confirmation and the win / death screen.
#pragma once

#include <stdbool.h>

#include "cJSON.h"

void menus_reset(void);
/** Collects RPC results; call every frame. */
void menus_poll(void);
/** The hover card follows the map tile under the cursor (valid=false hides it). */
void menus_hover_tile(int tx, int ty, bool valid);
/** Call after app.modal changes. */
void menus_on_modal_opened(void);
/** The hover card (under the modals). */
void menus_draw_overlay(void);
/** The open modal and the win screen, on top of everything. */
void menus_draw_modals(void);
bool menus_win_open(void);
/** B / Escape: closes the top modal; false if none was open. */
bool menus_back(void);
