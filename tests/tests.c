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

static void test_menu_screen(void)
{
    // a payload the way the firmware lays it out: characters, then colours
    static uint8_t scr[MENU_BYTES];
    memset(scr, ' ', MENU_COLS * MENU_ROWS);
    memset(scr + MENU_COLS * MENU_ROWS, 0x0F, MENU_COLS * MENU_ROWS);
    scr[0] = 0x01; // top-left corner, edge, top-right corner
    memset(scr + 1, 0x02, MENU_COLS - 2);
    scr[MENU_COLS - 1] = 0x03;
    scr[MENU_COLS] = 0x04;
    memcpy(scr + MENU_COLS + 2, "Disk File Browser", 17);
    // row 2 is the selected one: bit 7 on characters and colours, white
    // on light blue the way firmware 1.2.1 sends it (0xE1)
    const char *sel = "Run Disk";
    for (int i = 0; i < MENU_COLS; i++) {
        uint8_t c = i >= 2 && i < 10 ? (uint8_t)sel[i - 2] : ' ';
        scr[2 * MENU_COLS + i] = c | 0x80;
        scr[MENU_COLS * MENU_ROWS + 2 * MENU_COLS + i] = 0xE1;
    }
    scr[5 * MENU_COLS + 2] = 'X' | 0x80; // a reversed heading cell: plain
    scr[5 * MENU_COLS + 3] = 0x1C;       // colour byte; and a logo piece
    scr[3 * MENU_COLS] = 0x0B; // progress block
    scr[3 * MENU_COLS + 1] = 0x13; // marker

    char line[MENU_COLS + 1];
    menu_row_text(scr, 0, line);
    CHECK(line[0] == '+' && line[1] == '-' && line[MENU_COLS - 1] == '+');
    CHECK(strlen(line) == MENU_COLS);
    menu_row_text(scr, 1, line);
    CHECK(line[0] == '|' && !strncmp(line + 2, "Disk File Browser", 17));
    menu_row_text(scr, 2, line);
    CHECK(!strncmp(line, "  Run Disk ", 11)); // bit 7 stripped, case kept
    menu_row_text(scr, 3, line);
    CHECK(line[0] == '#' && line[1] == '*');
    CHECK(menu_row_selected(scr, 2) && !menu_row_selected(scr, 1));
    CHECK(!menu_row_selected(scr, 5)); // reverse alone is a heading
    menu_row_text(scr, 5, line);
    CHECK(line[2] == 'X' && line[3] == ' '); // unknown shape: a blank

    static uint32_t px[MENU_PX_W * MENU_PX_H];
    CHECK(menu_guess_bg(scr) == 14); // from the selected row's 0xE1
    menu_render(scr, 0, px, MENU_PX_W);
    const uint32_t black = 0xFF000000, white = 0xFFFFFFFF, grey = 0xFFB2B2B2;
    // the corner's arms: nothing top-left, the stroke runs right and down
    CHECK(px[0] == black);
    CHECK(px[3 * MENU_PX_W + 7] == grey && px[7 * MENU_PX_W + 3] == grey);
    CHECK(px[3 * MENU_PX_W + 0] == black && px[0 * MENU_PX_W + 3] == black);
    // edge joins the corner: the stroke is continuous across cells
    CHECK(px[3 * MENU_PX_W + 8] == grey && px[4 * MENU_PX_W + 15] == grey);
    // selected row: reverse video, so a blank cell is all foreground
    // (white) and the letters show in the background colour (light blue)
    CHECK(px[(2 * 8 + 1) * MENU_PX_W + 0] == white);
    // unselected blank cell stays black
    CHECK(px[(5 * 8 + 1) * MENU_PX_W + 0] == black);
    int lb = 0, wh = 0;
    for (int y = 16; y < 24; y++)
        for (int x = 16; x < 24; x++) { // the 'R' of "Run Disk"
            lb += px[y * MENU_PX_W + x] == 0xFF706DEB;
            wh += px[y * MENU_PX_W + x] == white;
        }
    CHECK(lb > 8 && lb + wh == 64);
    // on a light blue screen ("Commodore Blue") the empty cells are light
    // blue, the strokes keep their colour, the selected bar stays white
    menu_render(scr, 14, px, MENU_PX_W);
    CHECK(px[0] == 0xFF706DEB && px[(5 * 8 + 1) * MENU_PX_W] == 0xFF706DEB);
    CHECK(px[3 * MENU_PX_W + 7] == grey);
    CHECK(px[(2 * 8 + 1) * MENU_PX_W + 0] == white);
    CHECK(term_vic_color(6) == 0xFF2E2C9B);
    // progress block fills the cell below its first pixel row
    CHECK(px[(3 * 8 + 4) * MENU_PX_W + 4] == grey);
}

