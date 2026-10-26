#include "input.h"

#include <math.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
static PadState pad;
#else
static SDL_GameController *controller;
#endif

#define MAX_FINGERS 4
#define DRAG_START 10.0f  // pixels before a press becomes a drag
#define LONG_PRESS_MS 450

typedef struct {
    SDL_FingerID id;
    bool active;
    float x, y;        // pixels
    float sx, sy;      // where it went down
    Uint32 t0;
    bool dragged;
    bool long_pressed;
} Finger;

static Finger fingers[MAX_FINGERS];
static float pinch_dist;
static bool pinching;
#ifndef __SWITCH__
static bool mouse_down;
static float mouse_sx, mouse_sy;
static bool mouse_dragged;
#endif

void input_init(void) {
#ifdef __SWITCH__
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
#else
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            break;
        }
    }
#endif
}

void input_shutdown(void) {
#ifndef __SWITCH__
    if (controller) SDL_GameControllerClose(controller);
#endif
}

static float dead(float v) {
    if (fabsf(v) < 0.18f) return 0;
    return (v - copysignf(0.18f, v)) / 0.82f;
}

static Finger *finger(SDL_FingerID id, bool create) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (fingers[i].active && fingers[i].id == id) return &fingers[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].active) {
            memset(&fingers[i], 0, sizeof fingers[i]);
            fingers[i].id = id;
            fingers[i].active = true;
            return &fingers[i];
        }
    }
    return NULL;
}

static int active_fingers(Finger **a, Finger **b) {
    int n = 0;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].active) continue;
        if (n == 0) *a = &fingers[i];
        else if (n == 1) *b = &fingers[i];
        n++;
    }
    return n;
}

void input_poll(Input *in, int sw, int sh) {
    bool prev[BTN_COUNT];
    memcpy(prev, in->held, sizeof prev);
    bool keep_quit = in->quit, keep_touch = in->touch, keep_down = in->down, keep_drag = in->dragging;
    float px = in->px, py = in->py;
    memset(in, 0, sizeof *in);
    in->quit = keep_quit;
    in->touch = keep_touch;
    in->down = keep_down;
    in->dragging = keep_drag;
    in->px = px;
    in->py = py;
    in->key_digit = -1;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                in->quit = true;
                break;
            case SDL_FINGERDOWN: {
                Finger *f = finger(e.tfinger.fingerId, true);
                if (!f) break;
                f->x = f->sx = e.tfinger.x * (float)sw;
                f->y = f->sy = e.tfinger.y * (float)sh;
                f->t0 = SDL_GetTicks();
                in->touch = true;
                Finger *a = NULL, *b = NULL;
                int n = active_fingers(&a, &b);
                if (n == 1) {
                    in->px = f->x, in->py = f->y;
                    in->down = true;
                    in->pressed_primary = true;
                    in->dragging = false;
                    in->moved = true;
                } else if (n == 2) {
                    // A second finger turns the gesture into a pinch.
                    pinch_dist = hypotf(a->x - b->x, a->y - b->y);
                    pinching = true;
                    a->dragged = b->dragged = true;
                    in->dragging = true;
                }
                break;
            }
            case SDL_FINGERMOTION: {
                Finger *f = finger(e.tfinger.fingerId, false);
                if (!f) break;
                float nx = e.tfinger.x * (float)sw, ny = e.tfinger.y * (float)sh;
                Finger *a = NULL, *b = NULL;
                int n = active_fingers(&a, &b);
                if (n == 1 && !pinching) {
                    if (!f->dragged && hypotf(nx - f->sx, ny - f->sy) > DRAG_START) {
                        f->dragged = true;
                        in->dragging = true;
                    }
                    if (f->dragged) {
                        in->drag_dx += nx - f->x;
                        in->drag_dy += ny - f->y;
                    }
                    in->px = nx, in->py = ny;
                    in->moved = true;
                }
                f->x = nx;
                f->y = ny;
                if (n == 2) {
                    float d = hypotf(a->x - b->x, a->y - b->y);
                    if (pinch_dist > 0 && d > 0) in->zoom_steps += log2f(d / pinch_dist) * 4;
                    pinch_dist = d;
                    in->zoom_x = (a->x + b->x) / 2;
                    in->zoom_y = (a->y + b->y) / 2;
                }
                break;
            }
            case SDL_FINGERUP: {
                Finger *f = finger(e.tfinger.fingerId, false);
                if (!f) break;
                Finger *a = NULL, *b = NULL;
                int n = active_fingers(&a, &b);
                if (n == 1) {
                    if (!f->dragged && !f->long_pressed && !pinching) {
                        in->released_primary = true;
                        in->px = f->x, in->py = f->y;
                    }
                    in->down = false;
                    pinching = false;
                }
                f->active = false;
                pinch_dist = 0;
                break;
            }
#ifndef __SWITCH__
            case SDL_MOUSEMOTION:
                if (e.motion.which == SDL_TOUCH_MOUSEID) break;
                in->touch = false;
                in->moved = true;
                in->px = (float)e.motion.x;
                in->py = (float)e.motion.y;
                if (mouse_down) {
                    if (!mouse_dragged && hypotf(in->px - mouse_sx, in->py - mouse_sy) > 5) {
                        mouse_dragged = true;
                        in->dragging = true;
                    }
                    if (mouse_dragged) {
                        in->drag_dx += (float)e.motion.xrel;
                        in->drag_dy += (float)e.motion.yrel;
                    }
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;
                in->touch = false;
                in->px = (float)e.button.x;
                in->py = (float)e.button.y;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    mouse_down = true;
                    mouse_dragged = false;
                    mouse_sx = in->px, mouse_sy = in->py;
                    in->down = true;
                    in->pressed_primary = true;
                    in->dragging = false;
                }
                if (e.button.button == SDL_BUTTON_RIGHT) in->secondary = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (!mouse_dragged) in->released_primary = true;
                    mouse_down = false;
                    in->down = false;
                }
                break;
            case SDL_MOUSEWHEEL:
                in->zoom_steps += (float)e.wheel.y;
                in->wheel += (float)e.wheel.y;
                in->zoom_x = in->px;
                in->zoom_y = in->py;
                break;
            case SDL_KEYDOWN:
                if (!e.key.repeat && e.key.keysym.sym >= SDLK_0 && e.key.keysym.sym <= SDLK_9) {
                    in->key_digit = e.key.keysym.sym - SDLK_0;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!controller) controller = SDL_GameControllerOpen(e.cdevice.which);
                break;
