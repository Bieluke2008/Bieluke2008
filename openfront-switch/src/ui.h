// Immediate-mode UI over gfx: widgets register their hit rectangles while
// drawing, in z-order, and the topmost one under the pointer gets the hover
// and the click on the next frame, like the DOM does for the web HUD. The
// pointer is the mouse, the last touch, or the controller's cursor.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

// Tailwind v4 colours used by the web client's HUD (sRGB of the oklch values).
#define TW_WHITE 0xffffff
#define TW_GRAY_200 0xe5e7eb
#define TW_GRAY_300 0xd1d5dc
#define TW_GRAY_400 0x99a1af
#define TW_GRAY_500 0x6a7282
#define TW_GRAY_600 0x4a5565
#define TW_GRAY_700 0x364153
#define TW_GRAY_800 0x1e2939
#define TW_GRAY_900 0x101828
#define TW_SLATE_300 0xcad5e2
#define TW_SLATE_400 0x90a1b9
#define TW_SLATE_500 0x62748e
#define TW_SLATE_600 0x45556c
#define TW_ZINC_100 0xf4f4f5
#define TW_ZINC_200 0xe4e4e7
#define TW_ZINC_300 0xd4d4d8
#define TW_ZINC_400 0x9f9fa9
#define TW_ZINC_700 0x3f3f46
#define TW_ZINC_800 0x27272a
#define TW_ZINC_900 0x18181b
#define TW_GREEN_400 0x05df72
#define TW_GREEN_500 0x00c950
#define TW_GREEN_600 0x00a63e
#define TW_EMERALD_400 0x00d492
#define TW_ORANGE_400 0xff8904
#define TW_YELLOW_300 0xffdf20
#define TW_YELLOW_400 0xfdc700
#define TW_AMBER_400 0xffb900
#define TW_RED_200 0xffc9c9
#define TW_RED_300 0xffa2a2
#define TW_RED_400 0xff6467
#define TW_RED_500 0xfb2c36
#define TW_RED_700 0xc10007
#define TW_RED_900 0x82181a
#define TW_BLUE_400 0x51a2ff
#define TW_BLUE_500 0x2b7fff
#define TW_BLUE_600 0x155dfc
#define TW_SKY_300 0x74d4ff
#define TW_SKY_700 0x0069a8
#define TW_CYAN_300 0x53eafd
#define TW_INDIGO_400 0x7c86ff
#define TW_INDIGO_300 0xa3b3ff
#define MALIBU_BLUE 0x0084d1
#define AQUARIUS 0x3fa9f5

typedef struct {
    float x, y;            // page coordinates
    bool down;             // primary button held
    bool pressed;          // primary went down this frame
    bool released;         // primary went up this frame
    bool secondary;        // right click / long press / X this frame
    float wheel;           // scroll steps (UI lists)
    bool touch;            // the last pointer input was a touch
} Pointer;

void ui_begin(const Pointer *p);
void ui_end(void);
const Pointer *ui_pointer(void);

/** A stable id for a widget, from a name and an index. */
uint32_t ui_id(const char *name, int index);

/** Registers a hit rectangle; true while it is the topmost under the pointer. */
bool ui_hover(uint32_t id, Rect r);
/** ui_hover() plus: true on the frame the primary button is released on it. */
bool ui_button(uint32_t id, Rect r);
/** Blocks the map under a panel (no hover state needed). */
void ui_block(Rect r);
/** True when the pointer's last click landed on no widget (the map gets it). */
bool ui_click_on_map(void);
bool ui_secondary_on_map(void);
/** Something is under the pointer this frame (from last frame's regions). */
bool ui_pointer_over_ui(void);
/** Consumes the click so widgets drawn later see none. */
void ui_consume_click(void);
