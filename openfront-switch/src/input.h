// Controller, touch, keyboard and mouse folded into one per-frame state.
#pragma once

#include <SDL.h>
#include <stdbool.h>

enum {
    BTN_A,
    BTN_B,
    BTN_X,
    BTN_Y,
    BTN_L,
    BTN_R,
    BTN_ZL,
    BTN_ZR,
    BTN_PLUS,
    BTN_MINUS,
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_LSTICK,
    BTN_RSTICK,
    BTN_COUNT,
};

typedef struct {
    bool held[BTN_COUNT];
    bool pressed[BTN_COUNT];  // went down this frame
    float lx, ly, rx, ry;     // sticks, -1..1, deadzone applied

    // Pointer (mouse on PC, finger on the Switch touchscreen).
    bool pointer_moved;       // the mouse moved: the cursor follows it
    float px, py;
    bool tap;                 // a short touch / left click at (px, py)
    bool secondary_tap;       // right click
    float pan_dx, pan_dy;     // one-finger drag / mouse drag, in pixels
    float zoom_steps;         // wheel / pinch, >0 zooms in
    float pinch_x, pinch_y;   // where to zoom around

    bool quit;                // window closed / HOME exit
} Input;

void input_init(void);
void input_shutdown(void);
/** Polls events; `sw`/`sh` convert normalised touch coordinates to pixels. */
void input_poll(Input *in, int sw, int sh);
