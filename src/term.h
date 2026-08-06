// Minimal VT100 terminal for the Ultimate's telnet remote menu (port 23).
// Implements exactly the sequence repertoire of the firmware's
// screen_vt100.cc / keyboard_vt100.cc — not a general-purpose terminal.
#ifndef TERM_H
#define TERM_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define TERM_COLS 80
#define TERM_ROWS 25
#define TERM_PX_W (TERM_COLS * 8)
#define TERM_PX_H (TERM_ROWS * 8)

struct term {
    char ch[TERM_ROWS][TERM_COLS];
    uint8_t fg[TERM_ROWS][TERM_COLS]; // ANSI color index 0-15
    uint8_t rv[TERM_ROWS][TERM_COLS]; // reverse video flag
    int cx, cy;
    uint8_t cur_fg;
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
// Encodes an SDL key for the firmware's VT100 keyboard parser.
// Returns bytes written to out (cap >= 8), 0 if the key isn't mapped.
int term_encode_key(SDL_Keycode key, SDL_Keymod mod, uint8_t *out);

#endif
