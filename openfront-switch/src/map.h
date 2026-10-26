// The map view, drawn the way the web client's WebGL passes draw it:
// terrain, territory fill and borders (TerritoryPass, BorderStampPass),
// structures (StructurePass), units from the sprite atlas (UnitPass), names
// with flags, status icons and emoji (NamePass), nuke telegraphs, and the
// build preview (ghost, range circle, nuke trajectory).
#pragma once

#include <SDL.h>
#include <stdbool.h>

#include "cJSON.h"
#include "engine.h"

typedef struct {
    float cx, cy;  // map tile at the centre of the page
    float zoom;    // page pixels per tile (the web Camera's zoom)
} Camera;

#define CAM_MIN_ZOOM 0.2f
#define CAM_MAX_ZOOM 20.0f

typedef struct {
    int my_sid;
    int hover_owner;           // territory under the cursor (highlighted), 0 none
    bool spawn_phase;
    const cJSON *ghost;        // the adapter's ghost model (build preview), or NULL
    const char *ghost_type;    // UnitType string of the ghost, "" none
    const int *warships;       // selected warships
    int nwarships;
} MapOverlay;

bool map_init(const char *romfs);
void map_shutdown(void);
/** Forgets the current map (new game). */
void map_reset(void);
/** The flattened emoji table (index -> emoji) for emoji next to names. */
void map_set_emojis(const cJSON *list);

/** The alt view ("Toggle Terrain") and whose relations it shows. */
void map_set_view(bool alt, int my_sid);
/** Applies tile changes from the engine to the territory texture. */
void map_update(const Snapshot *s, int hover_owner);
void map_draw(const Snapshot *s, const Camera *cam, const MapOverlay *ov);

void cam_page_to_tile(const Camera *cam, float px, float py, float *tx, float *ty);
void cam_tile_to_page(const Camera *cam, float tx, float ty, float *px, float *py);
/** The web Camera's fit: the whole map at 90% of the view. */
void cam_fit(Camera *cam, int map_w, int map_h);
void cam_clamp(Camera *cam, int map_w, int map_h);
void cam_zoom_at(Camera *cam, float factor, float px, float py);

/** A terrain preview of a map, for the map picker (NULL if unavailable). */
SDL_Texture *map_preview(const char *romfs, const char *folder, int *w, int *h);

/** renderNumber() / renderTroops() of the web client. */
void format_number(double num, char *out, size_t n);
void format_troops(double troops, char *out, size_t n);
