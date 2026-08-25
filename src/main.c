// c64uv - Commodore 64 Ultimate Viewer
//
// Streams the Ultimate's video/audio into an SDL3 window, forwards keystrokes,
// and shows the Ultimate menu over telnet. Protocol notes live in CLAUDE.md.

#define _DEFAULT_SOURCE // struct ip_mreq with -std=c11

#include <SDL3/SDL.h>
#include <curl/curl.h>

#define C64UV_VERSION "0.1.2"

#include "discover.h"
#include "keys.h"
#include "term.h"
#include "video.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct config {
    const char *host;       // Ultimate hostname/IP for REST
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

// Lazy TCP connection with rate-limited reconnect (used for ports 64 and 23).
static int tcp_connect_to(const char *host, Uint16 port, int timeout_s)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = timeout_s};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static atomic_bool g_quit;
// machine:input capability (probed once per session): -1 unknown, 0 no, 1 yes
static atomic_int g_minput = -1;

// ---------------------------------------------------------------- REST control

struct rest_ctx {
    char start_url[2][256]; // [0] video, [1] audio
    char stop_url[2][256];
    char input_url[256];    // machine:input, probed for capability
    int nstreams;
    int sock;                    // our UDP socket, for the ARP-priming packet
    struct sockaddr_in ult_addr; // Ultimate's address
    char prime_cmd[160];         // ping command forcing the LAN iface, or ""
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

// Returns HTTP status, or -1 on transport error. Response body (truncated) in
// resp. A non-NULL json_body is sent with Content-Type: application/json.
static long rest_req(CURL *curl, const char *method, const char *url,
                     const char *json_body, long timeout_ms, char *resp)
{
    resp[0] = '\0';
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    struct curl_slist *hdrs = NULL;
    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        hdrs = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }
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
    return rest_req(curl, "PUT", url, NULL, 3000, resp);
}

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
        // entry every cycle. A plain sendto can leave through the wrong
        // interface when policy routing claims the LAN (Tailscale
        // accept-routes does), so prefer ping -I on the subnet's interface,
        // which is allowed to force the egress device without privileges.
        if (rc->prime_cmd[0])
            (void)!system(rc->prime_cmd);
        else
            sendto(rc->sock, "", 1, 0, (struct sockaddr *)&rc->ult_addr,
                   sizeof rc->ult_addr);
        // Capability probe (GET is side-effect free), retried until the
        // machine gives an HTTP answer, then cached for the session.
        if (rc->input_url[0] && atomic_load(&g_minput) < 0) {
            long code = rest_req(curl, "GET", rc->input_url, NULL, 3000, resp);
            if (code == 200) {
                atomic_store(&g_minput, 1);
                SDL_Log("machine:input available: matrix-level keyboard");
            } else if (code > 0) {
                atomic_store(&g_minput, 0);
                SDL_Log("machine:input not supported (HTTP %ld): typing goes "
                        "via the KERNAL buffer", code);
            }
        }
        for (int i = 0; i < rc->nstreams; i++) {
            long code = rest_put(curl, rc->start_url[i], resp);
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
    for (int i = 0; i < rc->nstreams; i++)
        rest_put(curl, rc->stop_url[i], resp);
    curl_easy_cleanup(curl);
    return 0;
}

// Preferred detection: walk our interfaces and find the one whose subnet
// contains the Ultimate. Immune to policy-routing detours (Tailscale
// accept-routes) that make route-based lookups pick the wrong source.
static bool find_lan_iface(const char *host, char *ip, size_t iplen,
                           char *ifname, size_t iflen)
{
    struct in_addr target;
    if (inet_pton(AF_INET, host, &target) != 1)
        return false; // hostname given; caller falls back to route lookup
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) != 0)
        return false;
    bool found = false;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask)
            continue;
        struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        struct in_addr m = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr;
        if (!m.s_addr || (a.s_addr & m.s_addr) != (target.s_addr & m.s_addr))
            continue;
        // wired and wireless can share the subnet; prefer wired for 22 Mbps
        if (found && strncmp(ifname, "wl", 2) != 0)
            continue;
        inet_ntop(AF_INET, &a, ip, (socklen_t)iplen);
        snprintf(ifname, iflen, "%s", i->ifa_name);
        found = true;
    }
    freeifaddrs(ifs);
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
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1 && IN_MULTICAST(ntohl(a.s_addr));
}

