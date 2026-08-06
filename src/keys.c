#include "keys.h"

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
