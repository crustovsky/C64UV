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

// ASCII -> C64 matrix key name(s) for scripted typing (--type --matrix):
// the key that prints the character with the default character set, plus
// a shift where the C64 needs one ('"' = shift+2, '?' = shift+slash).
// Letters come out unshifted in either case, like --type. '\n' and '\r'
// are RETURN. Returns the count, 0 = no such key.
int ascii_to_c64_matrix(unsigned char c, const char *names[2]);

// True for a key name the machine:input API accepts ("a", "f1",
// "run_stop", "cursor_up_down", "restore"...).
bool matrix_key_valid(const char *name);

// Builds the machine:input POST body for one keyboard event. transition is
// "press", "release", or "tap". Returns false if buf is too small.
bool matrix_event_json(const char *names[], int n, const char *transition,
                       char *buf, size_t cap);

// Gamepad -> C64 joystick, for machine:input joystick events. A state is
// a mask of JOY_* bits.
enum {
    JOY_UP = 1, JOY_DOWN = 2, JOY_LEFT = 4, JOY_RIGHT = 8,
    JOY_FIRE = 16, JOY_FIRE2 = 32, JOY_FIRE3 = 64,
};
// One pad button -> JOY_* bit (0 = unmapped): d-pad = directions, south
// (A/cross) = fire, west (X/square) = fire2, north (Y/triangle) = fire3,
// east (B/circle) = up, the jump button platformers lack.
unsigned pad_button_joy(SDL_GamepadButton b);
// Left stick -> direction bits; inside the dead zone = 0.
unsigned pad_stick_joy(int x, int y);
// Mouse motion -> direction bits for the joystick-emulation path. dx/dy are
// screen pixels (y grows downwards); an axis within `dz` pixels is ignored.
// Diagonals set two bits. This drives a joystick, never a 1351.
unsigned mouse_joy_dir(float dx, float dy, float dz);
// Parses "up,fire" style lists (the API's input names); 0 = invalid.
unsigned joy_parse(const char *list);
// machine:input body taking control port `port` from state `from` to state
// `to`: a release event, a press event, or both. False when nothing
// changed or buf is too small.
bool joy_event_json(int port, unsigned from, unsigned to, char *buf,
                    size_t cap);

// One static table drives the viewer's hotkey dispatch, the F10 help
// overlay, and the key list in --help, so they can never drift apart.
enum viewer_action {
    VA_NONE,
    VA_INFO, // help-text-only row, never dispatched
    VA_QUIT,
    VA_MENU_VIEW,
    VA_HELP,
    VA_RESET,
    VA_REBOOT,
    VA_PAUSE,
    VA_MENU_BTN,
    VA_MOUSE_JOY,
    VA_JOY_PORT,
};

struct viewer_binding {
    SDL_Keycode key;   // 0 for VA_INFO rows
    SDL_Keymod mod;    // exact Ctrl/Shift requirement (combined masks)
    const char *label; // shown in the overlay and --help
    const char *desc;
    enum viewer_action action;
    bool gap; // blank line after this row in the overlay and --help
};

extern const struct viewer_binding viewer_bindings[];
extern const int viewer_bindings_count;

// Returns the action bound to key with the given modifiers, or VA_NONE.
enum viewer_action viewer_binding_match(SDL_Keycode key, SDL_Keymod mod);

#endif
