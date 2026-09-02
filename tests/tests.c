// Unit tests for the hardware-independent parts: VT100 terminal parsing,
// VIC frame assembly, and PETSCII key mapping. No device needed; run with
// `make test`.
#define _POSIX_C_SOURCE 200809L // setenv with -std=c11 (test-only)

#include "../src/compat.h"
#include "../src/discover.h"
#include "../src/keys.h"
#include "../src/term.h"
#include "../src/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

static void feed_str(struct term *t, const char *s)
{
    term_feed(t, (const uint8_t *)s, (int)strlen(s));
}

static void test_term_basics(void)
{
    struct term t;
    term_init(&t);
    CHECK(t.cur_fg == 15); // firmware default: light grey

    // telnet negotiation + reset, as sent on connect
    const uint8_t hello[] = {0xFF, 0xFE, 0x22, 0xFF, 0xFB, 0x01, 0x1B, 'c'};
    term_feed(&t, hello, sizeof hello);
    CHECK(t.ch[0][0] == ' '); // IAC bytes must not print

    feed_str(&t, "\x1b[3;5HHELLO");
    CHECK(memcmp(&t.ch[2][4], "HELLO", 5) == 0);
    CHECK(t.cx == 9 && t.cy == 2);

    // firmware color table: 0;31;1 = VIC pink (10), 0;37;2 = light grey (15)
    feed_str(&t, "\x1b[0;31;1mX");
    CHECK(t.fg[2][9] == 10);
    feed_str(&t, "\x1b[0;37;2mY");
    CHECK(t.fg[2][10] == 15);
    feed_str(&t, "\x1b[0;30mZ");
    CHECK(t.fg[2][11] == 0); // black

    // reverse video marks cells until 27m
    feed_str(&t, "\x1b[7mR\x1b[27mN");
    CHECK(t.rv[2][12] == 1 && t.rv[2][13] == 0);

    // DEC line drawing: q -> '-', x -> '|', corners -> '+'
    feed_str(&t, "\x1b[10;1H\x1b(0lqk\x1b(Bab");
    CHECK(t.ch[9][0] == '+' && t.ch[9][1] == '-' && t.ch[9][2] == '+');
    CHECK(t.ch[9][3] == 'a' && t.ch[9][4] == 'b');

    // ESC[2J clears, cursor home
    feed_str(&t, "\x1b[2J");
    CHECK(t.ch[2][4] == ' ' && t.cx == 0 && t.cy == 0);
}

static void test_term_geometry(void)
{
    struct term t;
    term_init(&t);
    // clamp positioning to the 60x24 firmware screen
    feed_str(&t, "\x1b[99;99HX");
    CHECK(t.ch[TERM_ROWS - 1][TERM_COLS - 1] == 'X');
    // wrap: write at end of line, next char goes to col 0 below
    feed_str(&t, "\x1b[5;60HAB");
    CHECK(t.ch[4][59] == 'A' && t.ch[5][0] == 'B');
    // CR/LF handling
    feed_str(&t, "\x1b[7;10Hfoo\r\nbar");
    CHECK(t.ch[6][9] == 'f' && t.ch[7][0] == 'b');
}

static void test_term_keys(void)
{
    uint8_t buf[8];
    int n = term_encode_key(SDLK_F1, 0, buf);
    CHECK(n == 5 && memcmp(buf, "\x1b[11~", 5) == 0);
    n = term_encode_key(SDLK_F8, 0, buf);
    CHECK(n == 5 && memcmp(buf, "\x1b[19~", 5) == 0);
    n = term_encode_key(SDLK_UP, 0, buf);
    CHECK(n == 3 && memcmp(buf, "\x1b[A", 3) == 0);
    n = term_encode_key(SDLK_ESCAPE, 0, buf);
    CHECK(n == 1 && buf[0] == 0x1B);
    CHECK(term_encode_key(SDLK_A, 0, buf) == 0); // printables use text input
}

// Builds one video packet: 4 lines of 384 px, every pixel = `color`, except
// pixel 0 of the first line which is `first_px` (to test nibble order).
static int mk_packet(uint8_t *p, uint16_t frame, uint16_t line, bool last,
                     uint8_t color, uint8_t first_px)
{
    uint16_t lf = line | (last ? 0x8000 : 0);
    uint8_t hdr[VIDEO_HDR_LEN] = {0, 0, (uint8_t)(frame & 0xFF),
                                  (uint8_t)(frame >> 8), (uint8_t)(lf & 0xFF),
                                  (uint8_t)(lf >> 8), 384 & 0xFF, 384 >> 8,
                                  4, 4, 0, 0};
    memcpy(p, hdr, sizeof hdr);
    memset(p + VIDEO_HDR_LEN, (color << 4) | color, 768);
    p[VIDEO_HDR_LEN] = (uint8_t)((color << 4) | first_px); // low nibble = left
    return VIDEO_HDR_LEN + 768;
}

