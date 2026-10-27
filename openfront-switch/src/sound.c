#include "sound.h"

#include <SDL.h>
#include <SDL_mixer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

// Sounds.ts: the cue files and the channel each plays on.
typedef enum { CH_EFFECTS, CH_ALERTS, CH_INTERFACE } CueChannel;

static const struct {
    const char *name, *file;
    CueChannel ch;
} CUES[] = {
    {"ka-ching", "ka-ching", CH_EFFECTS},
    {"conquered", "conquered", CH_EFFECTS},
    {"atom-hit", "atom-hit", CH_EFFECTS},
    {"atom-launch", "atom-launch", CH_EFFECTS},
    {"hydrogen-hit", "hydrogen-hit", CH_EFFECTS},
    {"hydrogen-launch", "hydrogen-launch", CH_EFFECTS},
    {"mirv-launch", "mirv-launch", CH_EFFECTS},
    {"alliance-suggested", "alliance-suggested", CH_ALERTS},
    {"alliance-broken", "alliance-broken", CH_ALERTS},
    {"build-port", "build-port", CH_EFFECTS},
    {"build-city", "build-city", CH_EFFECTS},
    {"build-defense-post", "build-defense-post", CH_EFFECTS},
    {"build-warship", "build-warship", CH_EFFECTS},
    {"sam-built", "sam-built", CH_EFFECTS},
    {"silo-built", "silo-built", CH_EFFECTS},
    {"message", "alliance-suggested", CH_ALERTS},
    {"click", "click", CH_INTERFACE},
    {"click-1", "click-1", CH_INTERFACE},
    {"click-2", "click-2", CH_INTERFACE},
    {"click-3", "click-3", CH_INTERFACE},
    {"slider", "slider", CH_INTERFACE},
    {"alliance-accepted", "alliance-accepted", CH_ALERTS},
    {"alliance-declined", "alliance-declined", CH_ALERTS},
    {"build-factory", "build-factory", CH_EFFECTS},
    {"build-train-station", "build-train-station", CH_EFFECTS},
    {"transport-ship", "transport-ship", CH_EFFECTS},
    {"nuke-warning", "nuke-warning", CH_ALERTS},
    {"spawn", "spawn", CH_EFFECTS},
    {"game-start", "game-start", CH_EFFECTS},
    {"victory", "victory", CH_EFFECTS},
    {"defeat", "defeat", CH_EFFECTS},
};
#define N_CUES (int)(sizeof CUES / sizeof CUES[0])

static const char *AMBIENCE[] = {"city", "factory", "missile-silo", "sam-silo"};
#define N_AMBIENCE 4

// UserSettings.AUDIO_DEFAULTS.
static const float DEFAULT_VOLUME[VOL_COUNT] = {0.9f, 0.5f, 0.7f, 0.8f, 0.4f, 0.5f};
static const char *VOLUME_KEYS[VOL_COUNT] = {"volume.master", "volume.music",    "volume.effects",
                                             "volume.alerts", "volume.ambience", "volume.interface"};
static float volume[VOL_COUNT];

static bool ready;
static Mix_Chunk *cue_chunks[N_CUES];
static Mix_Chunk *amb_chunks[N_AMBIENCE];

// Ambience: two reserved channels so a new loop fades in while the old one
// fades out (SoundManager's 500 ms crossfade).
#define AMB_FADE_MS 500
static int amb_track = -1, amb_channel = 0;
static float amb_gain;

// Music: the menu theme, or the three gameplay tracks in turn.
static const char *MUSIC_FILES[] = {"menu-theme", "gameplay", "gameplay-2", "gameplay-3"};
static Mix_Music *music[4];
static int music_playing = -1, next_gameplay = 1;
static volatile bool music_ended;

/** AudioMixer.perceptualGain: the slider position squared. */
static float perceptual(float v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return v * v;
}

static int mix_volume(float gain) {
    int v = (int)lroundf(gain * MIX_MAX_VOLUME);
    return v < 0 ? 0 : v > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : v;
}

static float channel_gain(CueChannel ch) {
    float slider = ch == CH_INTERFACE ? volume[VOL_INTERFACE] : ch == CH_ALERTS ? volume[VOL_ALERTS] : volume[VOL_EFFECTS];
    return perceptual(volume[VOL_MASTER]) * perceptual(slider);
}

static void apply_music_volume(void) {
    // CATEGORY_TRIM: music runs 1 dB down.
    Mix_VolumeMusic(mix_volume(perceptual(volume[VOL_MASTER]) * perceptual(volume[VOL_MUSIC]) * 0.89f));
}

static void apply_ambience_volume(void) {
    if (amb_track < 0) return;
    float g = perceptual(volume[VOL_MASTER]) * perceptual(volume[VOL_AMBIENCE]) * amb_gain;
    Mix_Volume(amb_channel, mix_volume(g));
}

static void on_music_finished(void) { music_ended = true; }

