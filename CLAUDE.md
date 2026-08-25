# CLAUDE.md - Commodore 64 Ultimate Viewer

Linux SDL3 viewer for the C64 Ultimate / Ultimate 64: streams the machine's
video and audio into a window, forwards keystrokes, and shows the Ultimate's
menu over telnet. User-facing docs live in README.md; this file holds what a
developer (or Claude) needs beyond that.

## Architecture

Single binary, two dependencies (SDL3, libcurl):

```
src/main.c   event loop, sockets, audio, REST keepalive, rendering
src/keys.c   PC key -> PETSCII / C64 matrix maps + the viewer key-binding
             table (viewer_bindings drives dispatch, F10 overlay, --help)
src/discover.c  /v1/info sweep of the local /24s (SDL-free, curl multi)
src/video.c  VIC frame assembly (SDL-free, unit-tested)
src/term.c   minimal VT100 emulator matched to the firmware's remote screen
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
  (--multicast: both sockets also join 239.0.1.64/.65, SO_REUSEADDR)
SDL events -> PETSCII -> TCP :64 KEYB   |   F9 view: VT100 keys -> TCP :23
  (matrix-capable firmware: key up/down -> POST machine:input instead)
keepalive thread -> ARP prime (ping -I) + PUT streams/{video,audio}:start / 5 s
                    + one-time GET machine:input capability probe
no host -> discover_scan() /v1/info sweep   |   file drop/--run -> runners:*
Ctrl hotkeys / --do -> PUT machine:{reset,reboot,pause,resume,menu_button}
```

The hardware-independent pieces (video.c, term.c, keys.c, discover.c) are
split out so tests can link them; main.c keeps everything socket- and
SDL-bound.

## Tests

`make test` runs tests/tests.c (VT100 parser, frame assembly, PETSCII map,
JSON scanner). `tests/integration.sh` runs the mock-stream pipeline end to
end (frame geometry, palette, nibble order) plus REST-level tests against
`tests/fakeultimate.py`, a fake `/v1` server that logs every request.
Discovery is testable hermetically via env hooks: `C64U_DISCOVER_NET`
(sweep exactly that /24, loopback allowed) and `C64U_DISCOVER_PORT`
(override port 80). CI (GitHub Actions, Arch container) runs build + both
on every push and PR.

## Protocol facts (hard-won, verified on real hardware)

- Stream wire formats are in README.md. Pixel packing: **low nibble =
  leftmost pixel**. The `ip=<addr>:<port>` form works on `streams/*:start`;
  there is no separate `port` parameter.
- **Streams only leave the Ultimate's wired Ethernet port** (FPGA-generated).
  On WiFi-only, start fails with HTTP 500 "No Operational Network Interface".
- **Multicast destinations work**: `streams/*:start` accepts a multicast
  group in `ip=` (no ARP needed). c64uv convention (matching prkl_ultimate):
  `--multicast` = video 239.0.1.64, audio 239.0.1.65; a multicast `--dest`
  uses that group for video and group+1 for audio. Sockets take SO_REUSEADDR
  so several local viewers can share the port; on exit each viewer still
  issues `:stop`, and other viewers' keepalives restart the stream within
  5 s.
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
  mechanism. Works only for KERNAL-read input, not matrix-scanning games.
- **`machine:input` (CIA1 matrix-level)**: probed with a side-effect-free
  `GET /v1/machine:input` by the keepalive thread (404 on official firmware
  1.1.0, verified; upstream 3.15 beta has it). When capable, key events POST
  JSON batches (`{"events":[{"kind":"keyboard","inputs":[...],"transition":
  "press"|"release"|"tap"}]}`); mapping lives in keys.c
  (`key_to_c64_matrix`): cursor up/left and F2/4/6/8 are shift chords,
  Tab = C64 CTRL (PC Ctrl stays free for viewer hotkeys), PageUp = RESTORE
  (tap-only per the API). `release_all` fires on focus loss, F9, and exit.
  Three consecutive transport failures fall back to the KERNAL buffer.
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
- **Runners (verified on firmware 1.1.0)**: `POST /v1/runners:run_prg` takes
  the raw file as the request body (application/octet-stream, no multipart).
  The firmware then resets the machine itself, types
  `LOAD"/TEMP/TEMP0000",8,1` and `RUN` - no client-side reset or typing is
  needed. Because that internal reset re-reads the config, a configured
  cartridge would boot instead: blank `configs/C64 and Cartridge
  Settings/Cartridge` first, POST, then restore only after the readiness
  gate (`machine:readmem` of `$CC` == 0 twice in a row, 10 s timeout for
  programs that never return to the prompt). `run_crt` runs the posted cart
  on purpose, so no parking there. `$CC` reads 0x00 at the READY prompt,
  verified.

## Dev workflow (no hardware needed)

- `tools/mockstream.py [ip] [port] [secs]` sends synthetic video (color bars
  + sweep line) and audio (440 Hz tone) in the exact wire format.
- `./c64uv --no-start` = listen-only viewer against the mock.
- `./c64uv --dump f.ppm` (headless single-frame grab) and `--term-test`
  (headless menu-screen dump) are the two self-verification modes.
- `--verbose` logs fps / packet counts / gaps / audio queue depth every 5 s.

## Roadmap

Empty. The six 2026-08 milestones (discovery, multicast, machine:input,
machine control + password, drag-and-drop run, help overlay) all shipped
in v0.2.0. One dormant follow-up: the matrix-keyboard path activates
itself when official firmware ships `machine:input`; re-verify the mapping
against real hardware when that lands.