// A screen captured from a C64 Ultimate on firmware 1.2.1 (main menu just
// opened, first entry selected).
static void test_menu_screen_real(void)
{
    static uint8_t scr[MENU_BYTES];
    FILE *f = fopen("tests/menu_screen.bin", "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fread(scr, 1, MENU_BYTES, f) == MENU_BYTES);
    fclose(f);
    char line[MENU_COLS + 1];
    menu_row_text(scr, 0, line);
    CHECK(strstr(line, " COMMODORE 64 ") != NULL);
    menu_row_text(scr, 2, line);
    CHECK(!strcmp(line, "+--------------------------------------+"));
    menu_row_text(scr, 3, line);
    CHECK(!strncmp(line, "|DISK FILE BROWSER ", 19) && line[39] == '|');
    menu_row_text(scr, 24, line);
    CHECK(strstr(line, "F7=Help") != NULL); // mixed case survives
    int selected = 0;
    for (int row = 0; row < MENU_ROWS; row++)
        selected += menu_row_selected(scr, row);
    CHECK(selected == 1 && menu_row_selected(scr, 3));
    CHECK(menu_guess_bg(scr) == 14); // the machine was on "Commodore Blue"
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

static void test_matrix_typing(void)
{
    const char *n[2];
    CHECK(ascii_to_c64_matrix('a', n) == 1 && !strcmp(n[0], "a"));
    CHECK(ascii_to_c64_matrix('Q', n) == 1 && !strcmp(n[0], "q")); // unshifted
    CHECK(ascii_to_c64_matrix('7', n) == 1 && !strcmp(n[0], "7"));
    CHECK(ascii_to_c64_matrix('\n', n) == 1 && !strcmp(n[0], "return"));
    CHECK(ascii_to_c64_matrix(' ', n) == 1 && !strcmp(n[0], "space"));
    CHECK(ascii_to_c64_matrix('*', n) == 1 && !strcmp(n[0], "star"));
    CHECK(ascii_to_c64_matrix('"', n) == 2 && !strcmp(n[0], "left_shift") &&
          !strcmp(n[1], "2"));
    CHECK(ascii_to_c64_matrix('?', n) == 2 && !strcmp(n[1], "slash"));
    CHECK(ascii_to_c64_matrix('$', n) == 2 && !strcmp(n[1], "4"));
    CHECK(ascii_to_c64_matrix('~', n) == 0); // no such key
    CHECK(ascii_to_c64_matrix(0xE9, n) == 0);
    // every character a BASIC line needs resolves to valid API names
    for (const char *p = "LOAD\"*\",8,1:PRINT(A$+B$)<>3;?"; *p; p++) {
        int k = ascii_to_c64_matrix((unsigned char)*p, n);
        CHECK(k >= 1);
        for (int i = 0; i < k; i++)
            CHECK(matrix_key_valid(n[i]));
    }
    CHECK(matrix_key_valid("run_stop") && matrix_key_valid("f1") &&
          matrix_key_valid("z") && matrix_key_valid("0") &&
          matrix_key_valid("restore"));
    CHECK(!matrix_key_valid("f2") && !matrix_key_valid("enter") &&
          !matrix_key_valid("") && !matrix_key_valid("A") &&
          !matrix_key_valid("aa"));
}

static void test_joystick(void)
{
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_DPAD_LEFT) == JOY_LEFT);
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_SOUTH) == JOY_FIRE);
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_EAST) == JOY_UP); // jump
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_WEST) == JOY_FIRE2);
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_NORTH) == JOY_FIRE3);
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_START) == 0);
    CHECK(pad_button_joy(SDL_GAMEPAD_BUTTON_BACK) == 0); // port swap

    CHECK(pad_stick_joy(0, 0) == 0);
    CHECK(pad_stick_joy(8000, -8000) == 0); // dead zone
    CHECK(pad_stick_joy(-32768, 0) == JOY_LEFT);
    CHECK(pad_stick_joy(32767, 32767) == (JOY_RIGHT | JOY_DOWN));
    CHECK(pad_stick_joy(0, -20000) == JOY_UP);

    CHECK(mouse_joy_dir(0, 0, 2) == 0);
    CHECK(mouse_joy_dir(1, -1, 2) == 0); // dead zone
    CHECK(mouse_joy_dir(-3, 0, 2) == JOY_LEFT);
    CHECK(mouse_joy_dir(3, 0, 2) == JOY_RIGHT);
    CHECK(mouse_joy_dir(0, -3, 2) == JOY_UP); // screen y grows downwards
    CHECK(mouse_joy_dir(0, 3, 2) == JOY_DOWN);
    CHECK(mouse_joy_dir(3, -3, 2) == (JOY_RIGHT | JOY_UP));

    // in the menu view the pad steers the menu instead
    CHECK(pad_menu_key(SDL_GAMEPAD_BUTTON_DPAD_DOWN) == SDLK_DOWN);
    CHECK(pad_menu_key(SDL_GAMEPAD_BUTTON_SOUTH) == SDLK_RETURN);
    CHECK(pad_menu_key(SDL_GAMEPAD_BUTTON_EAST) == SDLK_LEFT); // back
    CHECK(pad_menu_key(SDL_GAMEPAD_BUTTON_START) == SDLK_UNKNOWN);

    CHECK(joy_parse("fire") == JOY_FIRE);
    CHECK(joy_parse("up,left,fire2") == (JOY_UP | JOY_LEFT | JOY_FIRE2));
    CHECK(joy_parse("fire,jump") == 0);
    CHECK(joy_parse("") == 0);
    CHECK(joy_parse("fir") == 0);

    char buf[320];
    CHECK(joy_event_json(2, 0, JOY_UP | JOY_FIRE, buf, sizeof buf));
    CHECK(!strcmp(buf, "{\"events\":[{\"kind\":\"joystick\",\"port\":2,"
                       "\"inputs\":[\"up\",\"fire\"],"
                       "\"transition\":\"press\"}]}"));
    // a direction change is one POST: release first, then press
    CHECK(joy_event_json(1, JOY_LEFT | JOY_FIRE, JOY_RIGHT | JOY_FIRE, buf,
                         sizeof buf));
    CHECK(!strcmp(buf, "{\"events\":[{\"kind\":\"joystick\",\"port\":1,"
                       "\"inputs\":[\"left\"],\"transition\":\"release\"},"
                       "{\"kind\":\"joystick\",\"port\":1,"
                       "\"inputs\":[\"right\"],\"transition\":\"press\"}]}"));
    CHECK(!joy_event_json(2, JOY_FIRE, JOY_FIRE, buf, sizeof buf)); // no-op
    CHECK(!joy_event_json(2, 0, 0x7f, buf, 40)); // too small
}