void sound_init(const char *romfs) {
    for (int i = 0; i < VOL_COUNT; i++) volume[i] = (float)settings_num(VOLUME_KEYS[i], DEFAULT_VOLUME[i]);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return;
    }
    Mix_Init(MIX_INIT_MP3);
    if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048) != 0) {
        fprintf(stderr, "Mix_OpenAudio: %s\n", Mix_GetError());
        return;
    }
    Mix_AllocateChannels(24);
    Mix_ReserveChannels(2);  // 0 and 1: ambience
    ready = true;
    char path[512];
    for (int i = 0; i < N_CUES; i++) {
        // "message" reuses alliance-suggested's sound.
        for (int j = 0; j < i; j++) {
            if (!strcmp(CUES[j].file, CUES[i].file)) cue_chunks[i] = cue_chunks[j];
        }
        if (cue_chunks[i]) continue;
        snprintf(path, sizeof path, "%ssounds/effects/%s.mp3", romfs, CUES[i].file);
        cue_chunks[i] = Mix_LoadWAV(path);
        if (!cue_chunks[i]) fprintf(stderr, "sound %s: %s\n", path, Mix_GetError());
    }
    for (int i = 0; i < N_AMBIENCE; i++) {
        snprintf(path, sizeof path, "%ssounds/ambience/%s.mp3", romfs, AMBIENCE[i]);
        amb_chunks[i] = Mix_LoadWAV(path);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%smusic/%s.mp3", settings_dir(), MUSIC_FILES[i]);
        music[i] = Mix_LoadMUS(path);
    }
    Mix_HookMusicFinished(on_music_finished);
    apply_music_volume();
}

void sound_shutdown(void) {
    if (!ready) return;
    Mix_HaltMusic();
    Mix_HaltChannel(-1);
    for (int i = 0; i < 4; i++) {
        if (music[i]) Mix_FreeMusic(music[i]);
    }
    for (int i = 0; i < N_AMBIENCE; i++) {
        if (amb_chunks[i]) Mix_FreeChunk(amb_chunks[i]);
    }
    for (int i = 0; i < N_CUES; i++) {
        bool shared = false;
        for (int j = 0; j < i; j++) shared |= cue_chunks[j] == cue_chunks[i];
        if (cue_chunks[i] && !shared) Mix_FreeChunk(cue_chunks[i]);
    }
    Mix_CloseAudio();
    Mix_Quit();
    ready = false;
}

void sound_play(const char *name) {
    // OPENFRONT_SOUND_LOG=1: print the cues (tests).
    static int log = -1;
    if (log < 0) log = getenv("OPENFRONT_SOUND_LOG") != NULL;
    if (log && name) printf("sound: %s\n", name);
    if (!ready || !name) return;
    for (int i = 0; i < N_CUES; i++) {
        if (strcmp(CUES[i].name, name) != 0) continue;
        if (!cue_chunks[i]) return;
        int ch = Mix_PlayChannel(-1, cue_chunks[i], 0);
        if (ch >= 0) Mix_Volume(ch, mix_volume(channel_gain(CUES[i].ch)));
        return;
    }
}

void sound_ambience(const char *track, float gain) {
    if (!ready) return;
    int t = -1;
    for (int i = 0; track && i < N_AMBIENCE; i++) {
        if (!strcmp(AMBIENCE[i], track)) t = i;
    }
    if (t >= 0 && !amb_chunks[t]) t = -1;
    if (t != amb_track) {
        if (amb_track >= 0) Mix_FadeOutChannel(amb_channel, AMB_FADE_MS);
        amb_track = t;
        if (t >= 0) {
            amb_channel = 1 - amb_channel;
            Mix_HaltChannel(amb_channel);
            Mix_Volume(amb_channel, 0);
            Mix_FadeInChannel(amb_channel, amb_chunks[t], -1, AMB_FADE_MS);
        }
    }
    amb_gain = gain;
    apply_ambience_volume();
}

void sound_music(bool in_game) {
    if (!ready) return;
    int want;
    if (!in_game) {
        want = 0;
    } else if (music_playing >= 1 && !music_ended) {
        want = music_playing;
    } else {
        // The next gameplay track (SoundManager plays them in turn).
        want = -1;
        for (int k = 0; k < 3 && want < 0; k++) {
            int i = 1 + (next_gameplay - 1 + k) % 3;
            if (music[i]) want = i;
        }
        if (want >= 0) next_gameplay = 1 + want % 3;
    }
    if (want == music_playing && !music_ended) return;
    if (want < 0 || !music[want]) {
        if (music_playing >= 0) Mix_HaltMusic();
        music_playing = -1;
        music_ended = false;
        return;
    }
    music_playing = want;
    Mix_FadeInMusic(music[want], want == 0 ? -1 : 1, 1000);
    // Starting a track stops the last one, which may report it as ended.
    music_ended = false;
    apply_music_volume();
}

float sound_volume(VolumeChannel c) { return volume[c]; }

void sound_set_volume(VolumeChannel c, float v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    volume[c] = v;
    settings_set_num(VOLUME_KEYS[c], roundf(v * 100) / 100);
    apply_music_volume();
    apply_ambience_volume();
}

bool sound_has_music(void) {
    for (int i = 0; i < 4; i++) {
        if (music[i]) return true;
    }
    return false;
}
