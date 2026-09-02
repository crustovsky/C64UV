#include "keys.h"

#include <stdio.h>

// Lowercase letters are the unshifted keys, uppercase are shifted (graphics
// symbols in the default charset) - same feel as sitting at the machine.
int ascii_to_petscii(unsigned char a)
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

// Single-name letter/digit keys; index 0-25 = a-z, 26-35 = 0-9.
static const char *const matrix_alnum[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};

int key_to_c64_matrix(SDL_Keycode key, const char *names[2])
{
    const char *one = NULL;
    if (key >= SDLK_A && key <= SDLK_Z)
        one = matrix_alnum[key - SDLK_A];
    else if (key >= SDLK_0 && key <= SDLK_9)
        one = matrix_alnum[26 + key - SDLK_0];
    else
        switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: one = "return"; break;
        case SDLK_BACKSPACE:
        case SDLK_DELETE: one = "inst_del"; break;
        case SDLK_SPACE: one = "space"; break;
        case SDLK_LSHIFT: one = "left_shift"; break;
        case SDLK_RSHIFT: one = "right_shift"; break;
        // C64 CTRL sits where Tab is; PC Ctrl stays free for viewer hotkeys
        case SDLK_TAB: one = "ctrl"; break;
        case SDLK_LALT:
        case SDLK_LGUI: one = "commodore"; break;
        case SDLK_ESCAPE: one = "run_stop"; break;
        case SDLK_HOME: one = "clr_home"; break;
        case SDLK_PAGEUP: one = "restore"; break; // tap-only
        // The C64's top row ends "+ - £ CLR/HOME INST/DEL"; a PC has no
        // unshifted + or £ key, so both live in the nav cluster next to
        // Home and Delete (Shift+= would reach the machine as shift+plus,
        // a graphic character, because Shift is a real held key there).
        case SDLK_INSERT: one = "pound"; break;
        case SDLK_END:
        case SDLK_KP_PLUS: one = "plus"; break;
        case SDLK_KP_MINUS: one = "minus"; break;
        case SDLK_KP_MULTIPLY: one = "star"; break;
        case SDLK_KP_DIVIDE: one = "slash"; break;
        case SDLK_KP_PERIOD: one = "period"; break;
        case SDLK_KP_0: one = "0"; break;
        case SDLK_KP_1: one = "1"; break;
        case SDLK_KP_2: one = "2"; break;
        case SDLK_KP_3: one = "3"; break;
        case SDLK_KP_4: one = "4"; break;
        case SDLK_KP_5: one = "5"; break;
        case SDLK_KP_6: one = "6"; break;
        case SDLK_KP_7: one = "7"; break;
        case SDLK_KP_8: one = "8"; break;
        case SDLK_KP_9: one = "9"; break;
        case SDLK_COMMA: one = "comma"; break;
        case SDLK_PERIOD: one = "period"; break;
        case SDLK_SLASH: one = "slash"; break;
        case SDLK_SEMICOLON: one = "semicolon"; break;
        case SDLK_APOSTROPHE: one = "colon"; break; // nearest physical key
        case SDLK_MINUS: one = "minus"; break;
        case SDLK_EQUALS: one = "equals"; break;
        case SDLK_LEFTBRACKET: one = "at"; break;   // C64 @ sits at [
        case SDLK_RIGHTBRACKET: one = "star"; break; // C64 * sits at ]
        case SDLK_BACKSLASH: one = "arrow_up"; break;
        case SDLK_GRAVE: one = "arrow_left"; break;
        // one C64 key, reached with shift on the real machine
        case SDLK_DOWN: one = "cursor_up_down"; break;
        case SDLK_RIGHT: one = "cursor_left_right"; break;
        case SDLK_F1: one = "f1"; break;
        case SDLK_F3: one = "f3"; break;
        case SDLK_F5: one = "f5"; break;
        case SDLK_F7: one = "f7"; break;
        case SDLK_UP:
            names[0] = "right_shift";
            names[1] = "cursor_up_down";
            return 2;
        case SDLK_LEFT:
            names[0] = "right_shift";
            names[1] = "cursor_left_right";
            return 2;
        case SDLK_F2: names[0] = "left_shift"; names[1] = "f1"; return 2;
        case SDLK_F4: names[0] = "left_shift"; names[1] = "f3"; return 2;
        case SDLK_F6: names[0] = "left_shift"; names[1] = "f5"; return 2;
        case SDLK_F8: names[0] = "left_shift"; names[1] = "f7"; return 2;
        }
    if (!one)
        return 0;
    names[0] = one;
    return 1;
}

