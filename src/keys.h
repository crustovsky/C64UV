// PC keyboard -> PETSCII mapping for the KERNAL keyboard buffer.
#ifndef KEYS_H
#define KEYS_H

#include <SDL3/SDL.h>

// ASCII -> PETSCII for the default uppercase/graphics charset; -1 = no key.
int ascii_to_petscii(unsigned char a);
// Non-printable SDL keys (cursors, F-keys, Home, Del/Ins...); -1 = unmapped.
int special_to_petscii(SDL_Keycode key, SDL_Keymod mod);

#endif
