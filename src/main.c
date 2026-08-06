// c64uv — Commodore 64 Ultimate video viewer
//
// Receives the Ultimate's raw VIC UDP stream (4bpp color indices) and shows it
// in an SDL3 window. Stream format: docs/data_streams in CLAUDE.md.

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include "term.h"

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

#define MAX_W 384
#define MAX_H 312 // safety headroom; PAL uses 272, NTSC 240
#define HDR_LEN 12

// Pepto/colodore-style VIC-II palette, ARGB8888.
static const Uint32 vic_palette[16] = {
    0xFF000000, 0xFFFFFFFF, 0xFF813338, 0xFF75CEC8, 0xFF8E3C97, 0xFF56AC4D,
    0xFF2E2C9B, 0xFFEDF171, 0xFF8E5029, 0xFF553800, 0xFFC46C71, 0xFF4A4A4A,
    0xFF7B7B7B, 0xFFA9FF9F, 0xFF706DEB, 0xFFB2B2B2,
};

struct config {
    const char *host;       // Ultimate hostname/IP for REST
    const char *dest;       // ip[:port] the stream should be sent to (auto if NULL)
    int listen_port;        // video; audio uses listen_port + 1
    int scale;
    bool no_start;          // don't touch REST (mock/local testing)
    bool no_audio;
    bool no_keyb;
    const char *dump_path;  // write first complete frame as PPM and exit
    bool term_test;         // headless telnet check: dump menu grid, exit
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

struct frame_buf {
    Uint32 px[MAX_W * MAX_H]; // palette-expanded
    int width, height;        // known once a last-flagged packet arrives
    Uint16 frame_no;
    int lines_got;
    bool ready;    // saw the last-packet flag for frame_no
    bool complete; // every line of the frame arrived
};

static atomic_bool g_quit;

// ---------------------------------------------------------------- REST control

struct rest_ctx {
    char start_url[2][256]; // [0] video, [1] audio
    char stop_url[2][256];
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

// Returns HTTP status, or -1 on transport error. Response body (truncated) in resp.
static long rest_put(CURL *curl, const char *url, char *resp)
{
    resp[0] = '\0';
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    if (curl_easy_perform(curl) != CURLE_OK)
        return -1;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
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
        // demand, so make it hear from us first — and keep refreshing its
        // entry every cycle. A plain sendto can leave through the wrong
        // interface when policy routing claims the LAN (Tailscale
        // accept-routes does), so prefer ping -I on the subnet's interface,
        // which is allowed to force the egress device without privileges.
        if (rc->prime_cmd[0])
            (void)!system(rc->prime_cmd);
        else
            sendto(rc->sock, "", 1, 0, (struct sockaddr *)&rc->ult_addr,
                   sizeof rc->ult_addr);
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
// keyboard buffer ($0277/$C6) — works for BASIC and anything else that reads
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
// keyboard scan restoring it — same trick the Ultimate's web UI uses.
static void keyb_stop(struct keyb *k)
{
    const Uint8 poke[] = {0x91, 0x00, 0x7F};
    for (int i = 0; i < 3; i++)
        keyb_raw(k, 0xFF06, poke, sizeof poke);
}

// ASCII → PETSCII for the default uppercase/graphics charset: lowercase
// letters are the unshifted keys, uppercase are shifted (graphics symbols).
static int ascii_to_petscii(unsigned char a)
{
    if (a >= 'a' && a <= 'z')
        return a - 'a' + 0x41;
    if (a >= 'A' && a <= 'Z')
        return a - 'A' + 0xC1;
    if (a >= ' ' && a <= '@') // space, punctuation, digits
        return a;
    switch (a) {
    case '[': return 0x5B;
    case ']': return 0x5D;
    case '^': return 0x5E; // up-arrow
    case '_': return 0xA4;
    }
    return -1; // no such key on a C64
}

static int special_to_petscii(SDL_Keycode key, SDL_Keymod mod)
{
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 0x0D;
    case SDLK_BACKSPACE:
    case SDLK_DELETE: return 0x14;
    case SDLK_INSERT: return 0x94;
    case SDLK_UP: return 0x91;
    case SDLK_DOWN: return 0x11;
    case SDLK_LEFT: return 0x9D;
    case SDLK_RIGHT: return 0x1D;
    case SDLK_HOME: return (mod & SDL_KMOD_SHIFT) ? 0x93 /*CLR*/ : 0x13;
    case SDLK_F1: return 0x85;
    case SDLK_F2: return 0x89;
    case SDLK_F3: return 0x86;
    case SDLK_F4: return 0x8A;
    case SDLK_F5: return 0x87;
    case SDLK_F6: return 0x8B;
    case SDLK_F7: return 0x88;
    case SDLK_F8: return 0x8C;
    }
    return -1;
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

// ---------------------------------------------------------------- video path

static bool handle_packet(const Uint8 *d, ssize_t len, struct frame_buf *fb)
{
    if (len < HDR_LEN)
        return false;
    Uint16 frame = (Uint16)(d[2] | d[3] << 8);
    Uint16 rawline = (Uint16)(d[4] | d[5] << 8);
    int line = rawline & 0x7FFF;
    bool last = rawline & 0x8000;
    int ppl = d[6] | d[7] << 8;
    int lpp = d[8];
    int bpp = d[9];
    int enc = d[10] | d[11] << 8;

    if (bpp != 4 || enc != 0 || ppl <= 0 || ppl > MAX_W || lpp <= 0 ||
        line + lpp > MAX_H || len < HDR_LEN + (ssize_t)(ppl / 2 * lpp))
        return false; // not a stream layout we understand

    if (frame != fb->frame_no) { // new frame begins; keep old pixels as filler
        fb->frame_no = frame;
        fb->lines_got = 0;
        fb->ready = false;
    }
    const Uint8 *src = d + HDR_LEN;
    for (int l = 0; l < lpp; l++) {
        Uint32 *dst = fb->px + (size_t)(line + l) * MAX_W;
        for (int x = 0; x < ppl; x += 2) {
            Uint8 b = *src++;
            dst[x] = vic_palette[b & 0x0F]; // low nibble = left pixel
            dst[x + 1] = vic_palette[b >> 4];
        }
    }
    fb->lines_got += lpp;
    fb->width = ppl;
    if (last) {
        fb->height = line + lpp;
        fb->ready = true;
        fb->complete = fb->lines_got >= fb->height;
    }
    return fb->ready;
}

static bool dump_ppm(const char *path, const struct frame_buf *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", fb->width, fb->height);
    for (int y = 0; y < fb->height; y++)
        for (int x = 0; x < fb->width; x++) {
            Uint32 p = fb->px[(size_t)y * MAX_W + x];
            fputc(p >> 16 & 0xFF, f);
            fputc(p >> 8 & 0xFF, f);
            fputc(p & 0xFF, f);
        }
    return fclose(f) == 0;
}

// ---------------------------------------------------------------- main

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --host IP [--dest IP[:PORT]] [--port N] [--scale N]\n"
            "          [--no-start] [--no-audio] [--no-keyb] [--dump FILE.ppm]\n"
            "          [--term-test] [--verbose]\n"
            "  --host    C64 Ultimate address (or set C64U_HOST)\n"
            "  --dest    where the Ultimate should send the streams (default: auto)\n"
            "  --port    local UDP video port; audio uses port+1 (default 11000)\n"
            "  --no-start  don't issue REST start/stop (e.g. mock stream test)\n"
            "  --dump    write first complete frame as PPM, then exit\n"
            "  --term-test  print the telnet menu screen as text, then exit\n",
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
        else if (!strcmp(argv[i], "--verbose"))
            cfg.verbose = true;
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!cfg.host && !cfg.no_start) {
        fprintf(stderr,
                "no Ultimate address: pass --host <ip> or set C64U_HOST\n"
                "(find it on the machine: F5 menu on the Ultimate shows its "
                "IP, or check your router)\n");
        return 2;
    }
    if (!cfg.host)
        cfg.host = ""; // --no-start mock mode needs no device