bool matrix_event_json(const char *names[], int n, const char *transition,
                       char *buf, size_t cap)
{
    int w = snprintf(buf, cap,
                     "{\"events\":[{\"kind\":\"keyboard\",\"inputs\":[");
    for (int i = 0; i < n && w > 0 && (size_t)w < cap; i++)
        w += snprintf(buf + w, cap - (size_t)w, "%s\"%s\"", i ? "," : "",
                      names[i]);
    if (w > 0 && (size_t)w < cap)
        w += snprintf(buf + w, cap - (size_t)w,
                      "],\"transition\":\"%s\"}]}", transition);
    return w > 0 && (size_t)w < cap;
}

// .gap groups the rows in the F10 overlay and --help with a blank line.
const struct viewer_binding viewer_bindings[] = {
    {.label = "any typing", .desc = "typed into the C64; Shift graphics",
     .action = VA_INFO},
    {.label = "Esc", .desc = "RUN/STOP", .action = VA_INFO},
    {.label = "F1-F8, cursors, Home", .desc = "the matching C64 keys",
     .action = VA_INFO},
    // ASCII only: the overlay renders through the 8x8 basic font
    {.label = "Tab, PgUp, Ins, End", .desc = "CTRL, RESTORE, pound, + (matrix)",
     .action = VA_INFO, .gap = true},
    {.label = "file drop", .desc = "run .prg/.crt/.sid/.d64 on the C64",
     .action = VA_INFO},
    {.key = SDLK_F9, .label = "F9", .desc = "toggle the Ultimate menu view",
     .action = VA_MENU_VIEW},
    {.key = SDLK_F10, .label = "F10", .desc = "show/hide this help",
     .action = VA_HELP, .gap = true},
    {.key = SDLK_R, .mod = SDL_KMOD_CTRL, .label = "Ctrl+R",
     .desc = "reset the C64", .action = VA_RESET},
    {.key = SDLK_R, .mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
     .label = "Ctrl+Shift+R", .desc = "reboot the Ultimate",
     .action = VA_REBOOT},
    {.key = SDLK_P, .mod = SDL_KMOD_CTRL, .label = "Ctrl+P",
     .desc = "pause/resume the machine", .action = VA_PAUSE},
    {.key = SDLK_M, .mod = SDL_KMOD_CTRL, .label = "Ctrl+M",
     // the on-screen menu only answers the physical keyboard on today's
     // firmware: every remote keystroke is a DMA write, and the firmware
     // evicts the menu before any DMA write (dma_load_raw_buffer)
     .desc = "menu button; F9 drives the menu", .action = VA_MENU_BTN,
     .gap = true},
    {.key = SDLK_Q, .mod = SDL_KMOD_CTRL, .label = "Ctrl+Q", .desc = "quit",
     .action = VA_QUIT},
};
const int viewer_bindings_count =
    (int)(sizeof viewer_bindings / sizeof viewer_bindings[0]);

enum viewer_action viewer_binding_match(SDL_Keycode key, SDL_Keymod mod)
{
    SDL_Keymod m = 0; // collapse left/right variants, ignore other mods
    if (mod & SDL_KMOD_CTRL)
        m |= SDL_KMOD_CTRL;
    if (mod & SDL_KMOD_SHIFT)
        m |= SDL_KMOD_SHIFT;
    for (int i = 0; i < viewer_bindings_count; i++) {
        const struct viewer_binding *b = &viewer_bindings[i];
        if (b->action != VA_INFO && b->key == key && b->mod == m)
            return b->action;
    }
    return VA_NONE;
}

int special_to_petscii(SDL_Keycode key, SDL_Keymod mod)
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
