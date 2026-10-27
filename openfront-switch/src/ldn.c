#include "ldn.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#ifdef __SWITCH__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <switch.h>

// Shared by every console running the port: the network passphrase and the
// scene id the scans filter on. LocalCommunicationId -1 = the NACP's own.
static const char PASSPHRASE[] = "OpenFront-Switch-local-play-v1..";  // 32 bytes
#define SCENE_ID 0x4F46
#define LDN_VERSION 1
#define MAX_SCAN 8

typedef enum { MODE_NONE, MODE_HOST, MODE_STATION } Mode;

static bool initialized;
static Mode mode;
static char err[96];
static char uname[33] = "OpenFront";

static SDL_mutex *lock;  // scan results
static SDL_Thread *scan_thread;
static volatile bool scanning;
static LdnNetworkInfo scan_infos[MAX_SCAN];
static LdnGame scan_games[MAX_SCAN];
static int nscan;

static const char *fail(const char *what, Result rc) {
    snprintf(err, sizeof err, "%s %04d-%04d (0x%x)", what, 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
    return err;
}

static void fill_security(LdnSecurityConfig *sec) {
    memset(sec, 0, sizeof *sec);
    sec->security_mode = LdnSecurityMode_Product;
    sec->passphrase_size = sizeof PASSPHRASE - 1;
    memcpy(sec->passphrase, PASSPHRASE, sizeof PASSPHRASE - 1);
}

static void fill_user(LdnUserConfig *user, const char *name) {
    memset(user, 0, sizeof *user);
    snprintf(user->user_name, sizeof user->user_name, "%s", name);
}

static const char *ensure_init(void) {
    if (initialized) return NULL;
    Result rc = ldnInitialize(LdnServiceType_User);
    if (R_FAILED(rc)) return fail("ldnInitialize", rc);
    if (!lock) lock = SDL_CreateMutex();
    initialized = true;
    return NULL;
}

bool ldn_supported(void) { return true; }

bool ldn_hosting(void) { return mode == MODE_HOST; }

const char *ldn_host_open(const char *user_name) {
    ldn_close();
    const char *e = ensure_init();
    if (e) return e;
    Result rc = ldnOpenAccessPoint();
    if (R_FAILED(rc)) return fail("OpenAccessPoint", rc);
    LdnSecurityConfig sec;
    LdnUserConfig user;
    fill_security(&sec);
    fill_user(&user, user_name);
    LdnNetworkConfig net;
    memset(&net, 0, sizeof net);
    net.intent_id.local_communication_id = -1;
    net.intent_id.scene_id = SCENE_ID;
    net.channel = 0;
    net.node_count_max = 8;
    net.local_communication_version = LDN_VERSION;
    rc = ldnCreateNetwork(&sec, &user, &net);
    if (R_FAILED(rc)) {
        ldnCloseAccessPoint();
        return fail("CreateNetwork", rc);
    }
    mode = MODE_HOST;
    return NULL;
}

void ldn_host_advertise(const char *json) {
    if (mode != MODE_HOST) return;
    size_t n = strlen(json);
    if (n > 0x180) n = 0x180;
    ldnSetAdvertiseData(json, n);
}

/** The lobby a network advertises (lanhost.c's announcement JSON). */
static bool parse_advert(const LdnNetworkInfo *info, LdnGame *g) {
    char buf[0x181];
    size_t n = info->advertise_data_size < 0x180 ? info->advertise_data_size : 0x180;
    memcpy(buf, info->advertise_data, n);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    const cJSON *of = cJSON_GetObjectItemCaseSensitive(j, "of");
    bool ok = cJSON_IsString(of) && !strcmp(of->valuestring, "openfront-switch");
    if (ok) {
        memset(g, 0, sizeof *g);
#define STR(k, dst)                                                              \
    do {                                                                         \
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);                 \
        if (cJSON_IsString(v)) snprintf(dst, sizeof dst, "%s", v->valuestring); \
    } while (0)
        STR("id", g->id);
        STR("host", g->host);
        STR("map", g->map);
        STR("v", g->version);
#undef STR
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "port");
        g->port = cJSON_IsNumber(v) ? v->valueint : 0;
        v = cJSON_GetObjectItemCaseSensitive(j, "players");
        g->players = cJSON_IsNumber(v) ? v->valueint : 0;
        g->started = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "started"));
    }
    cJSON_Delete(j);
    return ok;
}

