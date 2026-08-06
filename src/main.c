// c64uv — Commodore 64 Ultimate video viewer
//
// Receives the Ultimate's raw VIC UDP stream (4bpp color indices) and shows it
// in an SDL3 window. Stream format: docs/data_streams in CLAUDE.md.

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
    int listen_port;
    int scale;
    bool no_start;          // don't touch REST (mock/local testing)
    const char *dump_path;  // write first complete frame as PPM and exit
    bool verbose;
};

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
    char start_url[256];
    char stop_url[256];
    int sock;                    // our UDP socket, for the ARP-priming packet
    struct sockaddr_in ult_addr; // Ultimate's address
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
    long last_code = -2;
    while (!atomic_load(&g_quit)) {
        // The Ultimate refuses to start a stream toward an address missing
        // from its ARP table ("Network Host Resolve Error"), so poke it with
        // a throwaway datagram from our source address first.
        sendto(rc->sock, "", 1, 0, (struct sockaddr *)&rc->ult_addr,
               sizeof rc->ult_addr);
        long code = rest_put(curl, rc->start_url, resp);
        if (code != last_code) { // log only on state change
            if (code == 200)
                SDL_Log("stream start OK (%s)", rc->start_url);
            else if (code == -1)
                SDL_Log("stream start: no response from Ultimate");
            else
                SDL_Log("stream start HTTP %ld: %s%s", code, resp,
                        strstr(resp, "No Operational Network Interface")
                            ? " -> plug the Ultimate into wired Ethernet; "
                              "streams don't work over its WiFi"
                            : "");
            last_code = code;
        }
        for (int i = 0; i < 50 && !atomic_load(&g_quit); i++)
            SDL_Delay(100);
    }
    rest_put(curl, rc->stop_url, resp);
    curl_easy_cleanup(curl);
    return 0;
}

// Local IP the Ultimate can reach us on: connect a UDP socket toward it and
// read back the chosen source address. Beware: with policy routing (Tailscale)
// this can pick the wrong interface — the --dest flag overrides.
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
            "usage: %s [--host IP] [--dest IP[:PORT]] [--port N] [--scale N]\n"
            "          [--no-start] [--dump FILE.ppm] [--verbose]\n"
            "  --host    Ultimate address (default 192.168.8.173)\n"
            "  --dest    where the Ultimate should send the stream (default: auto)\n"
            "  --port    local UDP listen port (default 11000)\n"
            "  --no-start  don't issue REST start/stop (e.g. mock stream test)\n"
            "  --dump    write first complete frame as PPM, then exit\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct config cfg = {.host = "192.168.8.236", .listen_port = 11000, .scale = 2};
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
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            cfg.dump_path = argv[++i];
        else if (!strcmp(argv[i], "--verbose"))
            cfg.verbose = true;
        else {
            usage(argv[0]);
            return 2;
        }
    }

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

    curl_global_init(CURL_GLOBAL_DEFAULT);
    struct rest_ctx rc;
    SDL_Thread *ka = NULL;
    if (!cfg.no_start) {
        char dest[64];
        if (cfg.dest) {
            snprintf(dest, sizeof dest, "%s", cfg.dest);
        } else {
            char ip[46];
            if (!detect_local_ip(cfg.host, ip, sizeof ip)) {
                fprintf(stderr, "cannot detect local IP; use --dest\n");
                return 1;
            }
            snprintf(dest, sizeof dest, "%s:%d", ip, cfg.listen_port);
        }
        snprintf(rc.start_url, sizeof rc.start_url,
                 "http://%s/v1/streams/video:start?ip=%s", cfg.host, dest);
        snprintf(rc.stop_url, sizeof rc.stop_url,
                 "http://%s/v1/streams/video:stop", cfg.host);
        rc.sock = sock;
        rc.ult_addr = (struct sockaddr_in){.sin_family = AF_INET,
                                           .sin_port = htons(11000)};
        inet_pton(AF_INET, cfg.host, &rc.ult_addr.sin_addr);
        SDL_Log("requesting video stream -> %s", dest);
        ka = SDL_CreateThread(keepalive_thread, "keepalive", &rc);
    }

    bool windowed = cfg.dump_path == NULL;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    if (windowed) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        if (!SDL_CreateWindowAndRenderer("c64uv — waiting for stream…",
                                         MAX_W * cfg.scale, 272 * cfg.scale,
                                         SDL_WINDOW_RESIZABLE, &win, &ren)) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return 1;
        }
    }

    struct frame_buf *fb = calloc(1, sizeof *fb);
    fb->frame_no = 0xFFFF;
    fb->width = MAX_W;
    fb->height = 272;
    int tex_h = 0;
    Uint8 pkt[2048];
    Uint64 last_stat = SDL_GetTicks();
    long frames = 0, packets = 0;
    bool got_any = false;

    while (!atomic_load(&g_quit)) {
        if (windowed) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_EVENT_QUIT ||
                    (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE))
                    atomic_store(&g_quit, true);
        }

        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        poll(&pfd, 1, 5);
        bool frame_done = false;
        while (!frame_done) { // drain socket, stop at a completed frame
            ssize_t n = recv(sock, pkt, sizeof pkt, MSG_DONTWAIT);
            if (n < 0)
                break;
            packets++;
            frame_done = handle_packet(pkt, n, fb);
        }

        if (frame_done) {
            frames++;
            if (!got_any) {
                got_any = true;
                SDL_Log("receiving: %dx%d", fb->width, fb->height);
                if (windowed)
                    SDL_SetWindowTitle(win, "c64uv");
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
                SDL_SetRenderLogicalPresentation(
                    ren, fb->width, fb->height,
                    SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
                tex_h = fb->height;
            }
            SDL_UpdateTexture(tex, NULL, fb->px, MAX_W * sizeof(Uint32));
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        if (cfg.verbose && SDL_GetTicks() - last_stat >= 5000) {
            SDL_Log("%.1f fps, %ld pkts in last %llus", frames / 5.0, packets,
                    (unsigned long long)(SDL_GetTicks() - last_stat) / 1000);
            frames = packets = 0;
            last_stat = SDL_GetTicks();
        }
    }

    atomic_store(&g_quit, true);
    if (ka)
        SDL_WaitThread(ka, NULL);
    curl_global_cleanup();
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
