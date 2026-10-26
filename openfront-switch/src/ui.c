#include "ui.h"

#include <string.h>

#define MAX_REGIONS 1024

typedef struct {
    uint32_t id;
    Rect r;
} Region;

static Pointer ptr;
static Region regions[MAX_REGIONS], prev[MAX_REGIONS];
static int nregions, nprev;
static uint32_t hot;          // topmost id under the pointer, from last frame
static uint32_t pressed_on;   // id the primary press started on
static bool over_ui;          // any region under the pointer, last frame
static bool click_consumed;

/** The topmost of last frame's regions under the pointer's current spot. */
static void hit_test(void) {
    hot = 0;
    over_ui = false;
    for (int i = nprev - 1; i >= 0; i--) {
        if (in_rect(prev[i].r, ptr.x, ptr.y)) {
            hot = prev[i].id;
            over_ui = true;
            break;
        }
    }
}

void ui_begin(const Pointer *p) {
    ptr = *p;
    nregions = 0;
    click_consumed = false;
    // Hit-test at the pointer's new position (a tap jumps there and clicks
    // in the same frame).
    hit_test();
    if (ptr.pressed) pressed_on = hot;
}

void ui_end(void) {
    memcpy(prev, regions, sizeof(Region) * (size_t)nregions);
    nprev = nregions;
    if (ptr.released) pressed_on = 0;
}

const Pointer *ui_pointer(void) { return &ptr; }

uint32_t ui_id(const char *name, int index) {
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ (uint32_t)index) * 16777619u;
    return h ? h : 1;
}

static void add(uint32_t id, Rect r) {
    if (nregions < MAX_REGIONS) regions[nregions++] = (Region){id, r};
}

bool ui_hover(uint32_t id, Rect r) {
    add(id, r);
    return hot == id && in_rect(r, ptr.x, ptr.y);
}

bool ui_button(uint32_t id, Rect r) {
    bool h = ui_hover(id, r);
    bool clicked = h && ptr.released && pressed_on == id && !click_consumed;
    if (clicked) click_consumed = true;
    return clicked;
}

void ui_block(Rect r) { add(ui_id("block", nregions), r); }

bool ui_click_on_map(void) { return ptr.released && !over_ui && pressed_on == 0 && !click_consumed; }

bool ui_secondary_on_map(void) { return ptr.secondary && !over_ui; }

bool ui_pointer_over_ui(void) { return over_ui; }

void ui_consume_click(void) { click_consumed = true; }
