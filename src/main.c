// c64uv - Commodore 64 Ultimate Viewer
//
// Streams the Ultimate's video/audio into an SDL3 window, forwards keystrokes,
// and shows the Ultimate menu over telnet. Protocol notes live in CLAUDE.md.

#include <SDL3/SDL.h>
#include <curl/curl.h>

#define C64UV_VERSION "0.2.7"

#include "compat.h"
#include "discover.h"
#include "keys.h"
#include "term.h"
#include "video.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct config {
    const char *host;       // Ultimate hostname/IP for REST
    const char *password;   // network password -> X-Password header
    const char *do_action;  // one-shot machine control, then exit
    const char *run_path;   // one-shot: run this .prg/.crt/.sid/.d64, then exit
    const char *dest;       // ip[:port] the stream should be sent to (auto if NULL)
    int listen_port;        // video; audio uses listen_port + 1
    int scale;
    bool multicast;         // stream to the 239.0.1.64/.65 groups
    bool no_start;          // don't touch REST (mock/local testing)
    bool no_audio;
    bool no_keyb;
    const char *dump_path;  // write first complete frame as PPM and exit
    bool term_test;         // headless telnet check: dump menu grid, exit
    bool discover;          // sweep the local subnets for Ultimates and exit
    bool verbose;
};

static atomic_bool g_quit;
// Drag-and-drop transfer state shared between the run thread and the event
// loop. g_run_busy blocks a second drop and keeps the keyboard channel off
// port 64 while a transfer runs: the firmware serves one DMA client at a
// time, so an open keyboard connection would stall the image behind it.
// g_run_pct (-1 idle, else 0-100) drives the window title.
static atomic_bool g_run_busy;
static atomic_int g_run_pct = -1;
static char g_run_name[64]; // written before the run thread starts
// machine:input capability (probed once per session): -1 unknown, 0 no, 1 yes
static atomic_int g_minput = -1;
// Network password (firmware 3.12+), sent as X-Password on every REST call.
// Set once at startup, before any thread starts.
static const char *g_password;

// ---------------------------------------------------------------- REST control

struct rest_ctx {
    char start_url[2][256]; // [0] video, [1] audio
    char stop_url[2][256];
    char input_url[256];    // machine:input, probed for capability
    int nstreams;
    compat_sock sock;   // our UDP socket, for the ARP-priming packet
    char host[64];      // Ultimate's address, as given
    char prime_if[32];  // LAN interface to force the prime out of, or ""
};

static size_t curl_sink(char *data, size_t size, size_t nmemb, void *userp)
{
    char *buf = userp; // 512-byte response buffer, keeps first chunk only
    size_t n = size * nmemb;
    if (buf[0] == '\0') {
        size_t cap = 511 < n ? 511 : n;
        memcpy(buf, data, cap);
        buf[cap] = '\0';
    }
    return n;
}

// curl polls this about once a second while a request is in flight, also
// while it waits for a reply that never comes; a set flag aborts the call.
static int rest_cancel_cb(void *flag, curl_off_t dlt, curl_off_t dln,
                          curl_off_t ult, curl_off_t uln)
{
    (void)dlt; (void)dln; (void)ult; (void)uln;
    return atomic_load((const atomic_bool *)flag) ? 1 : 0;
}

// Returns HTTP status, or -1 on transport error (a cancelled call included).
// Response body (truncated) in resp. A non-NULL body of body_len bytes is
// sent with the given ctype. A non-NULL cancel flag ends the call early
// once it is set, so a background request cannot hold up an exit.
static long rest_req(CURL *curl, const char *method, const char *url,
                     const void *body, long body_len, const char *ctype,
                     long timeout_ms, char *resp, const atomic_bool *cancel)
{
    resp[0] = '\0';
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, rest_cancel_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
        // the callback does not run while a SYN goes unanswered, so bound
        // that part on its own (a LAN connect takes milliseconds; a
        // background call is retried anyway)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    struct curl_slist *hdrs = NULL;
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_len);
        char cthdr[64];
        snprintf(cthdr, sizeof cthdr, "Content-Type: %s", ctype);
        hdrs = curl_slist_append(hdrs, cthdr);
    }
    if (g_password) {
        char pwhdr[160];
        snprintf(pwhdr, sizeof pwhdr, "X-Password: %s", g_password);
        hdrs = curl_slist_append(hdrs, pwhdr);
    }
    if (hdrs)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    if (res != CURLE_OK)
        return -1;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

static long rest_put(CURL *curl, const char *url, char *resp)
{
    return rest_req(curl, "PUT", url, NULL, 0, NULL, 3000, resp, NULL);
}

// Set by the keepalive thread on its way out, so the main thread can keep
// the window responsive while it waits instead of blocking in a join.
static atomic_bool g_ka_done;

// Keepalive thread: re-issue the start command every 5 s so the stream survives
// machine resets and menu excursions. The command is idempotent.
static int keepalive_thread(void *arg)
{
    struct rest_ctx *rc = arg;
    CURL *curl = curl_easy_init();
    char resp[512];
    long last_code[2] = {-2, -2};
    while (!atomic_load(&g_quit)) {
        // The Ultimate refuses to start a stream toward an address missing
        // from its ARP table ("Network Host Resolve Error") and never ARPs on
        // demand, so make it hear from us first - and keep refreshing its
        // entry every cycle. A plain datagram can leave through the wrong
        // interface when policy routing claims the LAN (Tailscale
        // accept-routes does), so the prime is forced out of the subnet's
        // interface whenever one was found (see compat_arp_prime).
        compat_arp_prime(rc->sock, rc->host, rc->prime_if);
        // Capability probe (GET is side-effect free), retried until the
        // machine gives an HTTP answer, then cached for the session.
        if (rc->input_url[0] && atomic_load(&g_minput) < 0) {
            long code = rest_req(curl, "GET", rc->input_url, NULL, 0, NULL,
                                 3000, resp, &g_quit);
            if (atomic_load(&g_quit))
                break;
            if (code == 200) {
                atomic_store(&g_minput, 1);
                SDL_Log("machine:input available: matrix-level keyboard");
            } else if (code > 0) {
                atomic_store(&g_minput, 0);
                SDL_Log("machine:input not supported (HTTP %ld): typing goes "
                        "via the KERNAL buffer", code);
            }
        }
        for (int i = 0; i < rc->nstreams && !atomic_load(&g_quit); i++) {
            long code = rest_req(curl, "PUT", rc->start_url[i], NULL, 0,
                                 NULL, 3000, resp, &g_quit);
            if (atomic_load(&g_quit))
                break; // cancelled: the result says nothing about the machine
            if (code != last_code[i]) { // log only on state change
                if (code == 200)
                    SDL_Log("stream start OK (%s)", rc->start_url[i]);
                else if (code == -1)
                    SDL_Log("stream start: no response from Ultimate");
                else
                    SDL_Log("stream start HTTP %ld: %s%s", code, resp,
                            strstr(resp, "No Operational Network Interface")
                                ? " -> plug the Ultimate into wired Ethernet; "
                                  "streams don't work over its WiFi"
                                : "");
                last_code[i] = code;
            }
        }
        for (int i = 0; i < 50 && !atomic_load(&g_quit); i++)
            SDL_Delay(100);
    }
    // Stop the streams on the way out, briefly: a machine that stopped
    // answering (powered off) gets no stop calls at all, and one that is
    // there answers well within a second. Anything longer would hold the
    // exit, and the window with it, past the compositor's patience.
    for (int i = 0; i < rc->nstreams; i++)
        if (last_code[i] != -1)
            rest_req(curl, "PUT", rc->stop_url[i], NULL, 0, NULL, 1000, resp,
                     NULL);
    curl_easy_cleanup(curl);
    atomic_store(&g_ka_done, true);
    return 0;
}

