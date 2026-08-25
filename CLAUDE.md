# CLAUDE.md - Commodore 64 Ultimate Viewer

Linux SDL3 viewer for the C64 Ultimate / Ultimate 64: streams the machine's
video and audio into a window, forwards keystrokes, and shows the Ultimate's
menu over telnet. User-facing docs live in README.md; this file holds what a
developer (or Claude) needs beyond that.

## Architecture

Single binary, two dependencies (SDL3, libcurl):

```
src/main.c   event loop, sockets, audio, REST keepalive, rendering
src/video.c  VIC frame assembly (SDL-free, unit-tested)
src/term.c   minimal VT100 emulator matched to the firmware's remote screen
src/keys.c   PC key -> PETSCII mapping
src/font8x8.h  public-domain 8x8 bitmap font (rendering for term.c)
```

Packaging: `make install` (DESTDIR/PREFIX) installs the binary plus
`assets/c64uv.desktop` and `assets/c64uv.svg` (icon; regenerate with
`tools/genicon.py`, which rasterises font8x8.h - never hand-edit the SVG).
`packaging/aur/PKGBUILD` builds from the GitHub tag tarball, so it can only
reference tags that already contain the packaging files; bump `pkgver` and
`sha256sums` on release. `SDL_SetAppMetadata` identifier and the desktop
file basename must both stay `c64uv` or desktops lose the window icon.

```
UDP :11000 -> frame assembler -> VIC palette LUT -> SDL streaming texture
UDP :11001 -> SDL_AudioStream (resample + latency servo -> 60 ms target)
SDL events -> PETSCII -> TCP :64 KEYB   |   F9 view: VT100 keys -> TCP :23
keepalive thread -> ARP prime (ping -I) + PUT streams/{video,audio}:start / 5 s
```

The hardware-independent pieces (video.c, term.c, keys.c) are split out so
tests can link them; main.c keeps everything socket- and SDL-bound.

## Tests

`make test` runs tests/tests.c (VT100 parser, frame assembly, PETSCII map).
`tests/integration.sh` runs the mock-stream pipeline end to end and checks
the dumped frame's geometry, palette, and nibble order. CI (GitHub Actions,
Arch container) runs build + both on every push and PR.

## Protocol facts (hard-won, verified on real hardware)

- Stream wire formats are in README.md. Pixel packing: **low nibble =
  leftmost pixel**. The `ip=<addr>:<port>` form works on `streams/*:start`;
  there is no separate `port` parameter.
- **Streams only leave the Ultimate's wired Ethernet port** (FPGA-generated).
  On WiFi-only, start fails with HTTP 500 "No Operational Network Interface".
- **The firmware never ARPs on demand**: `streams/*:start` returns HTTP 404
  "Network Host Resolve Error" unless the destination is already in its ARP
  table. Hence the `ping -I <iface>` prime before every keepalive start - a
  plain UDP send is not enough when policy routing (e.g. a VPN with
  accept-routes covering the local subnet) sends LAN traffic through a
  tunnel, making packets arrive from the wrong MAC. Interface selection is
  by subnet match (getifaddrs), preferring wired over `wl*`.
- **Audio queue needs a servo, not a buffer**: input and output rates match,
  so startup fill (~170 ms observed) persists forever unless actively
  drained. `SDL_SetAudioStreamFrequencyRatio` nudges (±2 %, 1x/s) hold the
  queue at the 60 ms target.
- **Keyboard**: TCP :64 `KEYB` (0xFF03, frame `03 FF <len16 LE> <chars>`)
  DMA-writes into the KERNAL buffer `$0277` + count `$C6`. The firmware does
  NOT chunk - keep batches <= 10 chars (buffer size). RUN/STOP is not a buffer
  char: poke `$91 = $7F` via `DMAWRITE` (0xFF06), repeated to win the race
  against the KERNAL restoring it (the vendor web UI does the same). The
  vendor web UI itself types via `writemem $0277`, so this is the sanctioned
  mechanism. Works only for KERNAL-read input, not matrix-scanning games -
  upgrade to `machine:input` (CIA1-level, in upstream 3.15 beta) once the
  official firmware ships it; probe `PUT /v1/machine:input` after firmware
  updates.
