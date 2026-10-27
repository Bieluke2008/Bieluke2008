#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ENTRIES 128
#define KEY_LEN 48
#define VAL_LEN 96

static struct {
    char key[KEY_LEN], val[VAL_LEN];
} entries[MAX_ENTRIES];
static int nentries;
static bool dirty;

const char *settings_dir(void) {
#ifdef __SWITCH__
    return "sdmc:/switch/openfront/";
#else
    return "./";
#endif
}

static void path(char *out, size_t n) { snprintf(out, n, "%sopenfront-settings.ini", settings_dir()); }

void settings_load(void) {
    char p[256];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[KEY_LEN + VAL_LEN + 4];
    while (fgets(line, sizeof line, f) && nentries < MAX_ENTRIES) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(entries[nentries].key, KEY_LEN, "%.*s", KEY_LEN - 1, line);
        snprintf(entries[nentries].val, VAL_LEN, "%.*s", VAL_LEN - 1, v);
        nentries++;
    }
    fclose(f);
}

static int find(const char *key) {
    for (int i = 0; i < nentries; i++) {
        if (!strcmp(entries[i].key, key)) return i;
    }
    return -1;
}

double settings_num(const char *key, double fallback) {
    int i = find(key);
    return i < 0 ? fallback : atof(entries[i].val);
}

const char *settings_str(const char *key, const char *fallback) {
    int i = find(key);
    return i < 0 ? fallback : entries[i].val;
}

void settings_set_str(const char *key, const char *value) {
    int i = find(key);
    if (i < 0) {
        if (nentries == MAX_ENTRIES) return;
        i = nentries++;
        snprintf(entries[i].key, KEY_LEN, "%s", key);
        entries[i].val[0] = 0;
    }
    if (strcmp(entries[i].val, value) != 0) {
        snprintf(entries[i].val, VAL_LEN, "%s", value);
        dirty = true;
    }
}

void settings_set_num(const char *key, double value) {
    char v[32];
    snprintf(v, sizeof v, "%g", value);
    settings_set_str(key, v);
}

void settings_save(void) {
    if (!dirty) return;
    dirty = false;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/openfront", 0777);
#endif
    char p[256];
    path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nentries; i++) fprintf(f, "%s=%s\n", entries[i].key, entries[i].val);
    fclose(f);
}