static void test_video_assembly(void)
{
    static struct frame_buf fb;
    static uint8_t p[1024];
    video_init(&fb);

    // full PAL frame of color 6 (blue), first pixel white (1)
    bool done = false;
    for (int line = 0; line < 272; line += 4) {
        int n = mk_packet(p, 7, (uint16_t)line, line == 268, 6, line == 0 ? 1 : 6);
        done = video_handle_packet(p, n, &fb);
    }
    CHECK(done && fb.ready && fb.complete);
    CHECK(fb.width == 384 && fb.height == 272);
    CHECK(fb.px[0] == vic_palette[1]);   // low nibble was pixel 0
    CHECK(fb.px[1] == vic_palette[6]);   // high nibble pixel 1
    CHECK(fb.px[271 * VIDEO_MAX_W + 383] == vic_palette[6]);

    // next frame with a lost packet: ready but not complete
    for (int line = 0; line < 272; line += 4) {
        if (line == 100)
            continue; // dropped
        int n = mk_packet(p, 8, (uint16_t)line, line == 268, 3, 3);
        video_handle_packet(p, n, &fb);
    }
    CHECK(fb.ready && !fb.complete);
    CHECK(fb.px[0] == vic_palette[3]);
    CHECK(fb.px[100 * VIDEO_MAX_W] == vic_palette[6]); // stale line kept

    // garbage must be rejected
    CHECK(!video_handle_packet(p, 5, &fb));         // too short
    p[9] = 8;                                       // bpp != 4
    CHECK(!video_handle_packet(p, 780, &fb));
}

static void test_petscii(void)
{
    CHECK(ascii_to_petscii('a') == 0x41);
    CHECK(ascii_to_petscii('z') == 0x5A);
    CHECK(ascii_to_petscii('A') == 0xC1);
    CHECK(ascii_to_petscii('5') == '5');
    CHECK(ascii_to_petscii('"') == '"');
    CHECK(ascii_to_petscii('~') == -1); // no such key
    CHECK(special_to_petscii(SDLK_RETURN, 0) == 0x0D);
    CHECK(special_to_petscii(SDLK_UP, 0) == 0x91);
    CHECK(special_to_petscii(SDLK_F1, 0) == 0x85);
    CHECK(special_to_petscii(SDLK_F2, 0) == 0x89); // F2 = shift+F1
    CHECK(special_to_petscii(SDLK_HOME, 0) == 0x13);
    CHECK(special_to_petscii(SDLK_HOME, SDL_KMOD_SHIFT) == 0x93); // CLR
    CHECK(special_to_petscii(SDLK_A, 0) == -1); // printables via text input
}