// Preferred detection: walk our interfaces and find the one whose subnet
// contains the Ultimate. Immune to policy-routing detours (Tailscale
// accept-routes) that make route-based lookups pick the wrong source.
static bool find_lan_iface(const char *host, char *ip, size_t iplen,
                           char *ifname, size_t iflen)
{
    uint32_t target;
    if (!compat_ipv4_parse(host, &target))
        return false; // hostname given; caller falls back to route lookup
    struct compat_iface ifs[32];
    int n = compat_ifaces(ifs, 32);
    bool found = false, found_wireless = false;
    for (int k = 0; k < n; k++) {
        const struct compat_iface *i = &ifs[k];
        if (!i->mask || (i->addr & i->mask) != (target & i->mask))
            continue;
        // wired and wireless can share the subnet; prefer wired for 22 Mbps
        if (found && !found_wireless)
            continue;
        compat_ipv4_format(i->addr, ip, iplen);
        snprintf(ifname, iflen, "%.31s", i->name); // both are 32-byte names
        found = true;
        found_wireless = i->wireless;
    }
    return found;
}

// ------------------------------------------------------------- multicast
//
// The Ultimate happily streams to a multicast group, which lifts the
// one-viewer-per-Ultimate limitation: every viewer joins the group and asks
// the Ultimate to stream there (the start command is idempotent for the same
// destination). Convention (from prkl_ultimate): video group, audio group =
// video + 1 on the last octet.

static bool is_multicast_ip(const char *s)
{
    uint32_t a;
    return compat_ipv4_parse(s, &a) && compat_ipv4_is_multicast(a);
}

static void mcast_next_group(const char *video, char *audio, size_t cap)
{
    uint32_t a = 0;
    compat_ipv4_parse(video, &a);
    compat_ipv4_format(a + 1, audio, cap);
}

// Everything that needs the Ultimate's address: destination detection, ARP
// prime command, REST URLs, and the keepalive thread. Callable at startup
// (host given) or later, when background discovery finds the machine.
static SDL_Thread *net_start(struct config *cfg, struct rest_ctx *rc,
                             compat_sock sock, compat_sock asock,
                             const char *mc_video, const char *mc_audio)
{
    char lan_ip[46], ifname[32] = "";
    bool on_lan = find_lan_iface(cfg->host, lan_ip, sizeof lan_ip, ifname,
                                 sizeof ifname);
    if (on_lan && mc_video[0]) {
        // Background discovery bound the sockets before the host was known,
        // so the joins may sit on a routing-table-picked interface; join
        // again on the right one (a duplicate join just returns EADDRINUSE).
        (void)compat_mcast_join(sock, mc_video, lan_ip);
        if (asock != COMPAT_BAD_SOCK && mc_audio[0])
            (void)compat_mcast_join(asock, mc_audio, lan_ip);
    }
    char dstv[64], dsta[64];
    if (mc_video[0]) {
        snprintf(dstv, sizeof dstv, "%s", mc_video);
        snprintf(dsta, sizeof dsta, "%s", mc_audio);
    } else if (cfg->dest) {
        snprintf(dstv, sizeof dstv, "%s", cfg->dest);
        char *colon = strchr(dstv, ':'); // legacy ip:port form
        if (colon) {
            cfg->listen_port = atoi(colon + 1);
            *colon = '\0';
        }
        snprintf(dsta, sizeof dsta, "%s", dstv);
    } else if (on_lan) {
        snprintf(dstv, sizeof dstv, "%s", lan_ip);
        snprintf(dsta, sizeof dsta, "%s", lan_ip);
    } else if (compat_route_source_ip(cfg->host, dstv, sizeof dstv)) {
        // fallback: the source address the OS would route toward the host
        snprintf(dsta, sizeof dsta, "%s", dstv);
    } else {
        SDL_Log("cannot detect local IP; use --dest");
        return NULL;
    }
    snprintf(rc->host, sizeof rc->host, "%s", cfg->host);
    snprintf(rc->prime_if, sizeof rc->prime_if, "%s", on_lan ? ifname : "");
    snprintf(rc->start_url[0], sizeof rc->start_url[0],
             "http://%s/v1/streams/video:start?ip=%s:%d", cfg->host, dstv,
             cfg->listen_port);
    snprintf(rc->stop_url[0], sizeof rc->stop_url[0],
             "http://%s/v1/streams/video:stop", cfg->host);
    snprintf(rc->input_url, sizeof rc->input_url,
             "http://%s/v1/machine:input", cfg->host);
    rc->nstreams = 1;
    if (asock != COMPAT_BAD_SOCK) {
        snprintf(rc->start_url[1], sizeof rc->start_url[1],
                 "http://%s/v1/streams/audio:start?ip=%s:%d", cfg->host,
                 dsta, cfg->listen_port + 1);
        snprintf(rc->stop_url[1], sizeof rc->stop_url[1],
                 "http://%s/v1/streams/audio:stop", cfg->host);
        rc->nstreams = 2;
    }
    rc->sock = sock;
    SDL_Log("requesting %s -> %s:%d", asock != COMPAT_BAD_SOCK ? "video+audio" : "video",
            dstv, cfg->listen_port);
    return SDL_CreateThread(keepalive_thread, "keepalive", rc);
}

// Picks which discovery result to use (wired FPGA interface preferred among
// entries of the same machine) and logs the outcome. False when none found.
static bool discovery_choose(struct discovered *found, int n, char *out,
                             size_t outlen)
{
    if (n < 1)
        return false;
    // one machine can answer on both WiFi and wired; only warn when
    // genuinely different machines were found
    int distinct = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++)
            dup |= found[i].uid[0] && !strcmp(found[i].uid, found[j].uid);
        distinct += !dup;
    }
    if (distinct > 1) {
        fprintf(stderr, "found %d Ultimates, using the first; pass --host "
                "to pick another:\n", distinct);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "  %-15s  %s\n", found[i].ip, found[i].hostname);
    }
    // Same machine on WiFi + wired: the wired (FPGA) address must win -
    // only pings to it land in the ARP table the streams check, so the
    // WiFi address can't start a stream cold.
    int pick = 0;
    for (int i = 0; i < n; i++)
        if ((!found[i].uid[0] || !strcmp(found[i].uid, found[0].uid)) &&
            discover_ip_is_wired(found[i].ip)) {
            pick = i;
            break;
        }
    // test hook: when the sweep ran against an alternate port, the REST
    // calls must follow it there too
    const char *tport = getenv("C64U_DISCOVER_PORT");
    if (tport)
        snprintf(out, outlen, "%.39s:%.5s", found[pick].ip, tport);
    else
        snprintf(out, outlen, "%.45s", found[pick].ip);
    fprintf(stderr, "using %s (%s%s%s)\n", found[pick].ip,
            found[pick].product, found[pick].hostname[0] ? ", " : "",
            found[pick].hostname);
    return true;
}

// Background discovery for the windowed no-host start: the window opens
// immediately and shows progress while the sweep runs here.
struct disc_async {
    struct discovered found[DISCOVER_MAX];
    atomic_int n; // -1 while a sweep runs, result count when done
};

static int discover_thread(void *arg)
{
    struct disc_async *d = arg;
    atomic_store(&d->n, discover_scan(d->found, DISCOVER_MAX, false));
    return 0;
}

// ------------------------------------------------------ DMA socket (port 64)
//
// Firmware "socket DMA" service: little-endian command word, u16 payload
// length (the image and cartridge commands carry a third length byte),
// payload. With a network password set the first frame must be
// AUTHENTICATE, answered with one byte (1 = accepted); anything else on an
// unauthenticated connection makes the firmware drop it.

#define DMA_CMD_KEYB 0xFF03
#define DMA_CMD_DMAWRITE 0xFF06
#define DMA_CMD_RUN_IMG 0xFF0B // mount a .d64 on drive A, reset, LOAD"*",8,1, RUN
#define DMA_CMD_AUTHENTICATE 0xFF1F
#define DMA_MAX_PAYLOAD 200000 // firmware SOCKET_BUFFER_SIZE; longer is truncated

// The REST host may carry a :port (discovery test hook); the DMA socket
// wants the bare address. C64U_DMA_PORT overrides port 64 for tests.
static compat_sock dma_connect_raw(const char *host, int timeout_s)
{
    char ip[64];
    snprintf(ip, sizeof ip, "%s", host);
    char *colon = strchr(ip, ':');
    if (colon)
        *colon = '\0';
    const char *penv = getenv("C64U_DMA_PORT");
    return compat_tcp_connect(ip, penv ? (uint16_t)atoi(penv) : 64,
                              timeout_s);
}

