// The web client's always-on HUD (control panel, hotbar, leaderboard,
// sidebar, events, attacks), drawn from the adapter's frame model.
#pragma once

#include "cJSON.h"
#include "gfx.h"

void hud_draw(const cJSON *frame);
/** The troop bar shared by the control panel and the hover card. */
void hud_troop_bar(Rect r, float troop_frac, float attack_frac, SDL_Color fill, SDL_Color attack_fill);
