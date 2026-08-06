// Unit tests for the hardware-independent parts: VT100 terminal parsing,
// VIC frame assembly, and PETSCII key mapping. No device needed; run with
// `make test`.
#include "../src/keys.h"
#include "../src/term.h"
#include "../src/video.h"

#include <stdio.h>
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

int main(void)
{
    test_term_basics();
    test_term_geometry();
    test_term_keys();
    test_video_assembly();
    test_petscii();
    if (failures) {
        fprintf(stderr, "%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
