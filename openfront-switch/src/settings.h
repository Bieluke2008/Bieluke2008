// The player's settings (volumes, name, last game options), kept on the
// microSD as key=value lines. Defaults come from the callers.
#pragma once

#include <stdbool.h>

/** Where settings, music and other player files live (ends with '/'). */
const char *settings_dir(void);
void settings_load(void);
double settings_num(const char *key, double fallback);
const char *settings_str(const char *key, const char *fallback);
void settings_set_num(const char *key, double value);
void settings_set_str(const char *key, const char *value);
/** Writes the file if something changed since the last save. */
void settings_save(void);
