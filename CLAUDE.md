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
src/compat.h   platform layer: sockets, interface list, neighbor (ARP)
               table, ARP prime; compat_posix.c is the Linux reference
               implementation (a port swaps the file in the Makefile)
```

Nothing outside compat_posix.c includes a socket or network header: main.c
and discover.c are C11 + SDL3 + libcurl + compat.h. Sockets are
`compat_sock` compared against `COMPAT_BAD_SOCK` (never `< 0`, Winsock
handles are unsigned). SDL wrappers stand in for the POSIX string/env
calls (`SDL_strcasecmp`, `SDL_setenv_unsafe`).

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
(override port 80; the chosen host then keeps `:port` for the REST calls
too) and `C64U_ARP_TABLE` (neighbor-table file the wired-interface check
reads instead of `/proc/net/arp`). The unit tests also exercise the compat
layer itself with loopback sockets (ports 21098/21099). The suite listens
on port 21000 so a viewer the user left running
cannot collide, and the windowed no-host path is exercised headless via
`SDL_VIDEODRIVER=dummy`. CI (GitHub Actions, Arch container) runs build +
both on every push and PR.

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
  **Every raw DMA write evicts the on-screen menu**: the firmware's
  `dma_load_raw_buffer` (c64_subsys.cc) releases the user-interface client
  before touching memory, so a KEYB or DMAWRITE while the Ctrl+M menu is
  open closes it (verified on 1.1.0: any key kicks the user out). Hence
  the help copy points at F9; with `machine:input` the firmware routes
  keys into the menu instead.
- **`machine:input` (CIA1 matrix-level)**: probed with a side-effect-free
  `GET /v1/machine:input` by the keepalive thread (404 on official firmware
  1.1.0, re-verified 2026-09-02; the docs specify 501 for hardware without
  it). Commodore's 1.0.0 = upstream v3.14, 1.1.0 = a 3.14-based build 165;
  the input API (upstream PR #698, June 2026) first appears in the v3.15
  release candidate, so it needs a newer Commodore release. Documented
  limits: 1-64 events and <= 4096 bytes per POST, 1-8 keyboard names per
  event; joystick events take `port` 1|2 and up/down/left/right/fire/
  fire2/fire3. The same firmware adds `GET /v1/machine:menu_screen` (2000
  bytes: 40x25 chars + 40x25 colour attrs, reverse video = bit 7) and
  routes keyboard events to the menu while it is open, which would make a
  REST menu view possible without telnet. When capable, key events POST
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
- **Disk images (verified on 1.1.0)**: the DMA socket's `RUN_IMG` (0xFF0B,
  header `0B FF <len16 LE> <len bits 16-23>` then the image) saves the
  payload as `/temp/tcpimage.d64`, mounts it on drive A and runs
  `C64_DRIVE_LOAD` with `RUNCODE_MOUNT_LOAD_RUN`: the firmware resets,
  types `LOAD"*",8,1` and `RUN` itself (screen RAM confirms). Payload cap
  is `SOCKET_BUFFER_SIZE` = 200000 bytes (every .d64 variant fits; the
  firmware silently truncates beyond). Always saved as .d64, so only that
  type autostarts; `POST /v1/drives/a:mount?type=<d64|g64|d71|g71|d81>`
  with the image as the body mounts the others (lands as `/Temp/temp0000`).
  `MOUNT_IMG` (0xFF0A) is the socket twin of that mount. With a network
  password the socket needs `AUTHENTICATE` (0xFF1F, password as payload,
  one-byte reply 1/0, 1 s throttle on failure) before any other command,
  or the firmware drops the connection; `dma_connect` does it for both
  the keyboard channel and image runs. `C64U_DMA_PORT` redirects port 64
  for tests (fakeultimate.py logs `DMA cmd=FFxx len=N`). **One DMA client
  at a time**: `dmaThread` accepts a connection and serves it until it
  closes before accepting the next, so the viewer's open keyboard
  connection stalls an image transfer behind it (seen as a 30 s stall then
  EAGAIN; a bare socket takes 0.1 s for a .d64 whether or not the stream
  runs or the C64 is loading). Hence `run_file_async` closes the keyboard
  socket and `keyb_try_connect` stays off port 64 while `g_run_busy`.

## Dev workflow (no hardware needed)

- `tools/mockstream.py [ip] [port] [secs]` sends synthetic video (color bars
  + sweep line) and audio (440 Hz tone) in the exact wire format.
- `./c64uv --no-start` = listen-only viewer against the mock.
- `./c64uv --dump f.ppm` (headless single-frame grab) and `--term-test`
  (headless menu-screen dump) are the two self-verification modes.
- `--verbose` logs fps / packet counts / gaps / audio queue depth every 5 s.

## Roadmap

The six 2026-08 milestones (discovery, multicast, machine:input, machine
control + password, drag-and-drop run, help overlay) shipped in v0.2.0.

1. **Platform compat layer** (done 2026-09): `src/compat.h` +
   `compat_posix.c` hold sockets, interface enumeration, neighbor/ARP
   lookup, and the ARP prime (`ping -I` on Linux for policy routing; a
   plain datagram likely suffices elsewhere). Linux stays the reference
   implementation and sole CI target. Gated follow-ups, not commitments:
   a Windows port (`compat_win32.c`: Winsock, `GetAdaptersAddresses`,
   `GetIpNetTable`; CMake or dual build, CI job, zip-with-DLLs release)
   only when there is a test machine or a motivated tester with real
   hardware - the community is Windows-heavy, but an unverifiable port
   rots; a macOS port (compat_posix.c mostly builds as-is: BSD sockets +
   `getifaddrs`, but `/proc/net/arp` and `ping -I` need `arp -n` /
   `ping -b` equivalents) only on request.
2. **Gamepad -> machine:input joysticks**: SDL_Gamepad (SDL_INIT_GAMEPAD,
   hotplug), d-pad + digitalized left stick -> directions, A = fire, B = up
   as an option (platformers jump via up), events POSTed as
   `{"kind":"joystick","port":N,...}` through the existing minput plumbing
   (probe, release_all, fallback counters). Must include a port-swap
   key/flag: games split between ports 1 and 2. Dormant until official
   firmware ships `machine:input` (404 on 1.1.0, verified; no fallback
   exists - games read the CIA lines directly). Testable now: unit tests
   for mapping/JSON, integration via fakeultimate, SDL virtual gamepads
   for synthetic input. The new Steam Controller is SDL's job: support
   comes from SDL3's HIDAPI drivers + mapping db (worst case Steam udev
   rules or SDL_GAMECONTROLLERCONFIG); code against generic SDL_Gamepad.

3. **Persistent drop storage** (agreed 2026-09-02, not started): the drop
   path keeps the firmware's temp area (RAM disk, gone at power-off) as
   the fast default; a `--store <folder>` flag and/or a modifier held
   during the drop switch to FTP-upload-then-mount-by-path. FTP is the
   only upload route: the REST files API has no upload on any firmware
   (verified: `curl -T` to `ftp://<ult>/Temp/` works, `files/<path>:info`
   then sees the file, the FTP service is on by default on 1.1.0). Sequence:
   check `files/<path>:info` (refuse to overwrite), `curl -T` the file,
   `PUT drives/a:mount?image=<path>&mode=readwrite`, then for autostart
   `machine:reset` + readiness gate + `LOAD"*",8,1` / `RUN` over the
   keyboard channel (no firmware autostart for a path mount). Michal's
   preference: upload to `/Temp` and move the file from the Ultimate menu
   himself. Open questions: whether SDL reports a modifier held during a
   drag on Wayland (`SDL_GetKeyboardState` at drop time; if not, flag
   only), and the static release build needs curl rebuilt with FTP
   (`--disable-ftp` today in release.yml). Follow-up on top of it: in the
   F9 view, upload into the folder the menu currently shows (path line
   parse; truncated long paths need a fallback).

Dormant follow-up: when official firmware ships `machine:input`, re-verify
the matrix-keyboard mapping against real hardware and activate the gamepad
path alongside it.
