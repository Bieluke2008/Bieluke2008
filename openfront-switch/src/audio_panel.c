#include "audio_panel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "gfx.h"
#include "settings.h"
#include "sound.h"
#include "ui.h"

#define CARD_BG 0x1d293d

static const char *LABELS[VOL_COUNT] = {"user_setting.audio_master", "user_setting.audio_music",
                                        "user_setting.audio_effects", "user_setting.audio_alerts",
                                        "user_setting.audio_ambience", "user_setting.audio_interface"};
static const char *DESCS[VOL_COUNT] = {"user_setting.audio_master_desc", "user_setting.audio_music_desc",
                                       "user_setting.audio_effects_desc", "user_setting.audio_alerts_desc",
                                       "user_setting.audio_ambience_desc", "user_setting.audio_interface_desc"};
// AudioMixer.PREVIEW_CUE: the Test button's cue, NULL where there is none.
static const char *PREVIEW[VOL_COUNT] = {NULL, NULL, "build-city", "nuke-warning", NULL, "click"};
static const float DEFAULTS[VOL_COUNT] = {0.9f, 0.5f, 0.7f, 0.8f, 0.4f, 0.5f};

/** SettingSlider: label and description left, value and range right. */
static void volume_row(int c, Rect r) {
    gfx_box(r, 12, rgba(255, 255, 255, 13), 1, rgba(255, 255, 255, 26));
    gfx_text(FONT_700, 16, T(LABELS[c]), r.x + 16, r.y + 10, 22, hex(TW_WHITE), ALIGN_LEFT);
    const char *desc = T(DESCS[c]);
    if (c == VOL_MUSIC && !sound_has_music()) desc = T("port.music_missing");
    // Up to two lines, broken at a space.
    float maxw = r.w - 260;
    char line[200];
    snprintf(line, sizeof line, "%s", desc);
    char *rest = NULL;
    if (gfx_text_width(FONT_400, 13, line) > maxw) {
        for (char *sp = strrchr(line, ' '); sp; sp = strrchr(line, ' ')) {
            *sp = 0;
            if (gfx_text_width(FONT_400, 13, line) <= maxw) {
                rest = (char *)desc + (sp - line) + 1;
                break;
            }
        }
    }
    SDL_Color dc = rgba(255, 255, 255, 128);
    gfx_text(FONT_400, 13, line, r.x + 16, r.y + 34, 18, dc, ALIGN_LEFT);
    if (rest) {
        char second[200];
        gfx_ellipsize(FONT_400, 13, rest, maxw, second, sizeof second);
        gfx_text(FONT_400, 13, second, r.x + 16, r.y + 52, 18, dc, ALIGN_LEFT);
    }

    Rect track = {r.x + r.w - 196, r.y + 22, 164, 8};
    uint32_t id = ui_id("audio-slider", c);
    static int active = -1;
    Rect hit = {track.x - 12, track.y - 16, track.w + 24, 40};
    bool hover = ui_hover(id, hit);
    const Pointer *p = ui_pointer();
    if (hover && p->pressed) active = c;
    float v = sound_volume((VolumeChannel)c);
    if (active == c) {
        float nv = roundf(fmaxf(0, fminf(1, (p->x - track.x) / track.w)) * 100) / 100;
        if (nv != v) sound_set_volume((VolumeChannel)c, v = nv);
        if (!p->down) {
            active = -1;
            sound_play("slider");
        }
    }
    gfx_fill_rrect(track, 4, rgba(255, 255, 255, 26));
    gfx_fill_rrect((Rect){track.x, track.y, track.w * v, track.h}, 4, hex(MALIBU_BLUE));
    gfx_fill_circle(track.x + track.w * v, track.y + track.h / 2, 9, hex(MALIBU_BLUE));
    char val[8];
    snprintf(val, sizeof val, "%d", (int)lroundf(v * 100));
    gfx_text(FONT_700, 14, val, track.x - 12, track.y - 6, 20, hex(TW_WHITE), ALIGN_RIGHT);

    if (PREVIEW[c]) {
        // Muted when master or this category is down, as on the web.
        bool muted = sound_volume(VOL_MASTER) <= 0 || v <= 0;
        Rect b = {r.x + r.w - 90, r.y + r.h - 34, 74, 26};
        uint32_t bid = ui_id("audio-test", c);
        bool bh = ui_hover(bid, b) && !muted;
        gfx_box(b, 8, rgba(255, 255, 255, bh ? 38 : 13), 1, rgba(255, 255, 255, 26));
        gfx_text(FONT_500, 13, T("user_setting.audio_test"), b.x + b.w / 2, b.y + 4, 18,
                 muted ? rgba(255, 255, 255, 100) : hex(TW_WHITE), ALIGN_CENTER);
        if (ui_button(bid, b) && !muted) sound_play(PREVIEW[c]);
    }
}

bool audio_panel_draw(void) {
    const float W = 640, ROW = 76, GAP = 10;
    float h = 61 + 16 + VOL_COUNT * (ROW + GAP) + 44 + 16;
    Rect card = {PAGE_W / 2.0f - W / 2, PAGE_H / 2.0f - h / 2, W, h};
    gfx_fill_rect((Rect){0, 0, PAGE_W, PAGE_H}, rgba(0, 0, 0, 153));
    uint32_t bg = ui_id("audio-bg", 0);
    bool closed = false;
    if (ui_button(bg, (Rect){0, 0, PAGE_W, PAGE_H})) closed = true;
    gfx_box(card, 8, hex(CARD_BG), 1, hex(TW_SLATE_600));
    ui_block(card);
    gfx_text(FONT_600, 20, T("user_setting.tab_audio"), card.x + 20, card.y + 17, 28, hex(TW_WHITE), ALIGN_LEFT);
    Rect x_btn = {card.x + W - 41, card.y + 10, 32, 40};
    uint32_t xid = ui_id("audio-x", 0);
    bool xh = ui_hover(xid, x_btn);
    gfx_text(FONT_700, 24, "×", x_btn.x + 16, x_btn.y + 8, 24, xh ? hex(TW_WHITE) : hex(TW_SLATE_400), ALIGN_CENTER);
    if (ui_button(xid, x_btn)) closed = true;
    gfx_fill_rect((Rect){card.x + 1, card.y + 61, W - 2, 1}, hex(TW_SLATE_600));
    float y = card.y + 61 + 16;
    for (int c = 0; c < VOL_COUNT; c++) {
        volume_row(c, (Rect){card.x + 16, y, W - 32, ROW});
        y += ROW + GAP;
    }
    char reset[96];
    snprintf(reset, sizeof reset, "%s", T("user_setting.audio_reset"));
    float rw = gfx_text_width(FONT_500, 13, reset) + 24;
    Rect rb = {card.x + W - 16 - rw, y + 6, rw, 28};
    uint32_t rid = ui_id("audio-reset", 0);
    bool rh = ui_hover(rid, rb);
    gfx_box(rb, 8, rgba(255, 255, 255, rh ? 38 : 13), 1, rgba(255, 255, 255, 26));
    gfx_text(FONT_500, 13, reset, rb.x + rw / 2, rb.y + 5, 18, hex(TW_WHITE), ALIGN_CENTER);
    if (ui_button(rid, rb)) {
        for (int c = 0; c < VOL_COUNT; c++) sound_set_volume((VolumeChannel)c, DEFAULTS[c]);
    }
    if (closed) settings_save();
    return closed;
}
