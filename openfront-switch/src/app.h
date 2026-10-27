// State shared by the in-game screens (HUD, menus, map) and the actions they
// ask of the game screen.
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "engine.h"
#include "gfx.h"
#include "map.h"

typedef enum {
    MODAL_NONE,
    MODAL_PANEL,     // PlayerPanel
    MODAL_SEND,      // SendResourceModal (gold / troops)
    MODAL_EMOJI,     // EmojiTable
    MODAL_SETTINGS,  // SettingsModal
    MODAL_EXIT,      // exit confirmation
    MODAL_CHAT,      // quick chat
    MODAL_AUDIO,     // the settings' Audio tab
} Modal;

typedef struct {
    Camera cam;
    bool cam_ready;
    float cursor_x, cursor_y;  // page coordinates of the virtual mouse
    bool cursor_visible;       // a controller is driving the cursor
    const cJSON *frame;        // the adapter's HUD model for this frame
    int my_sid;
    bool spawn_phase;
    bool paused;
    int speed_idx;
    bool leaderboard_shown;
    bool leaderboard_auto;     // show it once the spawn phase ends
    bool replay_panel;         // the speed panel under the sidebar
    char ghost[32];            // UnitType being placed from the hotbar, "" none
    Modal modal;
    int modal_player;          // small id for PlayerPanel / send / emoji
    bool send_troops;          // MODAL_SEND: troops (else gold)
    int hover_tile_x, hover_tile_y;
    double last_click_ms;
    int warships[64];          // selected warships
    int nwarships;
    bool quit_to_menu;
    bool alt_view;             // "Toggle Terrain" in the in-game menu
    bool help_visible;         // the controls help overlay
} App;

extern App app;

/** Moves the camera to a player's name (GoToPlayerEvent). */
void app_focus_player(int sid);
void app_focus_unit(int id);
void app_focus_tile(float x, float y);
void app_set_paused(bool paused);
void app_set_speed(int idx);
void app_set_ghost(const char *type);
/** Opens the radial menu for the tile under a page point. */
void app_open_radial(float px, float py);
void app_open_modal(Modal m, int player);
double app_now_ms(void);
const char *T(const char *key);
/** Shows / hides the controls help (the in-game menu's "Settings"). */
void app_toggle_controls_help(void);