static void mcast_next_group(const char *video, char *audio, size_t cap)
{
    struct in_addr a;
    inet_pton(AF_INET, video, &a);
    a.s_addr = htonl(ntohl(a.s_addr) + 1);
    inet_ntop(AF_INET, &a, audio, (socklen_t)cap);
}

// Joins on the given interface address (NULL = kernel picks by route).
static bool mcast_join(int sock, const char *group, const char *ifip)
{
    struct ip_mreq m = {0};
    if (inet_pton(AF_INET, group, &m.imr_multiaddr) != 1)
        return false;
    if (ifip)
        inet_pton(AF_INET, ifip, &m.imr_interface);
    return setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
}

// Fallback: connect a UDP socket toward the Ultimate and read back the source
// address the kernel picked.
static bool detect_local_ip(const char *host, char *out, size_t outlen)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return false;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(80)};
    bool ok = inet_pton(AF_INET, host, &sa.sin_addr) == 1 &&
              connect(s, (struct sockaddr *)&sa, sizeof sa) == 0;
    if (ok) {
        struct sockaddr_in local;
        socklen_t len = sizeof local;
        ok = getsockname(s, (struct sockaddr *)&local, &len) == 0 &&
             inet_ntop(AF_INET, &local.sin_addr, out, outlen) != NULL;
    }
    close(s);
    return ok;
}

// ------------------------------------------------------- keyboard passthrough
//
// TCP port 64, firmware "socket DMA" protocol: little-endian command word,
// u16 payload length, payload. KEYB (0xFF03) drops chars into the KERNAL
// keyboard buffer ($0277/$C6) - works for BASIC and anything else that reads
// input the normal way; games polling the matrix won't see it (needs the
// machine:input firmware feature, not shipped yet).

struct keyb {
    int fd; // -1 when disconnected
    bool enabled;
    const char *host;
    Uint64 last_try;
};

static void keyb_try_connect(struct keyb *k)
{
    if (!k->enabled || k->fd >= 0 ||
        (k->last_try != 0 && SDL_GetTicks() - k->last_try < 3000))
        return;
    k->last_try = SDL_GetTicks();
    k->fd = tcp_connect_to(k->host, 64, 1);
    if (k->fd >= 0)
        SDL_Log("keyboard channel connected (port 64)");
}

static void keyb_raw(struct keyb *k, Uint16 cmd, const Uint8 *data, int n)
{
    keyb_try_connect(k);
    if (k->fd < 0)
        return;
    Uint8 frame[4 + 16];
    frame[0] = cmd & 0xFF;
    frame[1] = cmd >> 8;
    frame[2] = (Uint8)n;
    frame[3] = 0;
    memcpy(frame + 4, data, (size_t)n);
    if (send(k->fd, frame, 4 + (size_t)n, MSG_NOSIGNAL) < 0) {
        close(k->fd);
        k->fd = -1; // reconnect on next keypress
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
    if (rest_req(mi->curl, "POST", mi->url, body, 250, resp) == -1) {
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
    int fd = tcp_connect_to(host, 23, 3);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s:23\n", host);
        return 1;
    }
    struct term *t = malloc(sizeof *t);
    term_init(t);
    Uint8 buf[4096];
    Uint64 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < 2500) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) > 0) {
            ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n <= 0)
                break;
            term_feed(t, buf, (int)n);
        }
    }
    close(fd);
    for (int r = 0; r < TERM_ROWS; r++)
        printf("%.*s\n", TERM_COLS, t->ch[r]);
    free(t);
    return 0;
}