static void test_mouse(void)
{
    char b[512];
    struct mouse_btn_ev ev[2] = {{MOUSE_LEFT, true},
                                 {MOUSE_LEFT | MOUSE_RIGHT, false}};
    CHECK(mouse_event_json(5, -3, NULL, 0, b, sizeof b) &&
          !strcmp(b, "{\"events\":[{\"kind\":\"mouse\",\"move\":{\"x\":5,"
                     "\"y\":-3}}]}"));
    CHECK(mouse_event_json(0, 0, ev, 2, b, sizeof b) &&
          !strcmp(b, "{\"events\":[{\"kind\":\"mouse\",\"inputs\":[\"left\"],"
                     "\"transition\":\"press\"},{\"kind\":\"mouse\",\"inputs\":"
                     "[\"left\",\"right\"],\"transition\":\"release\"}]}"));
    CHECK(mouse_event_json(-127, 127, ev, 1, b, sizeof b) &&
          strstr(b, "\"move\":{\"x\":-127,\"y\":127}},{\"kind\":\"mouse\","
                    "\"inputs\":[\"left\"]"));
    CHECK(!mouse_event_json(0, 0, NULL, 0, b, sizeof b)); // nothing to send
    CHECK(!mouse_event_json(128, 0, NULL, 0, b, sizeof b)); // beyond a report
    CHECK(!mouse_event_json(5, 5, ev, 2, b, 40)); // too small

    CHECK(mouse_send_json("left", b, sizeof b) &&
          !strcmp(b, "{\"events\":[{\"kind\":\"mouse\",\"inputs\":[\"left\"],"
                     "\"transition\":\"tap\"}]}"));
    CHECK(mouse_send_json("right:down", b, sizeof b) &&
          strstr(b, "[\"right\"],\"transition\":\"press\""));
    CHECK(mouse_send_json("middle:up", b, sizeof b) &&
          strstr(b, "[\"middle\"],\"transition\":\"release\""));
    CHECK(mouse_send_json("move:40:-10", b, sizeof b) &&
          !strcmp(b, "{\"events\":[{\"kind\":\"mouse\",\"move\":{\"x\":40,"
                     "\"y\":-10}}]}"));
    // a long move becomes a path whose steps fit a report and add up
    CHECK(mouse_send_json("move:300:-10", b, sizeof b) &&
          !strcmp(b, "{\"events\":[{\"kind\":\"mouse\",\"path\":[[100,-3],"
                     "[100,-3],[100,-4]]}]}"));
    CHECK(mouse_send_json("wheel:-2", b, sizeof b) &&
          strstr(b, "\"wheel\":{\"vertical\":-2}"));
    CHECK(!mouse_send_json("move:0:0", b, sizeof b));
    CHECK(!mouse_send_json("move:5", b, sizeof b));
    CHECK(!mouse_send_json("move:5:x", b, sizeof b));
    CHECK(!mouse_send_json("move:9999:0", b, sizeof b));
    CHECK(!mouse_send_json("wheel:0", b, sizeof b));
    CHECK(!mouse_send_json("left:click", b, sizeof b));
    CHECK(!mouse_send_json("fire", b, sizeof b));
    CHECK(!mouse_send_json("", b, sizeof b));
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
    CHECK(viewer_binding_match(SDLK_G, SDL_KMOD_LCTRL) == VA_MOUSE);
    CHECK(viewer_binding_match(SDLK_G, 0) == VA_NONE); // plain G types

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
    CHECK(compat_neterr_transient()); // ...which is a would-block, not an error
    CHECK(compat_wait_readable(&rx, 1, 0) == 0);
    CHECK(compat_wait_writable(tx, 0) == 1); // an idle socket has room
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
    test_menu_screen();
    test_menu_screen_real();
    test_video_assembly();
    test_petscii();
    test_matrix_keys();
    test_matrix_typing();
    test_joystick();
    test_mouse();
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