static void test_matrix_keys(void)
{
    const char *n[2];
    CHECK(key_to_c64_matrix(SDLK_A, n) == 1 && !strcmp(n[0], "a"));
    CHECK(key_to_c64_matrix(SDLK_9, n) == 1 && !strcmp(n[0], "9"));
    CHECK(key_to_c64_matrix(SDLK_SPACE, n) == 1 && !strcmp(n[0], "space"));
    CHECK(key_to_c64_matrix(SDLK_UP, n) == 2 &&
          !strcmp(n[0], "right_shift") && !strcmp(n[1], "cursor_up_down"));
    CHECK(key_to_c64_matrix(SDLK_DOWN, n) == 1 &&
          !strcmp(n[0], "cursor_up_down"));
    CHECK(key_to_c64_matrix(SDLK_F2, n) == 2 &&
          !strcmp(n[0], "left_shift") && !strcmp(n[1], "f1"));
    CHECK(key_to_c64_matrix(SDLK_ESCAPE, n) == 1 && !strcmp(n[0], "run_stop"));
    CHECK(key_to_c64_matrix(SDLK_PAGEUP, n) == 1 && !strcmp(n[0], "restore"));
    // + and £ have no unshifted PC key: nav cluster and numeric keypad
    CHECK(key_to_c64_matrix(SDLK_END, n) == 1 && !strcmp(n[0], "plus"));
    CHECK(key_to_c64_matrix(SDLK_KP_PLUS, n) == 1 && !strcmp(n[0], "plus"));
    CHECK(key_to_c64_matrix(SDLK_INSERT, n) == 1 && !strcmp(n[0], "pound"));
    CHECK(key_to_c64_matrix(SDLK_KP_MULTIPLY, n) == 1 && !strcmp(n[0], "star"));
    CHECK(key_to_c64_matrix(SDLK_KP_7, n) == 1 && !strcmp(n[0], "7"));
    CHECK(key_to_c64_matrix(SDLK_F9, n) == 0);   // viewer keys stay local
    CHECK(key_to_c64_matrix(SDLK_LCTRL, n) == 0); // reserved for hotkeys

    char buf[192];
    const char *one[] = {"a"};
    CHECK(matrix_event_json(one, 1, "press", buf, sizeof buf));
    CHECK(!strcmp(buf, "{\"events\":[{\"kind\":\"keyboard\",\"inputs\":"
                       "[\"a\"],\"transition\":\"press\"}]}"));
    const char *two[] = {"left_shift", "f1"};
    CHECK(matrix_event_json(two, 2, "tap", buf, sizeof buf));
    CHECK(strstr(buf, "\"inputs\":[\"left_shift\",\"f1\"]") != NULL);
    CHECK(strstr(buf, "\"transition\":\"tap\"") != NULL);
    CHECK(!matrix_event_json(one, 1, "press", buf, 16)); // too small
}

static void test_bindings(void)
{
    // dispatch side of the shared table
    CHECK(viewer_binding_match(SDLK_Q, SDL_KMOD_LCTRL) == VA_QUIT);
    CHECK(viewer_binding_match(SDLK_R, SDL_KMOD_LCTRL) == VA_RESET);
    CHECK(viewer_binding_match(SDLK_R, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) ==
          VA_REBOOT);
    CHECK(viewer_binding_match(SDLK_R, 0) == VA_NONE); // plain R types
    CHECK(viewer_binding_match(SDLK_F9, 0) == VA_MENU_VIEW);
    CHECK(viewer_binding_match(SDLK_F10, 0) == VA_HELP);
    CHECK(viewer_binding_match(SDLK_F10, SDL_KMOD_NUM) == VA_HELP); // stray mod
    CHECK(viewer_binding_match(SDLK_M, SDL_KMOD_RCTRL) == VA_MENU_BTN);

    // table integrity: every row renders in help, no dispatch collisions
    for (int i = 0; i < viewer_bindings_count; i++) {
        const struct viewer_binding *b = &viewer_bindings[i];
        CHECK(b->label && b->label[0] && b->desc && b->desc[0]);
        CHECK(b->action != VA_NONE);
        CHECK((b->action == VA_INFO) == (b->key == 0));
        for (int j = i + 1; j < viewer_bindings_count; j++)
            if (b->action != VA_INFO && viewer_bindings[j].action != VA_INFO)
                CHECK(b->key != viewer_bindings[j].key ||
                      b->mod != viewer_bindings[j].mod);
    }
}

static void test_json(void)
{
    // shaped like a real /v1/info response
    const char *info = "{\"product\": \"Ultimate 64\", \"firmware_version\": "
                       "\"3.12\",\n \"hostname\": \"c64\", \"errors\": []}";
    char v[64];
    CHECK(json_find_str(info, "product", v, sizeof v) &&
          !strcmp(v, "Ultimate 64"));
    CHECK(json_find_str(info, "hostname", v, sizeof v) && !strcmp(v, "c64"));
    CHECK(!json_find_str(info, "missing", v, sizeof v));
    CHECK(!json_find_str(info, "errors", v, sizeof v)); // not a string value
    // truncation to cap, still NUL-terminated
    CHECK(json_find_str(info, "product", v, 4) && !strcmp(v, "Ult"));
    // escaped quote inside a value must not end the string early
    CHECK(json_find_str("{\"a\": \"x\\\"y\"}", "a", v, sizeof v) &&
          !strcmp(v, "x\"y"));
}

