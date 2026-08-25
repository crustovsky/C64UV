// PC keyboard -> PETSCII mapping for the KERNAL keyboard buffer.
#ifndef KEYS_H
#define KEYS_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>

// ASCII -> PETSCII for the default uppercase/graphics charset; -1 = no key.
int ascii_to_petscii(unsigned char a);
// Non-printable SDL keys (cursors, F-keys, Home, Del/Ins...); -1 = unmapped.
int special_to_petscii(SDL_Keycode key, SDL_Keymod mod);

// PC key -> C64 matrix key name(s) for the machine:input REST endpoint.
// Chorded keys (cursor up/left, F2/4/6/8) fill two names. Returns the count,
// 0 = unmapped. "restore" is tap-only per the API; check names[0] for it.
int key_to_c64_matrix(SDL_Keycode key, const char *names[2]);

// Builds the machine:input POST body for one keyboard event. transition is
// "press", "release", or "tap". Returns false if buf is too small.
bool matrix_event_json(const char *names[], int n, const char *transition,
                       char *buf, size_t cap);

#endif
