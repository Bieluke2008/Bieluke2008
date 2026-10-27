// Switch-to-Switch games in the same network: the host console runs the game
// server itself (js/server.ts in its own QuickJS runtime, on its own thread)
// and announces the lobby by UDP broadcast; the others find it and join it
// like any OpenFront server (mp.ts).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define LAN_PORT 47801            // the hosting console's game server (TCP)
#define LAN_DISCOVERY_PORT 47802  // lobby announcements (UDP broadcast)

/**
 * Starts the game server for a new lobby whose creator is `creator_token`.
 * Writes the lobby id and returns the TCP port, or 0 on failure.
 */
int lan_host_start(const char *romfs, const char *creator_token, char *game_id, size_t n);
void lan_host_stop(void);
bool lan_host_running(void);

/** This console's IPv4 address in the local network, "" if unknown. */
void net_local_ip(char *out, size_t n);

/** A lobby announced by another console. */
typedef struct {
    char ip[48];
    int port;
    char id[16];
    char host[64];     // the creator's name
    char map[48];      // GameMapType
    char version[16];  // the port version it runs
    int players;
    bool started;
    double seen_ms;
} LanGame;

void lan_discover_start(void);
void lan_discover_stop(void);
/** Reads pending announcements and returns the lobbies heard in the last seconds. */
int lan_discover_list(LanGame *out, int max);