// The socket's send timeout (the connect timeout, 1-3 s) caps a single
// send() call, and the Ultimate's TCP stack drains a disk image slowly, so
// a would-block only means "wait for room": keep going until the data is
// out or nothing moved for DMA_STALL_MS.
#define DMA_STALL_MS 30000

// A non-zero `total` publishes progress in g_run_pct (image transfers).
static bool send_all(compat_sock s, const void *data, size_t len,
                     size_t total)
{
    const uint8_t *p = data;
    Uint64 last_progress = SDL_GetTicks();
    while (len > 0) {
        int n = compat_send(s, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            last_progress = SDL_GetTicks();
            if (total)
                atomic_store(&g_run_pct, (int)((total - len) * 100 / total));
        } else if (n < 0 && compat_neterr_transient() &&
                   SDL_GetTicks() - last_progress < DMA_STALL_MS) {
            compat_wait_writable(s, 1000);
        } else {
            return false;
        }
    }
    return true;
}

// One frame; len24 adds the third length byte (RUN_IMG, MOUNT_IMG, RUN_CRT).
static bool dma_send(compat_sock s, uint16_t cmd, const void *data,
                     size_t len, bool len24)
{
    uint8_t hdr[5] = {cmd & 0xFF, cmd >> 8, len & 0xFF, (len >> 8) & 0xFF,
                      (len >> 16) & 0xFF};
    return send_all(s, hdr, len24 ? 5 : 4, 0) &&
           send_all(s, data, len, len24 ? len : 0);
}

// Connects and, when a password is set, authenticates. COMPAT_BAD_SOCK on
// failure (logged when the password was refused).
static compat_sock dma_connect(const char *host, int timeout_s)
{
    compat_sock s = dma_connect_raw(host, timeout_s);
    if (s == COMPAT_BAD_SOCK || !g_password)
        return s;
    uint8_t ok = 0;
    if (dma_send(s, DMA_CMD_AUTHENTICATE, g_password, strlen(g_password),
                 false) &&
        compat_wait_readable(&s, 1, 3000) > 0 &&
        compat_recv_nowait(s, &ok, 1) == 1 && ok == 1)
        return s;
    SDL_Log("DMA socket: the Ultimate refused the network password");
    compat_close(s);
    return COMPAT_BAD_SOCK;
}

// ------------------------------------------------------- keyboard passthrough
//
// KEYB (0xFF03) drops chars into the KERNAL keyboard buffer ($0277/$C6) -
// works for BASIC and anything else that reads input the normal way; games
// polling the matrix won't see it (needs the machine:input firmware
// feature, not shipped yet).

struct keyb {
    compat_sock fd; // COMPAT_BAD_SOCK when disconnected
    bool enabled;
    bool reclaim; // reconnect as soon as the running drop releases port 64
    const char *host;
    Uint64 last_try;
};

// Lazy connection with rate-limited reconnect.
static void keyb_try_connect(struct keyb *k)
{
    if (!k->enabled || k->fd != COMPAT_BAD_SOCK ||
        atomic_load(&g_run_busy) || // a transfer owns port 64 right now
        (k->last_try != 0 && SDL_GetTicks() - k->last_try < 3000))
        return;
    k->last_try = SDL_GetTicks();
    k->fd = dma_connect(k->host, 1);
    if (k->fd != COMPAT_BAD_SOCK)
        SDL_Log("keyboard channel connected (port 64)");
}

static void keyb_raw(struct keyb *k, Uint16 cmd, const Uint8 *data, int n)
{
    keyb_try_connect(k);
    if (k->fd == COMPAT_BAD_SOCK)
        return;
    if (!dma_send(k->fd, cmd, data, (size_t)n, false)) {
        compat_close(k->fd);
        k->fd = COMPAT_BAD_SOCK; // reconnect on next keypress
    }
}

static void keyb_type(struct keyb *k, Uint8 petscii)
{
    keyb_raw(k, 0xFF03, &petscii, 1);
}

// RUN/STOP isn't a buffer character: BASIC checks the stop flag at $91.
// Write it a few times (DMAWRITE) to win the race against the KERNAL's own
// keyboard scan restoring it - same trick the Ultimate's web UI uses.
static void keyb_stop(struct keyb *k)
{
    const Uint8 poke[] = {0x91, 0x00, 0x7F};
    for (int i = 0; i < 3; i++)
        keyb_raw(k, 0xFF06, poke, sizeof poke);
}

// ------------------------------------------------------- machine control
//
// Single REST calls: PUT /v1/machine:<action>. Shared by the Ctrl hotkeys
// and the one-shot --do flag.

static const char *const machine_actions[] = {
    "reset", "reboot", "pause", "resume", "menu_button", "poweroff", NULL,
};

static bool machine_ctl(const char *host, const char *action)
{
    static CURL *curl; // hotkeys reuse the connection
    if (!curl)
        curl = curl_easy_init();
    char url[256], resp[512];
    snprintf(url, sizeof url, "http://%s/v1/machine:%s", host, action);
    long code = rest_req(curl, "PUT", url, NULL, 0, NULL, 3000, resp, NULL);
    if (code == 200)
        SDL_Log("machine:%s OK", action);
    else if (code == -1)
        SDL_Log("machine:%s: no response from Ultimate", action);
    else
        SDL_Log("machine:%s HTTP %ld: %s", action, code, resp);
    return code == 200;
}

// --------------------------------------------------------------- file runner
//
// POST a .prg/.crt/.sid to the matching runners: endpoint. Verified on real
// firmware 1.1.0: run_prg takes the raw file as the request body, then the
// firmware itself resets the machine, types LOAD"/TEMP/TEMP0000",8,1 and
// RUN. Because that internal reset would boot a configured freezer cart
// into its menu instead, the Cartridge config item is blanked first and
// restored only after the machine came back up (config applies at reset
// time, so the program keeps running with the cart parked).

#define CART_CFG_PATH "configs/C64%20and%20Cartridge%20Settings/Cartridge"

static const char *runner_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    if (!SDL_strcasecmp(dot, ".prg"))
        return "run_prg";
    if (!SDL_strcasecmp(dot, ".crt"))
        return "run_crt";
    if (!SDL_strcasecmp(dot, ".sid"))
        return "sidplay";
    return NULL;
}

// Disk images. A .d64 goes through the DMA socket's RUN_IMG, which mounts
// it on drive A and has the firmware reset the machine, type LOAD"*",8,1
// and RUN (verified on 1.1.0; the image lands as /temp/tcpimage.d64, so
// writes never reach the dropped file). The other types the mount API
// accepts have no firmware autostart and are only mounted.
static const char *image_type_for(const char *path)
{
    static const char *const types[] = {"d64", "g64", "d71", "g71", "d81"};
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!SDL_strcasecmp(dot + 1, types[i]))
            return types[i];
    return NULL;
}

struct binbuf {
    uint8_t data[16];
    int len;
};

static size_t bin_sink(char *d, size_t size, size_t nmemb, void *userp)
{
    struct binbuf *b = userp;
    size_t n = size * nmemb;
    for (size_t i = 0; i < n && b->len < (int)sizeof b->data; i++)
        b->data[b->len++] = (uint8_t)d[i];
    return n;
}

// Readiness gate: the KERNAL zeroes $CC when it sits at a prompt with the
// cursor flashing. Two consecutive ready reads guard against sampling a
// transient zero mid-boot; the timeout covers programs that never return
// to the prompt (games) - by then the internal reset is long done.
static void wait_kernal_ready(CURL *curl, const char *host, int max_ms)
{
    char url[256];
    snprintf(url, sizeof url,
             "http://%s/v1/machine:readmem?address=00CC&length=1", host);
    int ready = 0;
    for (int t = 0; t < max_ms && ready < 2; t += 500) {
        struct binbuf b = {.len = 0};
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bin_sink);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
        struct curl_slist *hdrs = NULL;
        if (g_password) {
            char pwhdr[160];
            snprintf(pwhdr, sizeof pwhdr, "X-Password: %s", g_password);
            hdrs = curl_slist_append(NULL, pwhdr);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        }
        bool ok = curl_easy_perform(curl) == CURLE_OK;
        curl_slist_free_all(hdrs);
        if (ok && b.len >= 1 && b.data[0] == 0)
            ready++;
        else
            ready = 0;
        SDL_Delay(500);
    }
}