static void test_arp_wired(void)
{
    const char *path = "/tmp/c64uv-test-arp";
    FILE *f = fopen(path, "w");
    fputs("IP address       HW type     Flags       HW address            "
          "Mask     Device\n"
          "192.168.8.173    0x1         0x2         9c:13:9e:ef:14:d0     "
          "*        eth0\n"
          "192.168.8.236    0x1         0x2         02:15:41:79:9d:c6     "
          "*        eth0\n", f);
    fclose(f);
    setenv("C64U_ARP_TABLE", path, 1);
    char mac[COMPAT_MAC_STRLEN];
    CHECK(compat_neighbor_mac("192.168.8.173", mac, sizeof mac) &&
          !strcmp(mac, "9c:13:9e:ef:14:d0"));
    CHECK(discover_ip_is_wired("192.168.8.236"));
    CHECK(!discover_ip_is_wired("192.168.8.173")); // ESP32 WiFi side
    CHECK(!discover_ip_is_wired("192.168.8.99"));  // not in the table
    setenv("C64U_ARP_TABLE", "/nonexistent", 1);
    CHECK(!discover_ip_is_wired("192.168.8.236"));
    unsetenv("C64U_ARP_TABLE");
    remove(path);
}

static void test_compat(void)
{
    uint32_t a = 0;
    char s[COMPAT_IP_STRLEN];
    CHECK(compat_ipv4_parse("192.168.8.236", &a) && a == 0xC0A808ECu);
    CHECK(!strcmp(compat_ipv4_format(a, s, sizeof s), "192.168.8.236"));
    CHECK(!strcmp(compat_ipv4_format(a + 1, s, sizeof s), "192.168.8.237"));
    CHECK(!compat_ipv4_parse("c64-ultimate", &a)); // hostnames are refused
    CHECK(!compat_ipv4_parse("192.168.8", &a));
    CHECK(!compat_ipv4_parse("192.168.8.256", &a));
    CHECK(compat_ipv4_parse("239.0.1.64", &a) && compat_ipv4_is_multicast(a));
    CHECK(compat_ipv4_parse("10.0.0.1", &a) && !compat_ipv4_is_multicast(a));

    // sockets: bind an ephemeral UDP port, loop a datagram back, and make
    // sure the non-blocking receive and the readiness wait agree
    CHECK(compat_net_init());
    compat_sock rx = compat_udp_bind(0, 65536, false);
    compat_sock tx = compat_udp_bind(0, 65536, false);
    CHECK(rx != COMPAT_BAD_SOCK && tx != COMPAT_BAD_SOCK);
    char local[COMPAT_IP_STRLEN];
    CHECK(compat_route_source_ip("127.0.0.1", local, sizeof local) &&
          !strcmp(local, "127.0.0.1"));
    char buf[16];
    CHECK(compat_recv_nowait(rx, buf, sizeof buf) < 0); // nothing yet
    CHECK(compat_wait_readable(&rx, 1, 0) == 0);
    // the bound port is the one detail the layer has no getter for, so the
    // loopback test uses a fixed high port for the receiver instead
    compat_close(rx);
    rx = compat_udp_bind(21099, 65536, true);
    CHECK(rx != COMPAT_BAD_SOCK);
    CHECK(compat_sendto(tx, "hi", 2, "127.0.0.1", 21099) == 2);
    CHECK(compat_sendto(tx, "hi", 2, "nowhere", 21099) < 0);
    compat_sock set[3] = {COMPAT_BAD_SOCK, rx, COMPAT_BAD_SOCK};
    CHECK(compat_wait_readable(set, 3, 1000) == 1);
    CHECK(compat_recv_nowait(rx, buf, sizeof buf) == 2 && buf[0] == 'h');
    CHECK(compat_recv_nowait(rx, buf, sizeof buf) < 0);
    // a duplicate bind without reuse fails and reports why
    CHECK(compat_udp_bind(21099, 65536, false) == COMPAT_BAD_SOCK);
    CHECK(compat_neterr()[0] != '\0');
    // TCP to a closed port fails cleanly instead of hanging
    CHECK(compat_tcp_connect("127.0.0.1", 21098, 1) == COMPAT_BAD_SOCK);
    compat_close(rx);
    compat_close(tx);
    compat_close(COMPAT_BAD_SOCK); // must be a no-op

    // interface enumeration: loopback is always there and flagged
    struct compat_iface ifs[32];
    int n = compat_ifaces(ifs, 32);
    bool lo = false;
    for (int i = 0; i < n; i++)
        lo |= ifs[i].loopback && (ifs[i].addr >> 24) == 127;
    CHECK(lo);
    compat_net_quit();
}

int main(void)
{
    test_compat();
    test_term_basics();
    test_term_geometry();
    test_term_keys();
    test_video_assembly();
    test_petscii();
    test_matrix_keys();
    test_bindings();
    test_json();
    test_arp_wired();
    if (failures) {
        fprintf(stderr, "%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
