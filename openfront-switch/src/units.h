// How each unit kind is drawn. Indexes follow UNIT_KINDS in js/adapter.ts.
#pragma once

#include <SDL.h>
#include <stdbool.h>

typedef enum {
    SHAPE_SQUARE,
    SHAPE_CIRCLE,
    SHAPE_DIAMOND,
    SHAPE_TRIANGLE,
    SHAPE_SHIP,
    SHAPE_DOT,
} Shape;

typedef struct {
    Shape shape;
    float tiles;   // size in map tiles
    float min_px;  // clamped on screen
    float max_px;
    char letter;   // drawn inside when large enough
    bool fixed;    // use `color` instead of the owner's
    SDL_Color color;
} UnitStyle;

enum {
    UNIT_CITY,
    UNIT_PORT,
    UNIT_DEFENSE_POST,
    UNIT_MISSILE_SILO,
    UNIT_SAM_LAUNCHER,
    UNIT_FACTORY,
    UNIT_WARSHIP,
    UNIT_TRANSPORT,
    UNIT_TRADE_SHIP,
    UNIT_ATOM_BOMB,
    UNIT_HYDROGEN_BOMB,
    UNIT_MIRV,
    UNIT_MIRV_WARHEAD,
    UNIT_TRAIN,
    UNIT_SHELL,
    UNIT_SAM_MISSILE,
    UNIT_KIND_COUNT,
};

static const UnitStyle UNIT_STYLES[UNIT_KIND_COUNT] = {
    [UNIT_CITY] = {SHAPE_SQUARE, 5, 7, 30, 'C', false, {0}},
    [UNIT_PORT] = {SHAPE_SQUARE, 5, 7, 30, 'P', false, {0}},
    [UNIT_DEFENSE_POST] = {SHAPE_DIAMOND, 5, 7, 30, 'D', false, {0}},
    [UNIT_MISSILE_SILO] = {SHAPE_TRIANGLE, 6, 8, 34, 'S', false, {0}},
    [UNIT_SAM_LAUNCHER] = {SHAPE_CIRCLE, 5, 7, 30, 'A', false, {0}},
    [UNIT_FACTORY] = {SHAPE_SQUARE, 5, 7, 30, 'F', false, {0}},
    [UNIT_WARSHIP] = {SHAPE_SHIP, 5, 8, 26, 0, false, {0}},
    [UNIT_TRANSPORT] = {SHAPE_SHIP, 3, 6, 18, 0, false, {0}},
    [UNIT_TRADE_SHIP] = {SHAPE_SHIP, 3, 5, 16, 0, true, {240, 200, 60, 255}},
    [UNIT_ATOM_BOMB] = {SHAPE_DOT, 3, 6, 16, 0, true, {255, 60, 30, 255}},
    [UNIT_HYDROGEN_BOMB] = {SHAPE_DOT, 4, 8, 20, 0, true, {255, 0, 120, 255}},
    [UNIT_MIRV] = {SHAPE_DOT, 5, 9, 22, 0, true, {200, 0, 255, 255}},
    [UNIT_MIRV_WARHEAD] = {SHAPE_DOT, 2, 4, 10, 0, true, {255, 60, 30, 255}},
    [UNIT_TRAIN] = {SHAPE_DOT, 2, 3, 8, 0, false, {0}},
    [UNIT_SHELL] = {SHAPE_DOT, 1, 2, 5, 0, true, {255, 255, 200, 255}},
    [UNIT_SAM_MISSILE] = {SHAPE_DOT, 1, 3, 6, 0, true, {255, 255, 255, 255}},
};

static inline const UnitStyle *unit_style(int kind) {
    return kind >= 0 && kind < UNIT_KIND_COUNT ? &UNIT_STYLES[kind] : NULL;
}
