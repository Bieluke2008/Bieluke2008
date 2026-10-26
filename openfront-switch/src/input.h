// Controller, touch screen, keyboard and mouse folded into one per-frame
// state: buttons and sticks, plus a pointer that behaves like the web
// client's mouse (left click, right click / long press, drag, wheel/pinch).
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

    // The pointer (mouse, or the finger on the touch screen), output pixels.
    float px, py;
    bool moved;               // the mouse / finger moved this frame
    bool down;                // primary held
    bool pressed_primary;     // primary went down this frame
    bool released_primary;    // primary went up this frame, not after a drag
    bool secondary;           // right click / long press this frame
    bool dragging;            // the current press has become a drag
    bool touch;               // the last pointer input was a finger
    float drag_dx, drag_dy;   // drag movement this frame (pans the map)
    float zoom_steps;         // wheel / pinch, >0 zooms in
    float zoom_x, zoom_y;     // where to zoom around
    float wheel;              // raw wheel steps (lists)

    // Keyboard shortcuts on PC builds (digits for the hotbar...).
    int key_digit;            // 0-9 pressed this frame, -1 none
    bool quit;
} Input;

void input_init(void);
void input_shutdown(void);
/** Polls events; sw/sh convert normalised touch coordinates to pixels. */
void input_poll(Input *in, int sw, int sh);
