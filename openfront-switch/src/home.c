#include "home.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "app.h"
#include "gfx.h"
#include "settings.h"
#include "sound.h"
#include "ui.h"

// The Solo screen's palette (the web client's dark purple and orange theme).
#define PAGE_BG 0x0a060f
#define HEADER_BG 0x13051d
#define CARD_BG 0x13061e
#define CARD_BORDER 0x2b1e34
#define ITEM_BG 0x201229
#define ITEM_BORDER 0x33263d
#define ORANGE 0xe06c12
#define START_BG 0xb84700
#define LABEL 0xa99aa8

static SDL_Texture *background;
static int bg_w, bg_h;

void home_init(const char *romfs) {
    char path[512];
    snprintf(path, sizeof path, "%sui/background.jpg", romfs);
    background = gfx_load_texture(path, &bg_w, &bg_h);
}

void home_shutdown(void) {
    if (background) SDL_DestroyTexture(background);
    background = NULL;
}

void home_backdrop(void) {
    SDL_Renderer *ren = gfx_renderer();
    SDL_SetRenderDrawColor(ren, 0x0a, 0x06, 0x0f, 255);
    SDL_RenderClear(ren);
    if (background) {
        // bg-cover, top-centred, at the page's 30% opacity.
        float s = fmaxf((float)PAGE_W / (float)bg_w, (float)PAGE_H / (float)bg_h);
        float w = (float)bg_w * s, h = (float)bg_h * s;
        gfx_image(background, NULL, (Rect){(PAGE_W - w) / 2, 0, w, h}, rgba(255, 255, 255, 77));
    }
    // Header and footer.
    gfx_fill_rect((Rect){0, 0, PAGE_W, 88}, alpha(hex(HEADER_BG), 0.92f));
    gfx_text(FONT_800, 22, ">OPENFRONT<", 208, 22, 26, hex(0xff4040), ALIGN_CENTER);
    gfx_text(FONT_800, 12, "NINTENDO SWITCH", 208, 50, 16, hex(0xff8a1f), ALIGN_CENTER);
    gfx_text(FONT_600, 16, T("port.title"), 1140, 34, 22, hex(0xb0a6b8), ALIGN_RIGHT);
    gfx_fill_rect((Rect){0, 640, PAGE_W, 80}, alpha(hex(CARD_BG), 0.92f));
    gfx_text(FONT_400, 12, "v" OPENFRONT_VERSION, PAGE_W / 2.0f, 660, 18, hex(0x8f8696), ALIGN_CENTER);
    gfx_text(FONT_400, 12, T("port.credits"), PAGE_W / 2.0f, 684, 18, hex(0x8f8696), ALIGN_CENTER);
}

static void edit_name(StartOptions *o) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    char out[NAME_LEN] = {0};
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, o->name);
        swkbdConfigSetStringLenMax(&kbd, 20);
        if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out)) && out[0]) {
            snprintf(o->name, sizeof o->name, "%s", out);
            settings_set_str("name", o->name);
            settings_save();
        }
        swkbdClose(&kbd);
    }
#else
    (void)o;
#endif
}

bool home_button(const char *id, Rect r, const char *label, bool primary, float px) {
    uint32_t uid = ui_id(id, 0);
    bool hover = ui_hover(uid, r);
    Rect d = r;
    if (hover) d = (Rect){r.x - r.w * 0.01f, r.y - r.h * 0.04f, r.w * 1.02f, r.h * 1.08f};
    if (primary) gfx_fill_rrect(d, 10, hex(hover ? 0xcc5200 : START_BG));
    else gfx_box(d, 10, hex(hover ? 0x2a1834 : ITEM_BG), 1, hex(ITEM_BORDER));
    char t[64];
    snprintf(t, sizeof t, "%s", label);
    gfx_upper(t);
    gfx_text(primary ? FONT_700 : FONT_600, px, t, r.x + r.w / 2, r.y + r.h / 2 - px * 0.7f, px * 1.4f, hex(TW_WHITE),
             ALIGN_CENTER);
    return ui_button(uid, r);
}

HomeResult home_draw(StartOptions *o, const Input *in) {
    HomeResult result = HOME_NONE;
    home_backdrop();

    // Title.
    gfx_text(FONT_800, 76, "OPENFRONT", PAGE_W / 2.0f, 128, 90, hex(TW_WHITE), ALIGN_CENTER);
    gfx_text(FONT_600, 18, T("port.title"), PAGE_W / 2.0f, 222, 24, hex(0xd8d0dc), ALIGN_CENTER);

    const float W = 560, X = PAGE_W / 2.0f - W / 2;
    Rect card = {X - 24, 270, W + 48, 340};
    gfx_box(card, 14, alpha(hex(CARD_BG), 0.94f), 1, hex(CARD_BORDER));

    // UsernameInput.
    char l[64];
    snprintf(l, sizeof l, "%s", T("port.name"));
    gfx_upper(l);
    gfx_text(FONT_700, 13, l, X, 292, 18, hex(LABEL), ALIGN_LEFT);
    Rect nr = {X, 316, W, 52};
    uint32_t nid = ui_id("home-name", 0);
    bool nh = ui_hover(nid, nr);
    gfx_box(nr, 10, hex(nh ? 0x2a1834 : ITEM_BG), 1, nh ? alpha(hex(ORANGE), 0.6f) : hex(ITEM_BORDER));
    gfx_text(FONT_600, 20, o->name, nr.x + 18, nr.y + 13, 26, hex(TW_WHITE), ALIGN_LEFT);
    if (ui_button(nid, nr) || in->pressed[BTN_Y]) edit_name(o);

    // Solo (the web's primary action), then the port's own.
    float hw = (W - 16) / 2;
    if (home_button("home-solo", (Rect){X, 388, hw, 84}, T("main.solo"), true, 26) || in->pressed[BTN_PLUS])
        result = HOME_SOLO;
    if (home_button("home-multi", (Rect){X + hw + 16, 388, hw, 84}, T("port.multiplayer_short"), true, 22) ||
        in->pressed[BTN_X])
        result = HOME_MULTI;
    float bw = (W - 32) / 3;
    if (home_button("home-audio", (Rect){X, 488, bw, 56}, T("user_setting.tab_audio"), false, 16)) result = HOME_AUDIO;
    if (home_button("home-controls", (Rect){X + bw + 16, 488, bw, 56}, T("port.controls_title"), false, 16))
        result = HOME_CONTROLS;
    if (home_button("home-quit", (Rect){X + 2 * (bw + 16), 488, bw, 56}, T("port.quit"), false, 16)) result = HOME_QUIT;

    // Controller hints.
    gfx_text(FONT_400, 13, T("port.home_hint"), PAGE_W / 2.0f, 566, 18, hex(0x8f8696), ALIGN_CENTER);
    return result;
}
