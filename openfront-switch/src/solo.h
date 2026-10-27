// The solo game setup, styled after the web client's Solo modal.
#pragma once

#include "engine.h"
#include "input.h"

typedef enum { SOLO_NONE, SOLO_START, SOLO_QUIT } SoloResult;

void solo_init(const char *romfs);
void solo_shutdown(void);
/** A multiplayer lobby the host sets up on this screen (HostLobbyModal). */
typedef struct {
    const char *code;  // the lobby id the others type to join
    const char *hint;  // how the others join (under the code)
    const char *players[64];
    int nplayers;      // the host first
    bool starting;     // the start countdown is running
} SoloLobby;

/**
 * Draws the screen and edits `o`; between ui_begin() and ui_end(). With a
 * `lobby` it is the host's lobby screen: its code and players on top and no
 * game speed (the server sets the pace).
 */
SoloResult solo_draw(StartOptions *o, int *speed_idx, const Input *in, const SoloLobby *lobby);
