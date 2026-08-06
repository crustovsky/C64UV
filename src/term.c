#include "term.h"
#include "font8x8.h"

#include <string.h>

// VIC-II palette (Pepto), ARGB8888 - the menu should look like a C64, not a
// generic terminal.
static const uint32_t vic_palette[16] = {
    0xFF000000, 0xFFFFFFFF, 0xFF813338, 0xFF75CEC8, 0xFF8E3C97, 0xFF56AC4D,
    0xFF2E2C9B, 0xFFEDF171, 0xFF8E5029, 0xFF553800, 0xFFC46C71, 0xFF4A4A4A,
    0xFF7B7B7B, 0xFFA9FF9F, 0xFF706DEB, 0xFFB2B2B2,
};
#define TERM_BG vic_palette[6] // C64 blue

// The firmware maps VIC colors to ANSI (screen_vt100.cc set_color); this is
// that table inverted: [intensity][ansi base 30-37] -> VIC color.
static const uint8_t vic_from_ansi[3][8] = {
    {0, 2, 5, 7, 6, 4, 3, 15},  // normal
    {0, 10, 13, 7, 14, 4, 3, 1}, // ;1 bright
    {0, 9, 5, 8, 6, 4, 3, 15},  // ;2 dim
};

// DEC special graphics (ESC(0): map the glyphs the firmware uses to
// approximations present in the 8x8 ASCII font.
static char dec_graphics(char c)
{
    switch (c) {
    case 'q': return '-';  // horizontal line
    case 'x': return '|';  // vertical line
    case 'l': case 'k': case 'm': case 'j': return '+'; // corners
    case 'w': case 't': case 'u': case 'v': case 'n': return '+'; // tees
    case 's': case 'o': return '-'; // scan lines
    case '`': return '*';  // diamond
    case 'a': return '#';  // checkerboard
    }
    return c;
}

void term_init(struct term *t)
{
    memset(t, 0, sizeof *t);
    t->cur_fg = 15; // light grey, the firmware's default
    t->ansi_base = 7;
    memset(t->ch, ' ', sizeof t->ch);
    t->dirty = true;
}

static void put_char(struct term *t, char c)
{
    if (t->cx >= TERM_COLS) { // wrap
        t->cx = 0;
        t->cy++;
    }
    if (t->cy >= TERM_ROWS) { // scroll up
        memmove(t->ch[0], t->ch[1], sizeof t->ch[0] * (TERM_ROWS - 1));
        memmove(t->fg[0], t->fg[1], sizeof t->fg[0] * (TERM_ROWS - 1));
        memmove(t->rv[0], t->rv[1], sizeof t->rv[0] * (TERM_ROWS - 1));
        memset(t->ch[TERM_ROWS - 1], ' ', TERM_COLS);
        memset(t->fg[TERM_ROWS - 1], 0, TERM_COLS);
        memset(t->rv[TERM_ROWS - 1], 0, TERM_COLS);
        t->cy = TERM_ROWS - 1;
    }
    t->ch[t->cy][t->cx] = c;
    t->fg[t->cy][t->cx] = t->cur_fg;
    t->rv[t->cy][t->cx] = t->cur_rv;
    t->cx++;
}

static void clear_screen(struct term *t)
{
    memset(t->ch, ' ', sizeof t->ch);
    memset(t->fg, 0, sizeof t->fg);
    memset(t->rv, 0, sizeof t->rv);
    t->cx = t->cy = 0;
    t->cur_rv = false;
    t->draw_charset = false;
}

// SGR: the firmware only emits 0;3X with optional ;1 (bright) ;2 (dim),
// plus standalone 7/27 for reverse. Tracked as base+intensity so the
// sequence can be mapped back to the VIC color it stands for.
static void apply_sgr(struct term *t)
{
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->ansi_base = 7;
            t->ansi_int = 0;
            t->cur_rv = false;
        } else if (p == 1)
            t->ansi_int = 1;
        else if (p == 2)
            t->ansi_int = 2;
        else if (p == 7)
            t->cur_rv = true;
        else if (p == 27)
            t->cur_rv = false;
        else if (p >= 30 && p <= 37)
            t->ansi_base = p - 30;
    }
    t->cur_fg = vic_from_ansi[t->ansi_int][t->ansi_base];
}

static void csi_dispatch(struct term *t, char final)
{
    int p0 = t->nparams > 0 ? t->params[0] : 0;
    int p1 = t->nparams > 1 ? t->params[1] : 0;
    switch (final) {
    case 'H': // cursor position (1-based row;col)
    case 'f':
        t->cy = (p0 ? p0 : 1) - 1;
        t->cx = (p1 ? p1 : 1) - 1;
        if (t->cy < 0) t->cy = 0;
        if (t->cy >= TERM_ROWS) t->cy = TERM_ROWS - 1;
        if (t->cx < 0) t->cx = 0;
        if (t->cx >= TERM_COLS) t->cx = TERM_COLS - 1;
        break;
    case 'm':
        apply_sgr(t);
        break;
    case 'J':
        if (p0 == 2)
            clear_screen(t);
        break;
    case 'K': // erase to end of line
        for (int x = t->cx; x < TERM_COLS; x++) {
            t->ch[t->cy][x] = ' ';
            t->rv[t->cy][x] = 0;
        }
        break;
    case 'C': // cursor forward
        t->cx += p0 ? p0 : 1;
        if (t->cx >= TERM_COLS)
            t->cx = TERM_COLS - 1;
        break;
    case 'A': t->cy -= p0 ? p0 : 1; if (t->cy < 0) t->cy = 0; break;
    case 'B': t->cy += p0 ? p0 : 1; if (t->cy >= TERM_ROWS) t->cy = TERM_ROWS - 1; break;
    case 'D': t->cx -= p0 ? p0 : 1; if (t->cx < 0) t->cx = 0; break;
    case 'r': // scroll region - menu resets it; we don't implement regions
        break;
    }
}

