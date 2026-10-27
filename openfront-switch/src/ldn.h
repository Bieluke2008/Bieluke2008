// The console's local wireless play (LDN, no router): the hosting Switch opens
// a local network and advertises its lobby; the others scan, connect, and then
// reach the host's game server (lanhost.c) by IP like on Wi-Fi.
//
// LDN accepts only the LocalCommunicationId in the running application's NACP:
// the installed NSP has its own, so any consoles with it see each other. From
// the Homebrew Menu (the .nro) it is the NACP of the game used to open it, so
// all consoles must open the menu from the same game.
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** An advertised lobby found by a scan. */
typedef struct {
    int index;         // for ldn_join
    char id[16];
    char host[64];
    char map[48];
    char version[16];
    int players;
    bool started;
    int port;
} LdnGame;

/** False when this build has no LDN (the PC build). */
bool ldn_supported(void);

/** Opens a local network for hosting; NULL on success, else an error to show. */
const char *ldn_host_open(const char *user_name);
/** The lobby's advertisement (lanhost.c's announcement JSON, at most 384 bytes). */
void ldn_host_advertise(const char *json);
bool ldn_hosting(void);

/** Starts scanning in the background (station mode); NULL or an error. */
const char *ldn_scan_start(const char *user_name);
/** The lobbies of the last scan. */
int ldn_scan_list(LdnGame *out, int max);
/** Connects to scanned lobby `index` and writes the host's IP; NULL or an error. */
const char *ldn_join(int index, char *host_ip, size_t n);

/** Leaves whatever local network this console is in. */
void ldn_close(void);