// ---------------------------------------------------------------- main

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --host IP [--dest IP[:PORT]] [--port N] [--scale N]\n"
            "          [--multicast] [--no-start] [--no-audio] [--no-keyb]\n"
            "          [--dump FILE.ppm] [--term-test] [--discover] [--verbose]\n"
            "          [--version]\n"
            "  --host    C64 Ultimate address (or set C64U_HOST; omit to "
            "auto-discover)\n"
            "  --dest    where the Ultimate should send the streams (default: auto;\n"
            "            a multicast address is joined, audio group = video + 1)\n"
            "  --port    local UDP video port; audio uses port+1 (default 11000)\n"
            "  --multicast  stream via groups 239.0.1.64/.65 so several viewers\n"
            "            can watch the same Ultimate\n"
            "  --no-start  don't issue REST start/stop (e.g. mock stream test)\n"
            "  --dump    write first complete frame as PPM, then exit\n"
            "  --term-test  print the telnet menu screen as text, then exit\n"
            "  --discover  scan the local subnets for Ultimates, then exit\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct config cfg = {.host = getenv("C64U_HOST"),
                         .listen_port = 11000,
                         .scale = 2};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc)
            cfg.host = argv[++i];
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
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (cfg.discover) {
        struct discovered found[DISCOVER_MAX];
        int n = discover_scan(found, DISCOVER_MAX, true);
        for (int i = 0; i < n; i++)
            printf("%-15s  %s%s%s  firmware %s\n", found[i].ip,
                   found[i].product, found[i].hostname[0] ? "  " : "",
                   found[i].hostname, found[i].fw);
        if (n == 0)
            fprintf(stderr, "no Ultimate found\n");
        return n > 0 ? 0 : 1;
    }

    // No address given: try to find the machine ourselves before giving up.
    static char auto_host[46];
    if (!cfg.host && !cfg.no_start) {
        struct discovered found[DISCOVER_MAX];
        fprintf(stderr, "no --host given, discovering...\n");
        int n = discover_scan(found, DISCOVER_MAX, true);
        if (n >= 1) {
            // one machine can answer on both WiFi and wired; only warn when
            // genuinely different machines were found
            int distinct = 0;
            for (int i = 0; i < n; i++) {
                bool dup = false;
                for (int j = 0; j < i; j++)
                    dup |= found[i].uid[0] && !strcmp(found[i].uid,
                                                      found[j].uid);
                distinct += !dup;
            }
            if (distinct > 1) {
                fprintf(stderr, "found %d Ultimates, using the first; pass "
                        "--host to pick another:\n", distinct);
                for (int i = 0; i < n; i++)
                    fprintf(stderr, "  %-15s  %s\n", found[i].ip,
                            found[i].hostname);
            }
            snprintf(auto_host, sizeof auto_host, "%s", found[0].ip);
            cfg.host = auto_host;
            fprintf(stderr, "using %s (%s%s%s)\n", found[0].ip,
                    found[0].product, found[0].hostname[0] ? ", " : "",
                    found[0].hostname);
        }
    }
    if (!cfg.host && !cfg.no_start) {
        fprintf(stderr,
                "no Ultimate found on your network: pass --host <ip> or set "
                "C64U_HOST\n"
                "(find it on the machine: F5 menu on the Ultimate shows its "
                "IP, or check your router)\n");
        // Launched from a desktop entry there is no terminal to read stderr.
        if (!isatty(STDERR_FILENO) &&
            (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")))
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR, "Commodore 64 Ultimate Viewer",
                "No Ultimate found on your network.\n\n"
                "Check that the machine is powered on, or run from a\n"
                "terminal:  c64uv --host <ip>\n"
                "or set C64U_HOST in your environment.\n"
                "(F5 menu on the Ultimate shows its IP.)",
                NULL);
        return 2;
    }
    if (!cfg.host)
        cfg.host = ""; // --no-start mock mode needs no device

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
    // table's guess (policy routing can point elsewhere).
    char lan_ip[46], ifname[32] = "";
    bool on_lan = find_lan_iface(cfg.host, lan_ip, sizeof lan_ip, ifname,
                                 sizeof ifname);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    int rcvbuf = 1 << 20, one = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    if (mc_video[0]) // several viewers on one host share the port
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in bind_addr = {.sin_family = AF_INET,
                                    .sin_addr.s_addr = htonl(INADDR_ANY),
                                    .sin_port = htons((Uint16)cfg.listen_port)};
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) < 0) {
        perror("bind");
        return 1;
    }
    if (mc_video[0] &&
        !mcast_join(sock, mc_video, on_lan ? lan_ip : NULL)) {
        perror("multicast join");
        return 1;
    }
    int asock = -1;
    if (!cfg.no_audio) {
        asock = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(asock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
        if (mc_audio[0])
            setsockopt(asock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in aaddr = bind_addr;
        aaddr.sin_port = htons((Uint16)(cfg.listen_port + 1));
        if (bind(asock, (struct sockaddr *)&aaddr, sizeof aaddr) < 0) {
            perror("bind audio");
            return 1;
        }
        if (mc_audio[0] &&
            !mcast_join(asock, mc_audio, on_lan ? lan_ip : NULL)) {
            perror("multicast join audio");
            return 1;
        }
    }

    struct rest_ctx rc;
    SDL_Thread *ka = NULL;
    if (!cfg.no_start) {
        char dstv[64], dsta[64];
        if (mc_video[0]) {
            snprintf(dstv, sizeof dstv, "%s", mc_video);
            snprintf(dsta, sizeof dsta, "%s", mc_audio);
        } else if (cfg.dest) {
            snprintf(dstv, sizeof dstv, "%s", cfg.dest);
            char *colon = strchr(dstv, ':'); // legacy ip:port form
            if (colon) {
                cfg.listen_port = atoi(colon + 1);
                *colon = '\0';
            }
            snprintf(dsta, sizeof dsta, "%s", dstv);
        } else if (on_lan) {
            snprintf(dstv, sizeof dstv, "%s", lan_ip);
            snprintf(dsta, sizeof dsta, "%s", lan_ip);
        } else if (detect_local_ip(cfg.host, dstv, sizeof dstv)) {
            snprintf(dsta, sizeof dsta, "%s", dstv);
        } else {
            fprintf(stderr, "cannot detect local IP; use --dest\n");
            return 1;
        }
        if (on_lan)
            snprintf(rc.prime_cmd, sizeof rc.prime_cmd,
                     "ping -n -q -c 1 -W 1 -I '%s' '%s' >/dev/null 2>&1",
                     ifname, cfg.host);
        else
            rc.prime_cmd[0] = '\0';
        snprintf(rc.start_url[0], sizeof rc.start_url[0],
                 "http://%s/v1/streams/video:start?ip=%s:%d", cfg.host, dstv,
                 cfg.listen_port);
        snprintf(rc.stop_url[0], sizeof rc.stop_url[0],
                 "http://%s/v1/streams/video:stop", cfg.host);
        snprintf(rc.input_url, sizeof rc.input_url,
                 "http://%s/v1/machine:input", cfg.host);
        rc.nstreams = 1;
        if (asock >= 0) {
            snprintf(rc.start_url[1], sizeof rc.start_url[1],
                     "http://%s/v1/streams/audio:start?ip=%s:%d", cfg.host,
                     dsta, cfg.listen_port + 1);
            snprintf(rc.stop_url[1], sizeof rc.stop_url[1],
                     "http://%s/v1/streams/audio:stop", cfg.host);
            rc.nstreams = 2;
        }
        rc.sock = sock;
        rc.ult_addr = (struct sockaddr_in){.sin_family = AF_INET,
                                           .sin_port = htons(11000)};
        inet_pton(AF_INET, cfg.host, &rc.ult_addr.sin_addr);
        SDL_Log("requesting %s -> %s:%d", asock >= 0 ? "video+audio" : "video",
                dstv, cfg.listen_port);
        ka = SDL_CreateThread(keepalive_thread, "keepalive", &rc);
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
        if (!SDL_Init(SDL_INIT_VIDEO | (asock >= 0 ? SDL_INIT_AUDIO : 0))) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        if (!SDL_CreateWindowAndRenderer("c64uv - waiting for stream…",
                                         VIDEO_MAX_W * cfg.scale, 272 * cfg.scale,
                                         SDL_WINDOW_RESIZABLE, &win, &ren)) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return 1;
        }
        if (asock >= 0) {
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

    struct keyb kb = {.fd = -1, .host = cfg.host};
    struct minput mi = {0};
    if (windowed && !cfg.no_keyb && !cfg.no_start) {
        kb.enabled = true;
        SDL_StartTextInput(win);
        keyb_try_connect(&kb);
        snprintf(mi.url, sizeof mi.url, "http://%s/v1/machine:input",
                 cfg.host);
    }

    // Ultimate menu terminal (F9). Connected lazily on first toggle.
    struct term *trm = calloc(1, sizeof *trm);
    term_init(trm);
    bool term_active = false, term_present = false;
    int tfd = -1;
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
    // Audio latency control: startup fill and jitter leave a standing queue
    // that never drains on its own (input and output rates match). A servo on
    // the resample ratio (±2%, inaudible) steers the queue toward ~60 ms;
    // anything past 400 ms (pathological stall) is dropped outright.
    const int abytes_per_sec = 47983 * 4;
    const int aqueue_target = abytes_per_sec * 60 / 1000;
    const int aqueue_max = abytes_per_sec * 400 / 1000;
    Uint64 last_adj = 0;

    while (!atomic_load(&g_quit)) {
        if (windowed) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) {
                    atomic_store(&g_quit, true);
                } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    if ((ev.key.mod & SDL_KMOD_CTRL) && ev.key.key == SDLK_Q) {
                        atomic_store(&g_quit, true);
                    } else if (ev.key.key == SDLK_F9 && !cfg.no_start) {
                        term_active = !term_active;
                        term_present = true; // repaint whichever view we enter
                        if (term_active && atomic_load(&g_minput) == 1)
                            minput_release_all(&mi); // no keys stay held
                        if (term_active && tfd < 0 &&
                            (tfd_last_try == 0 ||
                             SDL_GetTicks() - tfd_last_try > 3000)) {
                            tfd_last_try = SDL_GetTicks();
                            tfd = tcp_connect_to(cfg.host, 23, 1);
                            if (tfd >= 0) {
                                term_init(trm);
                                SDL_Log("menu terminal connected (port 23)");
                            } else
                                SDL_Log("menu terminal: connect failed");
                        }
                    } else if (term_active) {
                        Uint8 seq[8];
                        int n = term_encode_key(ev.key.key, ev.key.mod, seq);
                        if (n > 0 && tfd >= 0 &&
                            send(tfd, seq, (size_t)n, MSG_NOSIGNAL) < 0) {
                            close(tfd);
                            tfd = -1;
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
                } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                    for (const char *p = ev.text.text; *p; p++) {
                        if (term_active) {
                            if ((unsigned char)*p < 128 && tfd >= 0 &&
                                send(tfd, p, 1, MSG_NOSIGNAL) < 0) {
                                close(tfd);
                                tfd = -1;
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

        struct pollfd pfd[3] = {{.fd = sock, .events = POLLIN},
                                {.fd = asock, .events = POLLIN},
                                {.fd = tfd, .events = POLLIN}};
        poll(pfd, 3, 5); // negative fds are ignored by poll

        if (tfd >= 0) { // keep the menu session current even when hidden
            ssize_t n;
            while ((n = recv(tfd, pkt, sizeof pkt, MSG_DONTWAIT)) > 0)
                term_feed(trm, pkt, (int)n);
            if (n == 0) { // server closed
                close(tfd);
                tfd = -1;
            }
        }

        if (asock >= 0) {
            ssize_t n;
            while ((n = recv(asock, pkt, sizeof pkt, MSG_DONTWAIT)) > 0) {
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
            ssize_t n = recv(sock, pkt, sizeof pkt, MSG_DONTWAIT);
            if (n < 0)
                break;
            packets++;
            frame_done = video_handle_packet(pkt, n, fb);
        }

        if (windowed && term_active && (trm->dirty || term_present)) {
            if (!term_tex) {
                term_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             TERM_PX_W, TERM_PX_H);
                SDL_SetTextureScaleMode(term_tex, SDL_SCALEMODE_NEAREST);
            }
            term_render(trm, term_px, TERM_PX_W);
            trm->dirty = false;
            term_present = false;
            SDL_UpdateTexture(term_tex, NULL, term_px,
                              TERM_PX_W * (int)sizeof(Uint32));
            // 2x vertical stretch: 8x8 glyphs read as 8x16 cells
            SDL_SetRenderLogicalPresentation(ren, TERM_PX_W, TERM_PX_H * 2,
                                             SDL_LOGICAL_PRESENTATION_LETTERBOX);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, term_tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        if (frame_done) {
            frames++;
            if (!got_any) {
                got_any = true;
                SDL_Log("receiving: %dx%d", fb->width, fb->height);
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
            if (!term_active) {
                SDL_UpdateTexture(tex, NULL, fb->px, VIDEO_MAX_W * sizeof(Uint32));
                SDL_SetRenderLogicalPresentation(
                    ren, fb->width, fb->height,
                    SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
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
    if (ka)
        SDL_WaitThread(ka, NULL);
    if (mi.curl)
        curl_easy_cleanup(mi.curl);
    curl_global_cleanup();
    if (kb.fd >= 0)
        close(kb.fd);
    if (tfd >= 0)
        close(tfd);
    if (term_tex)
        SDL_DestroyTexture(term_tex);
    free(term_px);
    free(trm);
    if (astream)
        SDL_DestroyAudioStream(astream);
    if (asock >= 0)
        close(asock);
    if (tex)
        SDL_DestroyTexture(tex);
    if (ren)
        SDL_DestroyRenderer(ren);
    if (win)
        SDL_DestroyWindow(win);
    if (windowed)
        SDL_Quit();
    close(sock);
    free(fb);
    return 0;
}