static bool run_file(const char *host, const char *path)
{
    const char *ep = runner_for(path);
    const char *img = ep ? NULL : image_type_for(path);
    if (!ep && !img) {
        SDL_Log("%s: only .prg, .crt, .sid and disk images (.d64, .g64, "
                ".d71, .g71, .d81) can be run", path);
        return false;
    }
    // the machine resets for a runner or a .d64 autostart; a plain mount
    // leaves it alone
    bool resets = ep || !strcmp(img, "d64");
    FILE *f = fopen(path, "rb");
    if (!f) {
        SDL_Log("%s: %s", path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 2 << 20) { // largest sensible .crt is ~1 MB
        SDL_Log("%s: unreasonable file size (%ld)", path, len);
        fclose(f);
        return false;
    }
    uint8_t *data = malloc((size_t)len);
    bool readok = data && fread(data, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!readok) {
        SDL_Log("%s: short read", path);
        free(data);
        return false;
    }

    CURL *curl = curl_easy_init();
    char url[512], resp[512];
    // cartridge parking (not for .crt: that one runs a cart on purpose)
    char cart[128];
    bool parked = false;
    if (resets && (!ep || strcmp(ep, "run_crt") != 0)) {
        snprintf(url, sizeof url, "http://%s/v1/%s", host, CART_CFG_PATH);
        if (rest_req(curl, "GET", url, NULL, 0, NULL, 3000, resp, NULL) ==
                200 &&
            json_find_str(resp, "current", cart, sizeof cart) && cart[0]) {
            char blank[560];
            snprintf(blank, sizeof blank, "%s?value=", url);
            parked = rest_put(curl, blank, resp) == 200;
            SDL_Log("cartridge '%s' parked for the run%s", cart,
                    parked ? "" : " FAILED");
        }
    }

    long code;
    if (ep) {
        snprintf(url, sizeof url, "http://%s/v1/runners:%s", host, ep);
        // generous timeout: the firmware saves the file before answering
        code = rest_req(curl, "POST", url, data, len,
                        "application/octet-stream", 15000, resp, NULL);
        if (code == 200)
            SDL_Log("runners:%s %s OK (%ld bytes)", ep, path, len);
        else if (code == -1)
            SDL_Log("runners:%s: no response from Ultimate", ep);
        else
            SDL_Log("runners:%s HTTP %ld: %s", ep, code, resp);
    } else if (resets) {
        // .d64: the firmware mounts and autostarts it (DMA socket RUN_IMG)
        code = -1;
        if (len > DMA_MAX_PAYLOAD) {
            SDL_Log("%s: %ld bytes exceeds the DMA socket's %d byte limit",
                    path, len, DMA_MAX_PAYLOAD);
        } else {
            compat_sock s = dma_connect(host, 3);
            if (s == COMPAT_BAD_SOCK)
                SDL_Log("%s: DMA socket (port 64) unreachable; is the "
                        "Ultimate DMA Service enabled?", path);
            else if (dma_send(s, DMA_CMD_RUN_IMG, data, (size_t)len, true))
                code = 200;
            else
                SDL_Log("%s: DMA socket send failed: %s", path,
                        compat_neterr());
            compat_close(s);
        }
        atomic_store(&g_run_pct, -1); // the title stops saying "sending"
        if (code == 200)
            SDL_Log("%s mounted on drive A and started (%ld bytes)", path,
                    len);
    } else {
        // other image types: mount only, the machine keeps running
        snprintf(url, sizeof url, "http://%s/v1/drives/a:mount?type=%s", host,
                 img);
        code = rest_req(curl, "POST", url, data, len,
                        "application/octet-stream", 15000, resp, NULL);
        if (code == 200)
            SDL_Log("%s mounted on drive A (%ld bytes); no autostart for "
                    ".%s, type LOAD\"*\",8,1 yourself", path, len, img);
        else if (code == -1)
            SDL_Log("drives/a:mount: no response from Ultimate");
        else
            SDL_Log("drives/a:mount HTTP %ld: %s", code, resp);
    }
    free(data);

    if (parked) {
        wait_kernal_ready(curl, host, 10000);
        char restore[560], *esc = curl_easy_escape(curl, cart, 0);
        snprintf(restore, sizeof restore, "http://%s/v1/%s?value=%s", host,
                 CART_CFG_PATH, esc ? esc : "");
        curl_free(esc);
        if (rest_put(curl, restore, resp) == 200)
            SDL_Log("cartridge '%s' restored (applies at next reset)", cart);
        else
            SDL_Log("cartridge restore FAILED - check the Ultimate's "
                    "Cartridge setting");
    }
    curl_easy_cleanup(curl);
    return code == 200;
}

// Drag-and-drop runs on a worker thread: the whole sequence can take
// seconds and must not freeze the viewer.
struct runjob {
    char host[64];
    char path[1024];
};

static int run_thread(void *arg)
{
    struct runjob *j = arg;
    run_file(j->host, j->path);
    free(j);
    atomic_store(&g_run_pct, -1);
    atomic_store(&g_run_busy, false);
    return 0;
}

// Called from the event loop; hands port 64 to the transfer by dropping
// the keyboard connection (it reconnects on the next keypress after the
// run). NULL kb for the headless --run path.
static void run_file_async(const char *host, const char *path,
                           struct keyb *kb)
{
    if (atomic_exchange(&g_run_busy, true)) {
        SDL_Log("still busy with the previous file");
        return;
    }
    if (kb) {
        compat_close(kb->fd);
        kb->fd = COMPAT_BAD_SOCK;
        kb->reclaim = kb->enabled;
    }
    const char *base = strrchr(path, '/');
    snprintf(g_run_name, sizeof g_run_name, "%s", base ? base + 1 : path);
    atomic_store(&g_run_pct, 0);
    struct runjob *j = malloc(sizeof *j);
    snprintf(j->host, sizeof j->host, "%s", host);
    snprintf(j->path, sizeof j->path, "%s", path);
    SDL_DetachThread(SDL_CreateThread(run_thread, "runfile", j));
}

// ----------------------------------------------- matrix keyboard (REST)
//
// When the firmware supports machine:input (probed by the keepalive thread),
// key presses and releases go to the CIA1 matrix instead of the KERNAL
// buffer: games, chords, and held keys work. Sent synchronously from the
// event loop with a short timeout; repeated transport failures flip the
// session back to the buffer path.

struct minput {
    CURL *curl;
    char url[256];
    int fails;
};

static void minput_post(struct minput *mi, const char *body)
{
    if (!mi->curl)
        mi->curl = curl_easy_init();
    char resp[512];
    if (rest_req(mi->curl, "POST", mi->url, body, (long)strlen(body),
                 "application/json", 250, resp, NULL) == -1) {
        if (++mi->fails >= 3) {
            atomic_store(&g_minput, 0);
            SDL_Log("machine:input unreachable, falling back to the KERNAL "
                    "buffer");
        }
    } else {
        mi->fails = 0;
    }
}

static void minput_key(struct minput *mi, SDL_Keycode key, bool down)
{
    const char *names[2];
    int n = key_to_c64_matrix(key, names);
    if (n == 0)
        return;
    bool tap = strcmp(names[0], "restore") == 0; // tap-only per the API
    if (tap && !down)
        return;
    char body[192];
    if (matrix_event_json(names, n, tap ? "tap" : down ? "press" : "release",
                          body, sizeof body))
        minput_post(mi, body);
}

// Fired on focus loss, view switches, and exit so no key stays held down.
static void minput_release_all(struct minput *mi)
{
    minput_post(mi, "{\"events\":[{\"kind\":\"release_all\"}]}");
}

// ------------------------------------------------------ telnet menu terminal
//
// Port 23 mirrors the Ultimate's menu as a VT100 session (src/term.c). The
// menu overlay is not part of the VIC video stream, so this is the only way
// to see it remotely.

// Headless verification: connect, read for a bit, print the parsed grid.
static int run_term_test(const char *host)
{
    compat_sock fd = compat_tcp_connect(host, 23, 3);
    if (fd == COMPAT_BAD_SOCK) {
        fprintf(stderr, "cannot connect to %s:23\n", host);
        return 1;
    }
    struct term *t = malloc(sizeof *t);
    term_init(t);
    Uint8 buf[4096];
    Uint64 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < 2500) {
        if (compat_wait_readable(&fd, 1, 200) > 0) {
            int n = compat_recv_nowait(fd, buf, sizeof buf);
            if (n <= 0)
                break;
            term_feed(t, buf, n);
        }
    }
    compat_close(fd);
    for (int r = 0; r < TERM_ROWS; r++)
        printf("%.*s\n", TERM_COLS, t->ch[r]);
    free(t);
    return 0;
}