- **Telnet menu (TCP :23)**: firmware `screen_vt100.cc` emits exactly: `ESC c`
  (RIS), `ESC[y;xH`, a fixed SGR set (`0;3X` + `;1`/`;2`, `7`/`27`), `ESC(0`
  / `ESC(B` charset switches, `ESC[2J`, `ESC[r`. Screen is fixed **60x24**.
  Colors are C64 colors round-tripped through ANSI - term.c inverts the
  firmware's `set_color` table back to VIC colors. Input parsing
  (`keyboard_vt100.cc`) wants xterm-style: `ESC[A-D`, `ESC[N~` (old-style
  F-keys: 11-15 = F1-F5, 17-19 = F6-F8), bare `ESC` = back. The menu overlay
  is not in the VIC stream; telnet is the only remote view of it.
- Screen RAM is remotely readable: `GET /v1/machine:readmem?address=0400&
  length=1000` - used to close the loop when testing typed input.

## Dev workflow (no hardware needed)

- `tools/mockstream.py [ip] [port] [secs]` sends synthetic video (color bars
  + sweep line) and audio (440 Hz tone) in the exact wire format.
- `./c64uv --no-start` = listen-only viewer against the mock.
- `./c64uv --dump f.ppm` (headless single-frame grab) and `--term-test`
  (headless menu-screen dump) are the two self-verification modes.
- `--verbose` logs fps / packet counts / gaps / audio queue depth every 5 s.

## Roadmap

1. **Discovery**: `--discover` flag (and auto-discovery when no host is given).
   One-shot `GET /v1/info` sweep of the local /24, bounded concurrency, one
   request per address, no port pre-scan and no retries. Use a split timeout:
   1.5 s TCP connect, then a separate 3.25 s for the response (wired REST can
   take ~2.5 s to answer while unreachable addresses should fail on the short
   connect budget). No mDNS/broadcast exists in the firmware.
2. **Multicast transport**: join groups 239.0.1.64/.65 (the prkl_ultimate
   defaults) via IP_ADD_MEMBERSHIP on the existing UDP sockets and pass the
   group address in `streams/*:start`. Removes the one-viewer-per-Ultimate
   limitation and lets c64uv coexist with u64deck/VLC watching the same
   machine.
3. **`machine:input` keyboard upgrade** when official firmware ships it:
   CIA1 matrix-level press/release (games, chords, held keys). Probe
   `PUT /v1/machine:input` for capability, cache the result, fall back to the
   KERNAL buffer; keep the buffer path for bulk text even on capable firmware.
4. **Machine-control hotkeys + password**: reset, reboot, pause/resume, menu
   button (single REST calls). Add `X-Password` header support (firmware
   3.12+ network password); currently there is no way to reach a
   password-protected Ultimate.
5. **Drag-and-drop run**: SDL3 drop events -> POST `.prg`/`.crt`/`.sid` to
   `runners:run_prg`/`:sidplay`. Two protocol facts to honour: (a)
   cartridge-safe run - blank the Cartridge config item before a DMA run and
   restore it after (config applies at next reset, so the program keeps
   running with the cart parked; avoids freezer-cart restore failures
   hard-resetting into the cart menu); (b) readiness gate - before typing
   after a reset, poll the KERNAL ready flag at zero-page `$CC` via
   `machine:readmem` and require two consecutive ready reads.
6. **Help overlay**: F10 toggles an in-window key reference rendered with the
   existing font8x8.h path (no new dependencies). Drive both the overlay text
   and the event dispatch from one static key-binding table so the help can
   never drift from the actual bindings; also print the same table on
   `--help`. F10 is the natural key: F1-F8 belong to the C64, F9 is the menu
   view.