static int scan_main(void *unused) {
    (void)unused;
    while (scanning) {
        LdnScanFilter filter;
        memset(&filter, 0, sizeof filter);
        filter.network_id.intent_id.local_communication_id = -1;
        filter.network_id.intent_id.scene_id = SCENE_ID;
        filter.flags = LdnScanFilterFlag_LocalCommunicationId | LdnScanFilterFlag_SceneId;
        static LdnNetworkInfo infos[MAX_SCAN];
        s32 total = 0;
        Result rc = ldnScan(0, &filter, infos, MAX_SCAN, &total);
        if (R_SUCCEEDED(rc)) {
            SDL_LockMutex(lock);
            nscan = 0;
            for (int i = 0; i < total && i < MAX_SCAN; i++) {
                LdnGame g;
                if (!parse_advert(&infos[i], &g)) continue;
                g.index = nscan;
                scan_infos[nscan] = infos[i];
                scan_games[nscan++] = g;
            }
            SDL_UnlockMutex(lock);
        }
        for (int i = 0; i < 15 && scanning; i++) SDL_Delay(100);
    }
    return 0;
}

const char *ldn_scan_start(const char *user_name) {
    snprintf(uname, sizeof uname, "%s", user_name);
    if (mode == MODE_STATION && scanning) return NULL;
    ldn_close();
    const char *e = ensure_init();
    if (e) return e;
    Result rc = ldnOpenStation();
    if (R_FAILED(rc)) return fail("OpenStation", rc);
    mode = MODE_STATION;
    nscan = 0;
    scanning = true;
    scan_thread = SDL_CreateThreadWithStackSize(scan_main, "ldnscan", 256 * 1024, NULL);
    return NULL;
}

static void stop_scan(void) {
    if (!scan_thread) return;
    scanning = false;
    SDL_WaitThread(scan_thread, NULL);
    scan_thread = NULL;
}

int ldn_scan_list(LdnGame *out, int max) {
    if (!lock) return 0;
    SDL_LockMutex(lock);
    int n = nscan < max ? nscan : max;
    memcpy(out, scan_games, sizeof(LdnGame) * (size_t)n);
    SDL_UnlockMutex(lock);
    return n;
}

const char *ldn_join(int index, char *host_ip, size_t n) {
    if (mode != MODE_STATION) return "not scanning";
    stop_scan();
    LdnNetworkInfo info;
    SDL_LockMutex(lock);
    bool found = index >= 0 && index < nscan;
    if (found) info = scan_infos[index];
    SDL_UnlockMutex(lock);
    if (!found) return "gone";
    LdnSecurityConfig sec;
    LdnUserConfig user;
    fill_security(&sec);
    fill_user(&user, uname);
    Result rc = ldnConnect(&sec, &user, LDN_VERSION, 0, &info);
    if (R_FAILED(rc)) return fail("Connect", rc);
    // The access point is the first node; its address comes in host order.
    struct in_addr a = {htonl(info.nodes[0].ip_addr.addr)};
    LdnNetworkInfo now;
    if (R_SUCCEEDED(ldnGetNetworkInfo(&now)) && now.nodes[0].ip_addr.addr) a.s_addr = htonl(now.nodes[0].ip_addr.addr);
    inet_ntop(AF_INET, &a, host_ip, (socklen_t)n);
    return NULL;
}

void ldn_close(void) {
    stop_scan();
    if (mode == MODE_HOST) {
        ldnDestroyNetwork();
        ldnCloseAccessPoint();
    } else if (mode == MODE_STATION) {
        LdnState st;
        if (R_SUCCEEDED(ldnGetState(&st)) && st == LdnState_StationConnected) ldnDisconnect();
        ldnCloseStation();
    }
    mode = MODE_NONE;
}

#else  // No local wireless on PC.

bool ldn_supported(void) { return false; }
const char *ldn_host_open(const char *user_name) {
    (void)user_name;
    return "LDN";
}
void ldn_host_advertise(const char *json) { (void)json; }
bool ldn_hosting(void) { return false; }
const char *ldn_scan_start(const char *user_name) {
    (void)user_name;
    return "LDN";
}
int ldn_scan_list(LdnGame *out, int max) {
    (void)out, (void)max;
    return 0;
}
const char *ldn_join(int index, char *host_ip, size_t n) {
    (void)index, (void)host_ip, (void)n;
    return "LDN";
}
void ldn_close(void) {}

#endif