// ------------------------------------------------------------- help overlay
//
// The F10 view draws pixels directly (term_draw_text): 12 spaced rows do
// not fit the terminal's fixed 24-row grid, and this way the line pitch is
// free. Same font, same colors.

#define HELP_PX_W TERM_PX_W
#define HELP_PX_H 232
#define HELP_LINE 11 // 8 px glyphs + 3 px of air between rows
#define HELP_BLUE 0xFF2E2C9Bu

static void help_render(uint32_t *px)
{
    for (int i = 0; i < HELP_PX_W * HELP_PX_H; i++)
        px[i] = HELP_BLUE;
    int y = 14;
    term_draw_text(px, HELP_PX_W, (HELP_PX_W - 16 * 8) / 2, y, 0xFFFFFFFF,
                   "c64uv " C64UV_VERSION " keys");
    y += 8 + 18;
    for (int i = 0; i < viewer_bindings_count; i++) {
        term_draw_text(px, HELP_PX_W, 24, y, 0xFFEDF171, // VIC yellow
                       viewer_bindings[i].label);
        term_draw_text(px, HELP_PX_W, 200, y, 0xFFB2B2B2, // light grey
                       viewer_bindings[i].desc);
        y += HELP_LINE + (viewer_bindings[i].gap ? 8 : 0);
    }
    term_draw_text(px, HELP_PX_W, 24, y + 12, 0xFF7B7B7B, // dark grey
                   "F10 or Esc closes this help");
}

// The status screen shown while there is no video yet (discovery running,
// stream not started) reuses the terminal grid renderer.

static void status_center(struct term *t, int row, uint8_t color,
                          const char *s)
{
    int col = (TERM_COLS - (int)strlen(s)) / 2;
    if (col < 0)
        col = 0;
    for (; *s && col < TERM_COLS; col++, s++) {
        t->ch[row][col] = *s;
        t->fg[row][col] = color;
    }
}

static void status_set(struct term *t, const char *l1, const char *l2)
{
    term_init(t);
    status_center(t, 10, 1, "c64uv " C64UV_VERSION);
    if (l1)
        status_center(t, 12, 15, l1);
    if (l2)
        status_center(t, 14, 12, l2);
    t->cx = -1;
    t->dirty = true;
}

// The stream-lost screen; re-issued whenever a short notice has used the
// status grid in the meantime.
static void status_stream_lost(struct term *t, const char *host)
{
    char msg[TERM_COLS + 1];
    if (host)
        snprintf(msg, sizeof msg, "no stream from %s", host);
    else
        snprintf(msg, sizeof msg, "the stream stopped");
    status_set(t, msg, "is the Ultimate powered on? waiting...");
}

// ---------------------------------------------------------------- main

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --host IP [--dest IP[:PORT]] [--port N] [--scale N]\n"
            "          [--multicast] [--no-start] [--no-audio] [--no-keyb]\n"
            "          [--password PW] [--do ACTION] [--dump FILE.ppm]\n"
            "          [--term-test] [--discover] [--verbose] [--version]\n"
            "  --host    C64 Ultimate address (or set C64U_HOST; omit to "
            "auto-discover)\n"
            "  --dest    where the Ultimate should send the streams (default: auto;\n"
            "            a multicast address is joined, audio group = video + 1)\n"
            "  --port    local UDP video port; audio uses port+1 (default 11000)\n"
            "  --multicast  stream via groups 239.0.1.64/.65 so several viewers\n"
            "            can watch the same Ultimate\n"
            "  --password  network password (or set C64U_PASSWORD)\n"
            "  --do      one machine action, then exit: reset reboot pause\n"
            "            resume menu poweroff\n"
            "  --run     run a .prg/.crt/.sid/.d64 on the machine, then exit\n"
            "            (.g64/.d71/.g71/.d81 are mounted without autostart)\n"
            "            (in the window: drop the file onto it instead)\n"
            "  --no-start  don't issue REST start/stop (e.g. mock stream test)\n"
            "  --dump    write first complete frame as PPM, then exit\n"
            "  --term-test  print the telnet menu screen as text, then exit\n"
            "  --discover  scan the local subnets for Ultimates, then exit\n",
            argv0);
    fprintf(stderr, "keys in the viewer window (F10 shows this in-window):\n");
    for (int i = 0; i < viewer_bindings_count; i++)
        fprintf(stderr, "  %-22s%s\n%s", viewer_bindings[i].label,
                viewer_bindings[i].desc, viewer_bindings[i].gap ? "\n" : "");
}