    if (cfg.term_test)
        return run_term_test(cfg.host);

    if (cfg.dump_path)
        cfg.no_audio = true; // headless frame grab needs no sound

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    int rcvbuf = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in bind_addr = {.sin_family = AF_INET,
                                    .sin_addr.s_addr = htonl(INADDR_ANY),
                                    .sin_port = htons((Uint16)cfg.listen_port)};
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) < 0) {
        perror("bind");
        return 1;
    }
    int asock = -1;
    if (!cfg.no_audio) {
        asock = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(asock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
        struct sockaddr_in aaddr = bind_addr;
        aaddr.sin_port = htons((Uint16)(cfg.listen_port + 1));
        if (bind(asock, (struct sockaddr *)&aaddr, sizeof aaddr) < 0) {
            perror("bind audio");
            return 1;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    struct rest_ctx rc;
    SDL_Thread *ka = NULL;
    if (!cfg.no_start) {
        char ip[64], lan_ip[46], ifname[32] = "";
        bool on_lan = find_lan_iface(cfg.host, lan_ip, sizeof lan_ip, ifname,
                                     sizeof ifname);
        if (cfg.dest) {
            snprintf(ip, sizeof ip, "%s", cfg.dest);
            char *colon = strchr(ip, ':'); // legacy ip:port form
            if (colon) {
                cfg.listen_port = atoi(colon + 1);
                *colon = '\0';
            }
        } else if (on_lan) {
            snprintf(ip, sizeof ip, "%s", lan_ip);
        } else if (!detect_local_ip(cfg.host, ip, sizeof ip)) {
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
                 "http://%s/v1/streams/video:start?ip=%s:%d", cfg.host, ip,
                 cfg.listen_port);
        snprintf(rc.stop_url[0], sizeof rc.stop_url[0],
                 "http://%s/v1/streams/video:stop", cfg.host);
        rc.nstreams = 1;
        if (asock >= 0) {
            snprintf(rc.start_url[1], sizeof rc.start_url[1],
                     "http://%s/v1/streams/audio:start?ip=%s:%d", cfg.host, ip,
                     cfg.listen_port + 1);
            snprintf(rc.stop_url[1], sizeof rc.stop_url[1],
                     "http://%s/v1/streams/audio:stop", cfg.host);
            rc.nstreams = 2;
        }
        rc.sock = sock;
        rc.ult_addr = (struct sockaddr_in){.sin_family = AF_INET,
                                           .sin_port = htons(11000)};
        inet_pton(AF_INET, cfg.host, &rc.ult_addr.sin_addr);
        SDL_Log("requesting %s -> %s:%d", asock >= 0 ? "video+audio" : "video",
                ip, cfg.listen_port);
        ka = SDL_CreateThread(keepalive_thread, "keepalive", &rc);
    }

    bool windowed = cfg.dump_path == NULL;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    SDL_AudioStream *astream = NULL;
    if (windowed) {
        if (!SDL_Init(SDL_INIT_VIDEO | (asock >= 0 ? SDL_INIT_AUDIO : 0))) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        if (!SDL_CreateWindowAndRenderer("c64uv — waiting for stream…",
                                         MAX_W * cfg.scale, 272 * cfg.scale,
                                         SDL_WINDOW_RESIZABLE, &win, &ren)) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return 1;
        }
        if (asock >= 0) {
            // Ultimate PAL audio clock; SDL resamples to whatever the
            // device wants. (NTSC is ~47940 — 0.09% off, inaudible.)
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
    if (windowed && !cfg.no_keyb && !cfg.no_start) {
        kb.enabled = true;
        SDL_StartTextInput(win);
        keyb_try_connect(&kb);
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
    fb->frame_no = 0xFFFF;
    fb->width = MAX_W;
    fb->height = 272;
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
                        if (ev.key.key == SDLK_ESCAPE) {
                            keyb_stop(&kb); // Esc = RUN/STOP
                        } else {
                            int c = special_to_petscii(ev.key.key, ev.key.mod);
                            if (c >= 0)
                                keyb_type(&kb, (Uint8)c);
                        }
                    }
                } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                    for (const char *p = ev.text.text; *p; p++) {
                        if (term_active) {
                            if ((unsigned char)*p < 128 && tfd >= 0 &&
                                send(tfd, p, 1, MSG_NOSIGNAL) < 0) {
                                close(tfd);
                                tfd = -1;
                            }
                        } else if (kb.enabled) {
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
            frame_done = handle_packet(pkt, n, fb);
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
                                 ? "c64uv — Esc = RUN/STOP, Ctrl+Q = quit"
                                 : "c64uv");
            }
            if (cfg.dump_path) {
                if (!fb->complete)
                    continue; // wait for a frame with no lost packets
                if (!dump_ppm(cfg.dump_path, fb)) {
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
                SDL_UpdateTexture(tex, NULL, fb->px, MAX_W * sizeof(Uint32));
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
    if (ka)
        SDL_WaitThread(ka, NULL);
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