void term_feed(struct term *t, const uint8_t *data, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t b = data[i];
        switch (t->state) {
        case 4: // after IAC
            if (b == 0xFF) { t->state = 0; } // escaped 0xFF: drop (not text)
            else if (b == 0xFA) t->state = 6; // subnegotiation
            else if (b >= 0xFB) t->state = 5; // WILL/WONT/DO/DONT + option
            else t->state = 0;
            continue;
        case 5: t->state = 0; continue; // option byte, ignored
        case 6: if (b == 0xF0) t->state = 0; continue; // eat until IAC SE
        }
        if (b == 0xFF) { t->state = 4; continue; }

        switch (t->state) {
        case 0:
            if (b == 0x1B) {
                t->state = 1;
            } else if (b == '\r') {
                t->cx = 0;
            } else if (b == '\n') {
                if (++t->cy >= TERM_ROWS) t->cy = TERM_ROWS - 1;
            } else if (b == 0x08) {
                if (t->cx > 0) t->cx--;
            } else if (b >= 32) {
                char c = (char)(b < 128 ? b : '?');
                if (t->draw_charset)
                    c = dec_graphics(c);
                put_char(t, c);
            }
            break;
        case 1: // after ESC
            if (b == '[') {
                t->state = 2;
                t->nparams = 0;
                memset(t->params, 0, sizeof t->params);
            } else if (b == '(') {
                t->state = 3;
            } else if (b == 'c') { // RIS: full reset
                clear_screen(t);
                t->cur_fg = 15;
                t->ansi_base = 7;
                t->ansi_int = 0;
                t->state = 0;
            } else {
                t->state = 0;
            }
            break;
        case 2: // CSI params
            if (b >= '0' && b <= '9') {
                if (t->nparams == 0)
                    t->nparams = 1;
                int *p = &t->params[t->nparams - 1];
                *p = *p * 10 + (b - '0');
            } else if (b == ';') {
                if (t->nparams < 8)
                    t->nparams++;
                if (t->nparams == 1) t->nparams = 2; // ";x" implies empty first
            } else if (b >= 0x40 && b <= 0x7E) {
                csi_dispatch(t, (char)b);
                t->state = 0;
            } else if (b != '?') { // ignore private markers, abort on junk
                t->state = 0;
            }
            break;
        case 3: // charset select: ESC(0 line drawing, ESC(B ASCII
            t->draw_charset = (b == '0');
            t->state = 0;
            break;
        }
    }
    t->dirty = true;
}

void term_render(const struct term *t, uint32_t *px, int pitch_px)
{
    for (int row = 0; row < TERM_ROWS; row++) {
        for (int col = 0; col < TERM_COLS; col++) {
            const char *glyph = font8x8_basic[(int)(unsigned char)t->ch[row][col] & 0x7F];
            bool cursor = (row == t->cy && col == t->cx);
            bool rv = t->rv[row][col] ^ cursor;
            uint32_t fgc = vic_palette[t->fg[row][col]];
            uint32_t bgc = TERM_BG;
            if (rv) { uint32_t tmp = fgc; fgc = bgc; bgc = tmp; }
            for (int y = 0; y < 8; y++) {
                uint32_t *out = px + (row * 8 + y) * pitch_px + col * 8;
                uint8_t bits = (uint8_t)glyph[y]; // LSB = leftmost pixel
                for (int x = 0; x < 8; x++)
                    out[x] = (bits >> x & 1) ? fgc : bgc;
            }
        }
    }
}

int term_encode_key(SDL_Keycode key, SDL_Keymod mod, uint8_t *out)
{
    (void)mod;
    const char *s = NULL;
    switch (key) {
    case SDLK_UP:    s = "\x1b[A"; break;
    case SDLK_DOWN:  s = "\x1b[B"; break;
    case SDLK_RIGHT: s = "\x1b[C"; break;
    case SDLK_LEFT:  s = "\x1b[D"; break;
    case SDLK_HOME:  s = "\x1b[1~"; break;
    case SDLK_INSERT: s = "\x1b[2~"; break;
    case SDLK_END:   s = "\x1b[4~"; break;
    case SDLK_PAGEUP: s = "\x1b[5~"; break;
    case SDLK_PAGEDOWN: s = "\x1b[6~"; break;
    case SDLK_F1: s = "\x1b[11~"; break;
    case SDLK_F2: s = "\x1b[12~"; break;
    case SDLK_F3: s = "\x1b[13~"; break;
    case SDLK_F4: s = "\x1b[14~"; break;
    case SDLK_F5: s = "\x1b[15~"; break;
    case SDLK_F6: s = "\x1b[17~"; break;
    case SDLK_F7: s = "\x1b[18~"; break;
    case SDLK_F8: s = "\x1b[19~"; break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: s = "\r"; break;
    case SDLK_BACKSPACE: s = "\x08"; break;
    case SDLK_ESCAPE: s = "\x1b"; break; // bare ESC = back in the menu
    default: return 0;
    }
    int n = (int)strlen(s);
    memcpy(out, s, (size_t)n);
    return n;
}
