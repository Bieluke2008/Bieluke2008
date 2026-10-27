// LAN multiplayer screens: the server address, Create Lobby / Join Lobby
// (the web client's HostLobbyModal and JoinLobbyModal) and the lobby itself.
// The game server is OpenFront's own, run on a PC in the same network.
#pragma once

#include "engine.h"
#include "input.h"

typedef enum { MULTI_NONE, MULTI_BACK, MULTI_GAME } MultiResult;

/** Shows the multiplayer menu (from the home page). */
void multi_open(void);
/** Draws the current multiplayer screen; between ui_begin() and ui_end(). */
MultiResult multi_draw(StartOptions *o, const Input *in);
/** True while the game screen shows a multiplayer game. */
bool multi_in_game(void);
/** Back to the menu after a game (the engine already left the server). */
void multi_game_over(void);

/** Whether this console created the lobby of the current game. */
bool multi_is_host(void);

/**
 * Tests: create a lobby on `server` (and start it once `start_players` have
 * joined), or join `code` there, without the menu.
 */
void multi_auto(const char *server, const char *code, int start_players);
