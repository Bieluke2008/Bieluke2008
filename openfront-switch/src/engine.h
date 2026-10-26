// The engine thread: owns the QuickJS runtime running OpenFront's engine
// (romfs:/engine.js) and steps the game. The render thread talks to it only
// through the command queue and the snapshot below.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_PLAYERS 4096
#define MAX_LEADERS 5
#define MAX_MENU 12
#define NAME_LEN 48

typedef struct {
    int map;          // index into the engine's map list
    int difficulty;   // 0 easy .. 3 impossible
    int bots;         // tribes
    int nations;      // -1 = map default, 0 = none
    char name[NAME_LEN];
} StartOptions;

typedef enum {
    CMD_START,
    CMD_PRIMARY,        // a=x b=y
    CMD_BOAT,           // a=x b=y
    CMD_BUILD,          // a=kind b=x c=y
    CMD_BUILD_MENU,     // a=x b=y -> menu result
    CMD_PLAYER_MENU,    // a=x b=y -> menu result
    CMD_PLAYER_ACTION,  // a=smallID b=action
    CMD_ALLIANCES,      // a=accept
    CMD_RATIO,          // f=ratio
    CMD_CANCEL_ATTACK,
    CMD_STOP,           // leave the game
} CmdType;

typedef struct {
    CmdType type;
    int a, b, c;
    double f;
    StartOptions start;
} Cmd;

typedef struct {
    int id;
    float x, y, size;
    double troops;
} Label;

typedef struct {
    int kind, x, y, owner, level;
} UnitRec;

typedef struct {
    int id;
    char name[NAME_LEN];
    double pct;
} Leader;

typedef struct {
    char text[160];
    int kind;  // 0 info, 1 good, 2 bad, 3 alliance
} Msg;

typedef struct {
    int kind;
    char name[NAME_LEN];
    double cost;
    bool enabled;
} MenuItem;

typedef enum { MENU_NONE, MENU_BUILD, MENU_PLAYER } MenuKind;

typedef struct {
    MenuKind kind;
    int seq;  // matches the request that produced it
    int x, y;
    int count;
    MenuItem items[MAX_MENU];
    // Player menu
    int player;
    char player_name[NAME_LEN];
    double player_troops;
    bool ally, can_ally, can_break, can_target, can_embargo, can_donate;
} MenuResult;

typedef enum {
    GAME_IDLE,
    GAME_LOADING,
    GAME_RUNNING,
    GAME_FAILED,
} GameStatus;

typedef struct {
    GameStatus status;
    char error[256];
    int width, height;
    const uint8_t *terrain;  // engine-owned, width*height
    const uint16_t *state;   // engine-owned, width*height; bits 0-11 owner
    const uint8_t *colors;   // engine-owned RGBA per small id

    // HUD, refreshed every tick.
    int tick;
    double seconds;
    bool spawn_phase, spawned, alive;
    int me;
    double troops, max_troops, gold, pct, ratio;
    int tiles, attacks;
    char winner[128];
    int nleaders;
    Leader leaders[MAX_LEADERS];
    double tick_ms;  // engine time of the last tick
    int pending_alliances;

    int nlabels;
    Label labels[MAX_PLAYERS];
    int nunits;
    UnitRec *units;
    int units_cap;
} Snapshot;

typedef struct {
    int nmaps;
    char (*map_names)[NAME_LEN];
    char (*map_folders)[NAME_LEN];
} MapList;

/**
 * Starts the engine thread and loads engine.js; false if that failed.
 * `keys` (NULL-terminated) are the strings engine_t can translate.
 */
bool engine_init(const char *romfs, const char *lang, const char *const *keys);
void engine_shutdown(void);
void engine_send(const Cmd *cmd);

/** Speed multiplier for ticks (1 = the web client's 10 ticks/s). */
void engine_set_speed(double speed);
void engine_set_paused(bool paused);

/** Locks the snapshot for reading. */
const Snapshot *engine_lock(void);
void engine_unlock(void);

/** Names indexed by small id; safe to read while the snapshot is locked. */
const char *engine_player_name(int id);

/** Moves the tile refs changed since the last call into *out (caller frees). */
int engine_take_dirty(uint32_t **out, int *full_redraw);

/** Pops one log message; false when none. */
bool engine_pop_message(Msg *out);

/** Copies the latest menu result; returns its seq (0 if none yet). */
int engine_menu(MenuResult *out);
int engine_next_seq(void);

const MapList *engine_maps(void);
/** A translated string from OpenFront's language files (or the port's). */
const char *engine_t(const char *key);