int main(int argc, char **argv)
{
    struct config cfg = {.host = getenv("C64U_HOST"),
                         .listen_port = 11000,
                         .scale = 2};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc)
            cfg.host = argv[++i];
        else if (!strcmp(argv[i], "--password") && i + 1 < argc)
            cfg.password = argv[++i];
        else if (!strcmp(argv[i], "--do") && i + 1 < argc)
            cfg.do_action = argv[++i];
        else if (!strcmp(argv[i], "--run") && i + 1 < argc)
            cfg.run_path = argv[++i];
        else if (!strcmp(argv[i], "--dest") && i + 1 < argc)
            cfg.dest = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            cfg.listen_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            cfg.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--multicast"))
            cfg.multicast = true;
        else if (!strcmp(argv[i], "--no-start"))
            cfg.no_start = true;
        else if (!strcmp(argv[i], "--no-audio"))
            cfg.no_audio = true;
        else if (!strcmp(argv[i], "--no-keyb"))
            cfg.no_keyb = true;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            cfg.dump_path = argv[++i];
        else if (!strcmp(argv[i], "--term-test"))
            cfg.term_test = true;
        else if (!strcmp(argv[i], "--discover"))
            cfg.discover = true;
        else if (!strcmp(argv[i], "--verbose"))
            cfg.verbose = true;
        else if (!strcmp(argv[i], "--version")) {
            puts("c64uv " C64UV_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!cfg.password)
        cfg.password = getenv("C64U_PASSWORD");
    g_password = cfg.password;
    if (g_password)
        // discovery reads the env; SDL wraps the C runtime setenv portably
        SDL_setenv_unsafe("C64U_PASSWORD", g_password, 1);
    if (cfg.do_action) {
        if (!strcmp(cfg.do_action, "menu"))
            cfg.do_action = "menu_button";
        bool known = false;
        for (int i = 0; machine_actions[i]; i++)
            known |= !strcmp(cfg.do_action, machine_actions[i]);
        if (!known) {
            fprintf(stderr,
                    "--do: unknown action '%s' (one of: reset reboot pause "
                    "resume menu poweroff)\n", cfg.do_action);
            return 2;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!compat_net_init()) {
        fprintf(stderr, "network init: %s\n", compat_neterr());
        return 1;
    }

    if (cfg.discover) {
        struct discovered found[DISCOVER_MAX];
        int n = discover_scan(found, DISCOVER_MAX, true);
        for (int i = 0; i < n; i++)
            printf("%-15s  %s%s%s  firmware %s%s\n", found[i].ip,
                   found[i].product, found[i].hostname[0] ? "  " : "",
                   found[i].hostname, found[i].fw,
                   discover_ip_is_wired(found[i].ip)
                       ? "  (wired)" : "");
        if (n == 0)
            fprintf(stderr, "no Ultimate found\n");
        return n > 0 ? 0 : 1;
    }

    // No address given: headless modes discover synchronously (they need
    // the answer before doing their one thing); the windowed viewer opens
    // immediately and discovers in the background instead.
    static char auto_host[46];
    bool headless = cfg.dump_path || cfg.term_test || cfg.do_action ||
                    cfg.run_path;
    if (!cfg.host && !cfg.no_start && headless) {
        struct discovered found[DISCOVER_MAX];
        fprintf(stderr, "no --host given, discovering...\n");
        int n = discover_scan(found, DISCOVER_MAX, true);
        if (discovery_choose(found, n, auto_host, sizeof auto_host)) {
            cfg.host = auto_host;
        } else {
            fprintf(stderr,
                    "no Ultimate found on your network: pass --host <ip> or "
                    "set C64U_HOST\n"
                    "(find it on the machine: F5 menu on the Ultimate shows "
                    "its IP, or check your router)\n");
            return 2;
        }
    }
    bool discovering = !cfg.host && !cfg.no_start;
    if (!cfg.host && cfg.no_start)
        cfg.host = ""; // mock mode needs no device

    if (cfg.do_action)
        return machine_ctl(cfg.host, cfg.do_action) ? 0 : 1;

    if (cfg.run_path)
        return run_file(cfg.host, cfg.run_path) ? 0 : 1;

    if (cfg.term_test)
        return run_term_test(cfg.host);

    if (cfg.dump_path)
        cfg.no_audio = true; // headless frame grab needs no sound

    // Work out the stream destination groups before the sockets exist, so the
    // memberships can be joined right after bind. --dest with a multicast
    // address behaves like --multicast with custom groups.
    char mc_video[46] = "", mc_audio[46] = "";
    if (cfg.multicast) {
        snprintf(mc_video, sizeof mc_video, "239.0.1.64");
        snprintf(mc_audio, sizeof mc_audio, "239.0.1.65");
    } else if (cfg.dest) {
        char iponly[46];
        snprintf(iponly, sizeof iponly, "%.*s",
                 (int)strcspn(cfg.dest, ":"), cfg.dest);
        if (is_multicast_ip(iponly)) {
            snprintf(mc_video, sizeof mc_video, "%s", iponly);
            mcast_next_group(mc_video, mc_audio, sizeof mc_audio);
        }
    }
    // The join must land on the interface the stream arrives on; when the
    // Ultimate's subnet is ours, use that interface rather than the routing
    // table's guess (policy routing can point elsewhere). With the host
    // still unknown (background discovery) net_start re-joins later.
    char lan_ip[46], ifname[32] = "";
    bool on_lan = cfg.host && find_lan_iface(cfg.host, lan_ip, sizeof lan_ip,
                                             ifname, sizeof ifname);
    (void)ifname;

    // 1 MB receive buffers: a frame is ~70 packets and the loop is polled
    // between renders. Multicast viewers on one host share the port.
    int rcvbuf = 1 << 20;
    compat_sock sock = compat_udp_bind((Uint16)cfg.listen_port, rcvbuf,
                                       mc_video[0] != '\0');
    if (sock == COMPAT_BAD_SOCK) {
        fprintf(stderr, "bind port %d: %s\n", cfg.listen_port, compat_neterr());
        return 1;
    }
    if (mc_video[0] &&
        !compat_mcast_join(sock, mc_video, on_lan ? lan_ip : NULL)) {
        fprintf(stderr, "multicast join: %s\n", compat_neterr());
        return 1;
    }
    compat_sock asock = COMPAT_BAD_SOCK;
    if (!cfg.no_audio) {
        asock = compat_udp_bind((Uint16)(cfg.listen_port + 1), rcvbuf,
                                mc_audio[0] != '\0');
        if (asock == COMPAT_BAD_SOCK) {
            fprintf(stderr, "bind audio port %d: %s\n", cfg.listen_port + 1,
                    compat_neterr());
            return 1;
        }
        if (mc_audio[0] &&
            !compat_mcast_join(asock, mc_audio, on_lan ? lan_ip : NULL)) {
            fprintf(stderr, "multicast join audio: %s\n", compat_neterr());
            return 1;
        }
    }

    static struct rest_ctx rc;
    SDL_Thread *ka = NULL;
    if (!cfg.no_start && cfg.host) {
        ka = net_start(&cfg, &rc, sock, asock, mc_video, mc_audio);
        if (!ka)
            return 1;
    }

    bool windowed = cfg.dump_path == NULL;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    SDL_AudioStream *astream = NULL;
    if (windowed) {
        // Identifier becomes the Wayland app_id / X11 WM_CLASS; it must match
        // the c64uv.desktop basename so desktops pair the window with its icon.
        SDL_SetAppMetadata("Commodore 64 Ultimate Viewer", C64UV_VERSION,
                           "c64uv");
        if (!SDL_Init(SDL_INIT_VIDEO | (asock != COMPAT_BAD_SOCK ? SDL_INIT_AUDIO : 0))) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        if (!SDL_CreateWindowAndRenderer(discovering
                                             ? "c64uv - looking for your Ultimate…"
                                             : "c64uv - waiting for stream…",
                                         VIDEO_MAX_W * cfg.scale, 272 * cfg.scale,
                                         SDL_WINDOW_RESIZABLE, &win, &ren)) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return 1;
        }
        if (asock != COMPAT_BAD_SOCK) {
            // Ultimate PAL audio clock; SDL resamples to whatever the
            // device wants. (NTSC is ~47940 - 0.09% off, inaudible.)
            SDL_AudioSpec aspec = {SDL_AUDIO_S16LE, 2, 47983};
            astream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &aspec, NULL, NULL);
            if (astream)
                SDL_ResumeAudioStreamDevice(astream);
            else
                SDL_Log("audio device unavailable: %s", SDL_GetError());
        }
    }

    struct keyb kb = {.fd = COMPAT_BAD_SOCK, .host = cfg.host};
    struct minput mi = {0};
    bool want_keyb = windowed && !cfg.no_keyb && !cfg.no_start;
    if (want_keyb)
        SDL_StartTextInput(win);
    if (want_keyb && cfg.host) {
        kb.enabled = true;
        keyb_try_connect(&kb);
        snprintf(mi.url, sizeof mi.url, "http://%s/v1/machine:input",
                 cfg.host);
    }

    // Ultimate menu terminal (F9). Connected lazily on first toggle.
    struct term *trm = calloc(1, sizeof *trm);
    term_init(trm);
    // Help overlay (F10): own pixel buffer, spaced lines don't fit the grid.
    uint32_t *help_px = calloc(HELP_PX_W * HELP_PX_H, sizeof(Uint32));
    SDL_Texture *help_tex = NULL;
    bool help_active = false;
    bool term_active = false, term_present = false;

    // Status screen: shown until the first video frame arrives.
    struct term *status = calloc(1, sizeof *status);
    {
        char msg[TERM_COLS + 1];
        if (discovering)
            status_set(status, "looking for your Ultimate...",
                       "scanning the local subnets");
        else if (cfg.no_start) {
            snprintf(msg, sizeof msg, "listening for a stream on UDP :%d",
                     cfg.listen_port);
            status_set(status, msg, NULL);
        } else {
            snprintf(msg, sizeof msg, "waiting for the stream from %s",
                     cfg.host);
            status_set(status, msg, NULL);
        }
    }

    // Background discovery (windowed start with no host).
    static struct disc_async da;
    SDL_Thread *dthr = NULL;
    Uint64 next_scan = 0;
    if (discovering) {
        atomic_store(&da.n, -1);
        dthr = SDL_CreateThread(discover_thread, "discover", &da);
    }
    compat_sock tfd = COMPAT_BAD_SOCK;
    int shown_pct = -1; // drop progress currently in the window title
    Uint64 tfd_last_try = 0;
    SDL_Texture *term_tex = NULL;
    Uint32 *term_px = calloc(TERM_PX_W * TERM_PX_H, sizeof(Uint32));

    struct frame_buf *fb = calloc(1, sizeof *fb);
    video_init(fb);
    int tex_h = 0;
    Uint8 pkt[2048];
    Uint64 last_stat = SDL_GetTicks();
    long frames = 0, packets = 0, apackets = 0, agaps = 0;
    Uint16 aseq_prev = 0;
    bool aseq_valid = false;
    bool got_any = false;
    // A stream that stops (machine powered off, cable out) must not leave
    // the last frame standing as if the viewer hung: after a moment the
    // status screen says what is going on, and the stream view returns by
    // itself with the next frame.
    Uint64 last_frame = 0;
    bool stream_lost = false;
    // A short notice on the status grid (menu unreachable, link lost)
    // shows until this tick, then the regular status text comes back.
    Uint64 notice_until = 0;
    bool paused = false; // Ctrl+P toggle state (viewer-side best guess)
    // Audio latency control: startup fill and jitter leave a standing queue
    // that never drains on its own (input and output rates match). A servo on
    // the resample ratio (±2%, inaudible) steers the queue toward ~60 ms;
    // anything past 400 ms (pathological stall) is dropped outright.
    const int abytes_per_sec = 47983 * 4;
    const int aqueue_target = abytes_per_sec * 60 / 1000;
    const int aqueue_max = abytes_per_sec * 400 / 1000;
    Uint64 last_adj = 0;

    while (!atomic_load(&g_quit)) {
        if (discovering) {
            int dn = atomic_load(&da.n);
            if (dthr && dn >= 0) { // sweep finished
                SDL_WaitThread(dthr, NULL);
                dthr = NULL;
                if (discovery_choose(da.found, dn, auto_host,
                                     sizeof auto_host)) {
                    cfg.host = auto_host;
                    discovering = false;
                    char msg[TERM_COLS + 1];
                    snprintf(msg, sizeof msg, "found %s", cfg.host);
                    status_set(status, msg, "starting the stream...");
                    term_present = true;
                    if (win)
                        SDL_SetWindowTitle(win, "c64uv - waiting for stream…");
                    ka = net_start(&cfg, &rc, sock, asock, mc_video,
                                   mc_audio);
                    if (want_keyb) {
                        kb.host = cfg.host;
                        kb.enabled = true;
                        keyb_try_connect(&kb);
                        snprintf(mi.url, sizeof mi.url,
                                 "http://%s/v1/machine:input", cfg.host);
                    }
                } else {
                    status_set(status, "no Ultimate found on your network",
                               "is it powered on? retrying in 10 s");
                    term_present = true;
                    next_scan = SDL_GetTicks() + 10000;
                }
            } else if (!dthr && SDL_GetTicks() >= next_scan) {
                atomic_store(&da.n, -1);
                dthr = SDL_CreateThread(discover_thread, "discover", &da);
                status_set(status, "looking for your Ultimate...",
                           "scanning the local subnets");
                term_present = true;
            }
        }
        if (windowed) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) {
                    atomic_store(&g_quit, true);
                } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    enum viewer_action va =
                        viewer_binding_match(ev.key.key, ev.key.mod);
                    if (va == VA_QUIT) {
                        atomic_store(&g_quit, true);
                    } else if (va == VA_HELP) {
                        help_active = !help_active;
                        term_present = true;
                    } else if (help_active) {
                        if (ev.key.key == SDLK_ESCAPE) {
                            help_active = false;
                            term_present = true;
                        } // anything else stays local while help is up
                    } else if (va == VA_RESET && !cfg.no_start && cfg.host) {
                        machine_ctl(cfg.host, "reset");
                    } else if (va == VA_REBOOT && !cfg.no_start && cfg.host) {
                        machine_ctl(cfg.host, "reboot");
                    } else if (va == VA_PAUSE && !cfg.no_start && cfg.host) {
                        paused = !paused;
                        machine_ctl(cfg.host, paused ? "pause" : "resume");
                    } else if (va == VA_MENU_BTN && !cfg.no_start &&
                               cfg.host) {
                        machine_ctl(cfg.host, "menu_button");
                    } else if (va == VA_MENU_VIEW && !cfg.no_start &&
                               cfg.host) {
                        term_present = true; // repaint whichever view we land in
                        if (term_active) {
                            term_active = false;
                        } else {
                            if (tfd == COMPAT_BAD_SOCK &&
                                (tfd_last_try == 0 ||
                                 SDL_GetTicks() - tfd_last_try > 3000)) {
                                tfd_last_try = SDL_GetTicks();
                                tfd = compat_tcp_connect(cfg.host, 23, 1);
                                if (tfd != COMPAT_BAD_SOCK) {
                                    term_init(trm);
                                    SDL_Log("menu terminal connected "
                                            "(port 23)");
                                } else
                                    SDL_Log("menu terminal: connect failed");
                            }
                            // no connection, no menu view: a blank or stale
                            // grid would pass for the machine's own screen
                            if (tfd != COMPAT_BAD_SOCK) {
                                term_active = true;
                                if (atomic_load(&g_minput) == 1)
                                    minput_release_all(&mi); // no keys held
                            } else {
                                status_set(status,
                                           "cannot reach the Ultimate menu "
                                           "(telnet, port 23)",
                                           "is it powered on? is telnet "
                                           "enabled?");
                                notice_until = SDL_GetTicks() + 3000;
                            }
                        }
                    } else if (term_active) {
                        Uint8 seq[8];
                        int n = term_encode_key(ev.key.key, ev.key.mod, seq);
                        if (n > 0 && tfd != COMPAT_BAD_SOCK &&
                            compat_send(tfd, seq, (size_t)n) < 0) {
                            compat_close(tfd);
                            tfd = COMPAT_BAD_SOCK;
                        }
                    } else if (kb.enabled) {
                        if (atomic_load(&g_minput) == 1) {
                            if (!ev.key.repeat) // the matrix has no repeat
                                minput_key(&mi, ev.key.key, true);
                        } else if (ev.key.key == SDLK_ESCAPE) {
                            keyb_stop(&kb); // Esc = RUN/STOP
                        } else {
                            int c = special_to_petscii(ev.key.key, ev.key.mod);
                            if (c >= 0)
                                keyb_type(&kb, (Uint8)c);
                        }
                    }
                } else if (ev.type == SDL_EVENT_KEY_UP) {
                    if (!term_active && kb.enabled &&
                        atomic_load(&g_minput) == 1)
                        minput_key(&mi, ev.key.key, false);
                } else if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    if (atomic_load(&g_minput) == 1)
                        minput_release_all(&mi);
                } else if (ev.type == SDL_EVENT_WINDOW_RESIZED ||
                           ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           ev.type == SDL_EVENT_WINDOW_EXPOSED) {
                    // repaint the current grid view at the new size; the
                    // tiling WM resizes the window right after it maps, and
                    // a stale frame would be shown scaled and off-center
                    term_present = true;
                } else if (ev.type == SDL_EVENT_DROP_FILE) {
                    if (!cfg.no_start && cfg.host && ev.drop.data)
                        run_file_async(cfg.host, ev.drop.data, &kb);
                } else if (ev.type == SDL_EVENT_TEXT_INPUT && !help_active) {
                    for (const char *p = ev.text.text; *p; p++) {
                        if (term_active) {
                            if ((unsigned char)*p < 128 &&
                                tfd != COMPAT_BAD_SOCK &&
                                compat_send(tfd, p, 1) < 0) {
                                compat_close(tfd);
                                tfd = COMPAT_BAD_SOCK;
                            }
                        } else if (kb.enabled &&
                                   atomic_load(&g_minput) != 1) {
                            // matrix mode types via key events instead
                            int c = ascii_to_petscii((unsigned char)*p);
                            if (c >= 0)
                                keyb_type(&kb, (Uint8)c);
                        }
                    }
                }
            }
        }

        // a drop hands port 64 to the transfer; take the keyboard channel
        // back as soon as it ends so the next keystroke is not lost (only
        // then: a blind retry every loop would block the render loop for
        // the connect timeout whenever the Ultimate is unreachable)
        if (kb.reclaim && !atomic_load(&g_run_busy)) {
            kb.reclaim = false;
            keyb_try_connect(&kb);
        }

        if (windowed) { // a running drop shows its progress in the title
            int pct = atomic_load(&g_run_pct);
            if (pct != shown_pct) {
                shown_pct = pct;
                char title[128];
                if (pct >= 0)
                    snprintf(title, sizeof title, "c64uv - sending %s %d%%",
                             g_run_name, pct);
                else
                    snprintf(title, sizeof title, "%s",
                             !got_any ? "c64uv - waiting for stream…"
                             : kb.enabled
                                 ? "c64uv - Esc = RUN/STOP, Ctrl+Q = quit"
                                 : "c64uv");
                SDL_SetWindowTitle(win, title);
            }
        }

        compat_sock waitset[3] = {sock, asock, tfd}; // unset ones are skipped
        compat_wait_readable(waitset, 3, 5);

        if (tfd != COMPAT_BAD_SOCK) { // keep the menu session current even when hidden
            int n;
            while ((n = compat_recv_nowait(tfd, pkt, sizeof pkt)) > 0)
                term_feed(trm, pkt, n);
            if (n == 0) { // server closed
                compat_close(tfd);
                tfd = COMPAT_BAD_SOCK;
            }
        }
        if (term_active && tfd == COMPAT_BAD_SOCK) { // closed or send failed
            term_active = false;
            term_present = true;
            status_set(status, "the Ultimate menu connection was lost",
                       "F9 reconnects");
            notice_until = SDL_GetTicks() + 3000;
            SDL_Log("menu terminal: connection lost");
        }

        if (asock != COMPAT_BAD_SOCK) {
            int n;
            while ((n = compat_recv_nowait(asock, pkt, sizeof pkt)) > 0) {
                if (n < 6)
                    continue;
                Uint16 aseq = (Uint16)(pkt[0] | pkt[1] << 8);
                if (aseq_valid && (Uint16)(aseq - aseq_prev) != 1)
                    agaps++;
                aseq_prev = aseq;
                aseq_valid = true;
                apackets++;
                if (astream)
                    SDL_PutAudioStreamData(astream, pkt + 2, (int)n - 2);
            }
            if (astream) {
                int q = SDL_GetAudioStreamQueued(astream);
                if (q > aqueue_max) {
                    SDL_ClearAudioStream(astream);
                } else if (SDL_GetTicks() - last_adj >= 1000) {
                    float err_s = (float)(q - aqueue_target) / abytes_per_sec;
                    float ratio = 1.0f + err_s * 0.5f;
                    ratio = ratio < 0.98f ? 0.98f : ratio > 1.02f ? 1.02f : ratio;
                    SDL_SetAudioStreamFrequencyRatio(astream, ratio);
                    last_adj = SDL_GetTicks();
                }
            }
        }

        bool frame_done = false;
        while (!frame_done) { // drain socket, stop at a completed frame
            int n = compat_recv_nowait(sock, pkt, sizeof pkt);
            if (n < 0)
                break;
            packets++;
            frame_done = video_handle_packet(pkt, n, fb);
        }

        if (windowed && help_active && term_present) {
            if (!help_tex) {
                help_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             HELP_PX_W, HELP_PX_H);
                SDL_SetTextureScaleMode(help_tex, SDL_SCALEMODE_NEAREST);
            }
            help_render(help_px);
            term_present = false;
            SDL_UpdateTexture(help_tex, NULL, help_px,
                              HELP_PX_W * (int)sizeof(Uint32));
            // same 2x vertical stretch as the terminal views
            SDL_SetRenderLogicalPresentation(ren, HELP_PX_W, HELP_PX_H * 2,
                                             SDL_LOGICAL_PRESENTATION_LETTERBOX);
            SDL_SetRenderDrawColor(ren, 0x2E, 0x2C, 0x9B, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, help_tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        if (got_any && !stream_lost && SDL_GetTicks() - last_frame > 2000) {
            stream_lost = true;
            status_stream_lost(status, cfg.host);
            notice_until = 0;
            term_present = true;
            if (windowed)
                SDL_SetWindowTitle(win, "c64uv - waiting for stream…");
            // a silent power-off never closes the menu link, so its last
            // screen would stay up as if live; F9 reconnects later
            if (tfd != COMPAT_BAD_SOCK) {
                compat_close(tfd);
                tfd = COMPAT_BAD_SOCK;
                term_active = false;
            }
            SDL_Log("stream stopped");
        }
        if (notice_until && SDL_GetTicks() >= notice_until) {
            notice_until = 0;
            if (stream_lost)
                status_stream_lost(status, cfg.host);
            else if (!got_any && !discovering && cfg.host) {
                char msg[TERM_COLS + 1];
                snprintf(msg, sizeof msg, "waiting for the stream from %s",
                         cfg.host);
                status_set(status, msg, NULL);
            }
            term_present = true;
        }

        struct term *view = help_active ? NULL
                            : term_active ? trm
                            : !got_any || stream_lost || notice_until
                                ? status
                                : NULL;
        if (windowed && view && (view->dirty || term_present)) {
            if (!term_tex) {
                term_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             TERM_PX_W, TERM_PX_H);
                SDL_SetTextureScaleMode(term_tex, SDL_SCALEMODE_NEAREST);
            }
            term_render(view, term_px, TERM_PX_W);
            view->dirty = false;
            term_present = false;
            SDL_UpdateTexture(term_tex, NULL, term_px,
                              TERM_PX_W * (int)sizeof(Uint32));
            // 2x vertical stretch: 8x8 glyphs read as 8x16 cells
            SDL_SetRenderLogicalPresentation(ren, TERM_PX_W, TERM_PX_H * 2,
                                             SDL_LOGICAL_PRESENTATION_LETTERBOX);
            // fill the letterbox borders with the grid's C64 blue, so the
            // status/help/menu screens cover the whole window
            SDL_SetRenderDrawColor(ren, 0x2E, 0x2C, 0x9B, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, term_tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        if (frame_done) {
            frames++;
            last_frame = SDL_GetTicks();
            if (!got_any || stream_lost) {
                if (got_any)
                    SDL_Log("stream resumed");
                else
                    SDL_Log("receiving: %dx%d", fb->width, fb->height);
                got_any = true;
                stream_lost = false;
                if (windowed)
                    SDL_SetWindowTitle(
                        win, kb.enabled
                                 ? "c64uv - Esc = RUN/STOP, Ctrl+Q = quit"
                                 : "c64uv");
            }
            if (cfg.dump_path) {
                if (!fb->complete)
                    continue; // wait for a frame with no lost packets
                if (!video_dump_ppm(cfg.dump_path, fb)) {
                    perror("dump");
                    return 1;
                }
                SDL_Log("wrote %s (%dx%d)", cfg.dump_path, fb->width, fb->height);
                break;
            }
            if (fb->height != tex_h) { // (re)create texture on size detection
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, fb->width,
                                        fb->height);
                SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
                tex_h = fb->height;
            }
            if (!term_active && !help_active) {
                SDL_UpdateTexture(tex, NULL, fb->px, VIDEO_MAX_W * sizeof(Uint32));
                SDL_SetRenderLogicalPresentation(
                    ren, fb->width, fb->height,
                    SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); // video keeps black
                SDL_RenderClear(ren);
                SDL_RenderTexture(ren, tex, NULL, NULL);
                SDL_RenderPresent(ren);
            }
        }

        if (cfg.verbose && SDL_GetTicks() - last_stat >= 5000) {
            int qms = astream
                          ? (int)(SDL_GetAudioStreamQueued(astream) * 1000LL /
                                  (47983 * 4))
                          : 0;
            SDL_Log("%.1f fps, %ld vpkts, %ld apkts (%ld gaps, %d ms queued)",
                    frames / 5.0, packets, apackets, agaps, qms);
            frames = packets = apackets = 0;
            last_stat = SDL_GetTicks();
        }
    }

    atomic_store(&g_quit, true);
    if (atomic_load(&g_minput) == 1 && mi.url[0])
        minput_release_all(&mi);
    // The background threads must not outlive curl, but a blocking join
    // would leave the compositor's pings unanswered: with the Ultimate
    // powered off, the old 3 s REST timeouts added up to a ten-second
    // "application not responding" hang on Ctrl+Q. Keep pumping events
    // until they are done (a sweep in progress, the keepalive's cancelled
    // request or its quick stop calls).
    if (dthr) { // sweep still running
        while (windowed && atomic_load(&da.n) < 0) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        SDL_WaitThread(dthr, NULL);
    }
    if (ka) {
        while (windowed && !atomic_load(&g_ka_done)) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        SDL_WaitThread(ka, NULL);
    }
    if (mi.curl)
        curl_easy_cleanup(mi.curl);
    curl_global_cleanup();
    compat_close(kb.fd);
    compat_close(tfd);
    if (term_tex)
        SDL_DestroyTexture(term_tex);
    if (help_tex)
        SDL_DestroyTexture(help_tex);
    free(term_px);
    free(trm);
    free(help_px);
    free(status);
    if (astream)
        SDL_DestroyAudioStream(astream);
    compat_close(asock);
    if (tex)
        SDL_DestroyTexture(tex);
    if (ren)
        SDL_DestroyRenderer(ren);
    if (win)
        SDL_DestroyWindow(win);
    if (windowed)
        SDL_Quit();
    compat_close(sock);
    compat_net_quit();
    free(fb);
    return 0;
}
