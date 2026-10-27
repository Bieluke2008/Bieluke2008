// Audio like the web client's SoundManager/AudioMixer: OpenFront's sound
// effects and structure ambience (romfs/sounds), and its music when the
// player has copied it to the microSD (it is not ours to ship).
#pragma once

#include <stdbool.h>

// The web client's audio categories, in its settings order.
typedef enum { VOL_MASTER, VOL_MUSIC, VOL_EFFECTS, VOL_ALERTS, VOL_AMBIENCE, VOL_INTERFACE, VOL_COUNT } VolumeChannel;

void sound_init(const char *romfs);
void sound_shutdown(void);
/** Plays a cue by its web name ("click", "build-city", ...). */
void sound_play(const char *name);
/** The structure ambience loop: "city", "factory", "missile-silo", "sam-silo" or NULL; gain 0..1. */
void sound_ambience(const char *track, float gain);
/** Menu theme (false) or the gameplay playlist (true). Call every frame. */
void sound_music(bool in_game);
float sound_volume(VolumeChannel c);
void sound_set_volume(VolumeChannel c, float v);
/** Whether the music files were found. */
bool sound_has_music(void);
