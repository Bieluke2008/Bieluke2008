// The engine thread: owns the QuickJS runtime running OpenFront's engine and
// the adapter (romfs/engine.js) and steps the game. The render thread talks
// to it only through the functions below.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PLAYERS 4096
#define NAME_LEN 64

/** The Solo modal's game config (SinglePlayerModal.startGame). */
typedef struct {
    int map;          // index into the engine's map list
    int difficulty;   // 0 easy .. 3 impossible
    int bots;         // tribes
    int nations;      // -1 = map default, 0 = none
    bool instant_build, infinite_gold, infinite_troops, random_spawn, water_nukes;
    int game_mode;    // 0 free for all, 1 teams
    int team_count;   // index into the team count options (2..7, quads, trios, duos, humans vs nations)
    bool doomsday;
    int doomsday_speed;  // 0 slow, 1 normal, 2 fast, 3 very fast
    // The toggle-input cards: on/off and their value.
    bool max_timer_on, gold_mult_on, starting_gold_on, alliance_on, overtime_on;
    double max_timer, gold_mult, starting_gold, alliance_minutes, overtime;
    unsigned disabled_units;  // bit i: the Solo modal's unit option i is off
    char name[NAME_LEN];
} StartOptions;

typedef enum {
    GAME_IDLE,
    GAME_LOADING,
    GAME_RUNNING,
    GAME_FAILED,
} GameStatus;

/** A name label (NameViewData plus the name pass's status row). */
typedef struct {
    int id;
    float x, y, size;
    double troops;
    int icons;  // ICON_* bits
    int emoji;  // index+1 into the emoji table, 0 = none
} Label;

enum {
    ICON_CROWN = 1,
    ICON_TRAITOR = 2,
    ICON_DISCONNECTED = 4,
    ICON_ALLIANCE = 8,
    ICON_ALLIANCE_REQUEST = 16,
    ICON_TARGET = 32,
    ICON_EMBARGO = 64,
    ICON_NUKE_WHITE = 128,
    ICON_NUKE_RED = 256,
};

typedef struct {
    int kind, x, y, owner, level, flags, id, tx, ty;
    int progress;  // structure bar (BarPass) in per mille, -1 for none
} UnitRec;

enum {
    UNIT_UNDER_CONSTRUCTION = 1,
    UNIT_MARKED_FOR_DELETION = 2,
    UNIT_RETREATING = 4,
    UNIT_TRADE_FRIENDLY = 8,
    UNIT_SILO_COOLDOWN = 32,
    UNIT_TRADE_SELF = 64,
};

/** An attack's troop label (AttackingTroopsController): up to two front positions. */
typedef struct {
    int key;  // stable per attack while it lasts
    bool incoming;
    double troops;
    int npos;
    float x[2], y[2];  // tiles
} AttackLabel;

#define MAX_ATTACK_LABELS 64

typedef struct {
    char name[NAME_LEN];
    char flag[16];
    int type;  // 0 human, 1 nation, 2 bot
    bool known;
} PlayerInfo;

typedef struct {
    GameStatus status;
    char error[256];
    int width, height;
    const uint8_t *terrain;  // engine-owned, width*height
    const uint16_t *state;   // engine-owned, width*height; bits 0-11 owner
    const uint8_t *colors;   // engine-owned, 8 bytes per small id (colors.ts)
    int tick;
    double tick_ms;          // engine time of the last tick
    int nlabels;
    Label *labels;
    int nunits;
    UnitRec *units;
    int nattacks;
    AttackLabel attacks[MAX_ATTACK_LABELS];
    char sounds[1024];       // queued cue names, '\n'-separated; the renderer drains it
    char *frame;             // the adapter's HUD model (JSON), may be NULL
    int frame_version;
    uint8_t *relations;      // MAX_REL x MAX_REL: 1 friendly, 2 embargo
    int relations_version;
} Snapshot;

#define MAX_REL 1024

typedef struct {
    int nmaps;
    char (*map_names)[NAME_LEN];
    char (*map_folders)[NAME_LEN];
    char (*map_sections)[NAME_LEN];  // picker section heading map i opens, "" if none
    char (*map_groups)[NAME_LEN];    // group heading within it, "" if none
    int *map_nations;                // the map's own nation count
} MapList;

/**
 * Starts the engine thread and loads engine.js; false if that failed.
 * `keys` (NULL-terminated) are the strings engine_t can translate.
 */
bool engine_init(const char *romfs, const char *lang, const char *const *keys);
void engine_shutdown(void);

void engine_start(const StartOptions *opts);
void engine_stop(void);
/** Speed multiplier for ticks (1 = the web client's 10 ticks/s). */
void engine_set_speed(double speed);
void engine_set_paused(bool paused);

/**
 * Calls OF.call(method, args_json) on the engine thread. Returns a request
 * id for engine_result(), or 0 when the result is not wanted.
 */
int engine_call(const char *method, const char *args_json, bool want_result);
/** The JSON result of a call (caller frees), or NULL while it is pending. */
char *engine_result(int id);

/** Locks the snapshot for reading. */
const Snapshot *engine_lock(void);
/** Moves the queued sound cue names ('\n'-separated) into `out` and clears the queue. */
void engine_take_sounds(char *out, size_t n);
void engine_unlock(void);

/** Static player info by small id; safe to read while the snapshot is locked. */
const PlayerInfo *engine_player(int id);

/** Moves the tile refs changed since the last call into *out (caller frees). */
int engine_take_dirty(uint32_t **out, int *full_redraw);

const MapList *engine_maps(void);
/** A translated string from OpenFront's language files (or the port's). */
const char *engine_t(const char *key);
