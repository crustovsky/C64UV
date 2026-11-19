// Minimal VT100 terminal for the Ultimate's telnet remote menu (port 23).
// Implements exactly the sequence repertoire of the firmware's
// screen_vt100.cc / keyboard_vt100.cc - not a general-purpose terminal.
#ifndef TERM_H
#define TERM_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

// The firmware's remote screen is fixed 60x24 (screen_vt100.h get_size_x/y).
#define TERM_COLS 60
#define TERM_ROWS 24
#define TERM_PX_W (TERM_COLS * 8)
#define TERM_PX_H (TERM_ROWS * 8)

struct term {
    char ch[TERM_ROWS][TERM_COLS];
    uint8_t fg[TERM_ROWS][TERM_COLS]; // VIC color index 0-15
    uint8_t rv[TERM_ROWS][TERM_COLS]; // reverse video flag
    int cx, cy;
    uint8_t cur_fg;   // VIC color
    int ansi_base;    // last SGR base color 0-7
    int ansi_int;     // 0 normal, 1 bright, 2 dim
    bool cur_rv;
    bool draw_charset; // ESC(0 line drawing active
    bool dirty;
    // parser state
    int state; // 0 idle, 1 esc, 2 csi, 3 charset, 4 iac, 5 iac-opt, 6 iac-sub
    int params[8], nparams;
};

void term_init(struct term *t);
void term_feed(struct term *t, const uint8_t *data, int n);
// Renders the full grid into a TERM_PX_W x TERM_PX_H ARGB8888 buffer.
void term_render(const struct term *t, uint32_t *px, int pitch_px);
// Draws s with the 8x8 font at pixel (x, y) into an ARGB8888 buffer,
// foreground pixels only. Lets other views (help overlay) use the font
// without a free line pitch being forced into the 24-row grid.
void term_draw_text(uint32_t *px, int pitch_px, int x, int y, uint32_t color,
                    const char *s);
// ---- The Ultimate menu as GET /v1/machine:menu_screen returns it
// (firmware 1.2+): 40x25 character bytes, then 40x25 colour bytes. The
// characters are ASCII (0x20-0x7E), codes below 0x20 are the firmware's
// frame, marker and logo shapes, bit 7 of a character is reverse video.
// The colour byte holds two VIC colours: low nibble foreground, high
// nibble background (0, black, nearly everywhere). The selected row has
// bit 7 set in both bytes (0xE1 on 1.2.1: reversed white on light blue);
// headings are reversed too, but with a plain colour byte.
#define MENU_COLS 40
#define MENU_ROWS 25
#define MENU_BYTES (MENU_COLS * MENU_ROWS * 2)
#define MENU_PX_W (MENU_COLS * 8)
#define MENU_PX_H (MENU_ROWS * 8)

// Renders a MENU_BYTES payload into a MENU_PX_W x MENU_PX_H ARGB8888 buffer.
void menu_render(const uint8_t *scr, uint32_t *px, int pitch_px);
// One row as plain text: frame shapes become + - |, the block and marker
// shapes # and *, shapes this code has no drawing for (the logo pieces) a
// space. out takes MENU_COLS + 1 bytes.
void menu_row_text(const uint8_t *scr, int row, char *out);
// True when the row is the selected one (a cell with bit 7 in both its
// character and its colour byte).
bool menu_row_selected(const uint8_t *scr, int row);

// Encodes an SDL key for the firmware's VT100 keyboard parser.
// Returns bytes written to out (cap >= 8), 0 if the key isn't mapped.
int term_encode_key(SDL_Keycode key, SDL_Keymod mod, uint8_t *out);

#endif