#endif
            default:
                break;
        }
    }

    // Long press: the touch screen's right click (radial menu).
    for (int i = 0; i < MAX_FINGERS; i++) {
        Finger *f = &fingers[i];
        if (f->active && !f->dragged && !f->long_pressed && !pinching && SDL_GetTicks() - f->t0 >= LONG_PRESS_MS) {
            f->long_pressed = true;
            in->secondary = true;
            in->px = f->x, in->py = f->y;
            in->down = false;  // the press is spent
        }
    }

#ifdef __SWITCH__
    padUpdate(&pad);
    u64 b = padGetButtons(&pad);
    static const u64 map[BTN_COUNT] = {
        HidNpadButton_A,     HidNpadButton_B,     HidNpadButton_X,      HidNpadButton_Y,
        HidNpadButton_L,     HidNpadButton_R,     HidNpadButton_ZL,     HidNpadButton_ZR,
        HidNpadButton_Plus,  HidNpadButton_Minus, HidNpadButton_Up,     HidNpadButton_Down,
        HidNpadButton_Left,  HidNpadButton_Right, HidNpadButton_StickL, HidNpadButton_StickR,
    };
    for (int i = 0; i < BTN_COUNT; i++) in->held[i] = (b & map[i]) != 0;
    HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
    in->lx = dead((float)l.x / 32767.0f);
    in->ly = dead(-(float)l.y / 32767.0f);
    in->rx = dead((float)r.x / 32767.0f);
    in->ry = dead(-(float)r.y / 32767.0f);
#else
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    in->held[BTN_A] = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE];
    in->held[BTN_B] = k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE];
    in->held[BTN_X] = k[SDL_SCANCODE_X];
    in->held[BTN_Y] = k[SDL_SCANCODE_B];
    in->held[BTN_L] = k[SDL_SCANCODE_T];
    in->held[BTN_R] = k[SDL_SCANCODE_Y];
    in->held[BTN_ZL] = k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_MINUS];
    in->held[BTN_ZR] = k[SDL_SCANCODE_E] || k[SDL_SCANCODE_EQUALS];
    in->held[BTN_PLUS] = k[SDL_SCANCODE_P];
    in->held[BTN_MINUS] = k[SDL_SCANCODE_TAB];
    in->held[BTN_UP] = k[SDL_SCANCODE_UP];
    in->held[BTN_DOWN] = k[SDL_SCANCODE_DOWN];
    in->held[BTN_LEFT] = k[SDL_SCANCODE_LEFT];
    in->held[BTN_RIGHT] = k[SDL_SCANCODE_RIGHT];
    in->held[BTN_RSTICK] = k[SDL_SCANCODE_C];
    in->rx = (float)(k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]);
    in->ry = (float)(k[SDL_SCANCODE_S] - k[SDL_SCANCODE_W]);
    if (controller) {
        static const SDL_GameControllerButton map[BTN_COUNT] = {
            SDL_CONTROLLER_BUTTON_A,          SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_X,          SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_INVALID,    SDL_CONTROLLER_BUTTON_INVALID,
            SDL_CONTROLLER_BUTTON_START,      SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            SDL_CONTROLLER_BUTTON_LEFTSTICK,  SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        };
        for (int i = 0; i < BTN_COUNT; i++) {
            if (map[i] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(controller, map[i])) {
                in->held[i] = true;
            }
        }
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) in->held[BTN_ZL] = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) in->held[BTN_ZR] = true;
        float lx = dead(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f);
        float ly = dead(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f);
        float rx = dead(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f);
        float ry = dead(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f);
        if (lx || ly) in->lx = lx, in->ly = ly;
        if (rx || ry) in->rx = rx, in->ry = ry;
    }
#endif
    for (int i = 0; i < BTN_COUNT; i++) in->pressed[i] = in->held[i] && !prev[i];
}
