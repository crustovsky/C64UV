# CLAUDE.md - Commodore 64 Ultimate Viewer

Linux SDL3 viewer for the C64 Ultimate / Ultimate 64: streams the machine's
video and audio into a window, forwards keystrokes, gamepads and the mouse, and shows the
Ultimate's menu (over REST on firmware 1.2+, over telnet before). User-facing docs live in README.md; this file holds what a
developer (or Claude) needs beyond that.

## Architecture

Single binary, two dependencies (SDL3, libcurl):

```
src/main.c   event loop, sockets, audio, REST keepalive, rendering, the
             menu-poll and mouse-sender threads
src/keys.c   PC key -> PETSCII / C64 matrix maps, ASCII -> matrix keys,
             joystick and mouse event JSON + the viewer key-binding
             table (viewer_bindings drives dispatch, F10 overlay, --help)
src/discover.c  /v1/info sweep of the local /24s (SDL-free, curl multi)
src/video.c  VIC frame assembly (SDL-free, unit-tested)
src/term.c   minimal VT100 emulator matched to the firmware's remote screen,
             plus the decoder/renderer for machine:menu_screen payloads
src/font8x8.h  public-domain 8x8 bitmap font (rendering for term.c)
src/compat.h   platform layer: sockets, interface list, neighbor (ARP)
               table, ARP prime; compat_posix.c is the Linux reference
               implementation, compat_win32.c the Winsock port
               (`make TARGET=win32`, MinGW cross build in release.yml)
```

Nothing outside compat_posix.c includes a socket or network header: main.c
and discover.c are C11 + SDL3 + libcurl + compat.h. Sockets are
`compat_sock` compared against `COMPAT_BAD_SOCK` (never `< 0`, Winsock
handles are unsigned). SDL wrappers stand in for the POSIX string/env
calls (`SDL_strcasecmp`, `SDL_setenv_unsafe`).

Packaging: `make install` (DESTDIR/PREFIX) installs the binary plus
`assets/c64uv.desktop` and `assets/c64uv.svg` (icon; regenerate with
`tools/genicon.py`, which rasterises font8x8.h and also wraps the PNGs
into `assets/c64uv.ico` - never hand-edit the SVG). The Windows exe gets
the icon and a version block from `assets/c64uv.rc` (windres, in the
`TARGET=win32` Makefile branch; SDL uses the exe's first icon as the
window icon on Windows).
`packaging/aur/PKGBUILD` builds from the GitHub tag tarball, so it can only
reference tags that already contain the packaging files; bump `pkgver` and
`sha256sums` on release (the release workflow builds the .pkg.tar.zst from
the pushed tag itself, overriding pkgver, so a stale PKGBUILD never blocks
it; a manual run with the `tag` input does the same for an existing
release). `SDL_SetAppMetadata` identifier and the desktop
file basename must both stay `c64uv` or desktops lose the window icon.

```
UDP :11000 -> frame assembler -> VIC palette LUT -> SDL streaming texture
UDP :11001 -> SDL_AudioStream (resample + latency servo -> 60 ms target)
  (--multicast: both sockets also join 239.0.1.64/.65, SO_REUSEADDR)
SDL events -> PETSCII -> TCP :64 KEYB   |   F9 view: VT100 keys -> TCP :23
  (firmware 1.2+: key up/down, pads, mouse -> POST machine:input instead;
   F9 view = menu thread polling GET machine:menu_screen, keys as above)
keepalive thread -> ARP prime (ping -I) + PUT streams/{video,audio}:start / 5 s
                    + one-time capability probe: GET machine:input (keys,
                    mouse), Joystick Swapper config, GET machine:menu_screen
no host -> discover_scan() /v1/info sweep   |   file drop/--run -> runners:*
Ctrl hotkeys / --do -> PUT machine:{reset,reboot,pause,resume,menu_button}
--type -> KEYB batches over TCP :64   |   --screen -> GET machine:readmem $0400
--keys / --type --matrix / --joy / --mouse-send -> POST machine:input
--menu-screen -> GET machine:menu_screen
```

Every in-window action has a headless one-shot flag (`--discover`, `--do`,
`--run`/`--store`, `--type`, `--keys`, `--joy`, `--mouse-send`, `--screen`,
`--menu-screen`, `--dump`, `--term-test`); keep it that way so scripts and
agents can drive the machine. `--type` + `--screen` is the closed loop for
checking typed input, `--do menu` + `--keys` + `--menu-screen` the one for
the Ultimate menu.

Threads: the event loop never waits for the network longer than one
`machine:input` POST (250 ms cap). Everything slower has a thread: the
keepalive, discovery, a file run, the menu poll (`rmenu_thread`) and the
mouse sender (`mfwd_thread`). Each is joined at exit behind an
event-pumping wait (see the power-off notes under Dev workflow). Results go to stdout, logs to
stderr; exit 0/1/2 = ok / refused or unreachable / usage.

The hardware-independent pieces (video.c, term.c, keys.c, discover.c) are
split out so tests can link them; main.c keeps everything socket- and
SDL-bound.

## Tests

`make test` runs tests/tests.c (VT100 parser, frame assembly, PETSCII map,
JSON scanner). `tests/integration.sh` runs the mock-stream pipeline end to
end (frame geometry, palette, nibble order) plus REST-level tests against
`tests/fakeultimate.py`, a fake `/v1` server that logs every request
(ports: 8064 REST, 8065 DMA, 8067 FTP; 8068 and 127.0.0.44 stay closed for
the services-off tests). The fake keeps a little state a test can read or
change like on the machine: the Joystick Swapper setting (PUT the config
value), a menu that `menu_button` toggles and that `machine:input` keys
move and leave, and whether `machine:input` lists a mouse (`PUT
/test/mouse?value=0|1`).
`C64U_TEST_EVENTS` scripts key and mouse input for the windowed viewer
(`ready wait=300 key=ctrl+g move=12,-4 button=left:1 key=f9`, see
`test_events_pump` in main.c), so Ctrl+G, F9 and Ctrl+M are tested end to
end under `SDL_VIDEODRIVER=dummy`, in CI too; only the gamepad tests need
`/dev/uinput`.
Discovery is testable hermetically via env hooks: `C64U_DISCOVER_NET`
(sweep exactly that /24, loopback allowed) and `C64U_DISCOVER_PORT`
(override port 80; the chosen host then keeps `:port` for the REST calls
too) and `C64U_ARP_TABLE` (neighbor-table file the wired-interface check
reads instead of `/proc/net/arp`). The unit tests also exercise the compat
layer itself with loopback sockets (ports 21098/21099). The suite listens
on port 21000 so a viewer the user left running
cannot collide, and the windowed no-host path is exercised headless via
`SDL_VIDEODRIVER=dummy`. A tarpit server (accepts, never answers) stands in
for a powered-off Ultimate: quitting (SIGTERM = `SDL_EVENT_QUIT`) must
finish within 3 s while the keepalive thread is stuck in a REST call. CI
(GitHub Actions, Arch container) runs build + both on every push and PR.

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
  table, and the table only fills for packets the firmware *answers*: a
  bare UDP datagram to the stream port leaves it empty (verified on
  Windows 2026-09-27, 404 until the prime became an ICMP echo), a ping
  works because the reply makes the firmware ARP for us. Hence the ping
  prime before every keepalive start (`ping -I <iface>` on Linux,
  `IcmpSendEcho` on Windows). `-I` matters when policy routing (e.g. a VPN
  with accept-routes covering the local subnet) sends LAN traffic through a
  tunnel, making packets arrive from the wrong MAC. Interface selection is
  by subnet match (getifaddrs), preferring wired over `wl*`.
- **Audio queue needs a servo, not a buffer**: input and output rates match,
  so startup fill (~170 ms observed) persists forever unless actively
  drained. `SDL_SetAudioStreamFrequencyRatio` nudges (±2 %, 1x/s) hold the
  queue at the 60 ms target.
- **Keyboard**: TCP :64 `KEYB` (0xFF03, frame `03 FF <len16 LE> <chars>`)
  DMA-writes into the KERNAL buffer `$0277` + count `$C6`. The firmware does
  NOT chunk, and the buffer is 10 bytes; batches of exactly 10 were lost
  twice on hardware (2026-09-27, Windows and Linux: the following short
  batch arrived, the 10-byte one never showed), so `dma_type` sends 8 per
  frame (`KEYB_BATCH`). RUN/STOP is not a buffer
  char: poke `$91 = $7F` via `DMAWRITE` (0xFF06), repeated to win the race
  against the KERNAL restoring it (the vendor web UI does the same). The
  vendor web UI itself types via `writemem $0277`, so this is the sanctioned
  mechanism. Works only for KERNAL-read input, not matrix-scanning games.
  **Every raw DMA write evicts the on-screen menu**: the firmware's
  `dma_load_raw_buffer` (c64_subsys.cc) releases the user-interface client
  before touching memory, so a KEYB or DMAWRITE while the Ctrl+M menu is
  open closes it (verified on 1.1.0: any key kicks the user out). Hence
  the telnet view on that firmware; with `machine:input` the firmware
  routes keys into the menu instead.
- **Firmware 1.2.1 (verified over WiFi 2026-11-18)**: the update switches
  every network service off (ports 80/21/23/64 refuse until re-enabled on
  the menu page "Network Services & Timezone": Ultimate Ident Service,
  Ultimate DMA Service, Telnet Remote Menu Service, FTP File Service, Web
  Remote Control Service). `machine:input` answers 200 (keyboard, joysticks, mouse)
  and the key names in keys.c all match the spec, which the machine
  serves at `/openapi.yaml` (`/api.html` is the explorer). A `tap` returns
  before the key is released, so back-to-back taps overlap; press/release
  pairs do not. Joystick lines read back through `machine:readmem` of
  `$DC00`/`$DC01`: with Joystick Swapper on "Swapped", API port
  1 drives `$DC00` and port 2 `$DC01` (see the Joystick Swapper entry). `machine:menu_screen` is 404 until
  the menu is up (about one request after `menu_button`), then 2000
  bytes. The characters are ASCII (0x20-0x7E, mixed case), not screen
  codes; frame lines are codes below 0x20, and the selected row has bit 7
  set in both its character and colour bytes. The whole menu is drivable
  through `machine:input` press/release pairs (verified: main menu ->
  Disk File Browser -> folders via cursor right -> RETURN opens the
  action popup -> RETURN on "Run Disk" started a game), which is all a
  REST menu view needs. `machine:poweroff` exists. `run_prg` now loads from an upload
  folder under `/Temp` (cartridge parking and `$CC` gate unchanged).
  Streams over WiFi still fail with HTTP 500. The DMA socket (port 64)
  is **closed by the firmware after 1.0 s without a command**, and the
  first send into the dead socket still succeeds: one-shot typing opens a
  connection per batch, the viewer reconnects after 700 ms idle. A KEYB
  batch overwrites unread keys and BASIC reads none while it executes a
  line (about 200 ms after RETURN): `dma_type` waits for `$C6` == 0.
- **`machine:input` (CIA1 matrix-level)**: probed with a side-effect-free
  `GET /v1/machine:input` by the keepalive thread (404 on official firmware
  1.1.0, re-verified 2026-09-02; the docs specify 501 for hardware without
  it). Commodore's 1.0.0 = upstream v3.14, 1.1.0 = a 3.14-based build 165;
  the input API (upstream PR #698, June 2026) first appears in the v3.15
  release candidate; Commodore ships it from 1.2.0. Documented
  limits: 1-64 events and <= 4096 bytes per POST, 1-8 keyboard names per
  event; joystick events take `port` 1|2 and up/down/left/right/fire/
  fire2/fire3. The same firmware adds `GET /v1/machine:menu_screen` (2000
  bytes: 40x25 chars + 40x25 colour attrs, reverse video = bit 7) and
  routes keyboard events to the menu while it is open, which is what the
  REST menu view (below) is built on. When capable, key events POST
  JSON batches (`{"events":[{"kind":"keyboard","inputs":[...],"transition":
  "press"|"release"|"tap"}]}`); mapping lives in keys.c
  (`key_to_c64_matrix`): cursor up/left and F2/4/6/8 are shift chords,
  Tab = C64 CTRL (PC Ctrl stays free for viewer hotkeys), PageUp = RESTORE
  (tap-only per the API). `release_all` fires on focus loss, F9, and exit.
  Three consecutive transport failures fall back to the KERNAL buffer.
- **Services off = connection refused**: after a firmware update every
  network service is off; the machine answers ARP and ping but each TCP
  port sends RST. `rest_req` records that in `t_refused` (libcurl's
  `CURLINFO_OS_ERRNO` through `compat_errno_refused`), the socket paths ask
  `compat_neterr_refused()`, and every message then names the service
  (`rest_no_answer`, `dma_no_answer`, the status screen via `g_rest_off`).
  Discovery cannot get `/v1/info` from such a machine, but its sweep's SYN
  leaves the wired 02:15:41 MAC in the neighbor table:
  `discover_services_off` looks for that prefix and confirms with a
  refused connect. A WiFi-only machine in that state stays anonymous.
- **Joystick Swapper**: `GET /v1/configs/U64 Specific Settings/Joystick
  Swapper`, `current` = `Normal` (the default), `Swapped`, `WASD Port 2`
  or `WASD Port 1`. Verified on 1.2.1 in both states (2026-11-19, fire
  held, `$DC00`/`$DC01` read back): with `Normal` the API's port 2 drives
  `$DC00` = C64 control port 2; with `Swapped` the API's port 1 does. The
  WASD values count as not swapped. A config PUT marks the settings dirty:
  leaving a config page in the menu then asks "Save changes to Flash?".
  The viewer speaks C64 port numbers everywhere (flags, logs, notices) and
  converts in `joy_api_port`; the setting is read once by the keepalive
  thread before `g_minput` flips to 1, and by `--joy` itself.
- **Mouse (`kind: mouse`)**: firmware 1.2.1 lists a `mouse` object in the
  `GET machine:input` answer; that is the capability test (`g_mouse`).
  Events are handled like a USB mouse's reports: a 1351 on control port 1,
  one report per 20 ms from a 1024-report queue, 429 when a batch does not
  fit, `mouse.pending` = reports still queued, `release_all` detaches the
  mouse. Verified on 1.2.1 (2026-11-19) through the POT registers:
  `$D419`/`$D41A` read `$FF` until the first mouse event, then one count
  moves a POT value by 2 (a 1351 step), wrapping modulo 128, y inverted
  as on a real 1351; the left button pulls `$DC01` bit 4 (port 1 fire)
  with the swapper on Normal. `mfwd_thread` sends one `move` (up to 127 counts per axis) plus
  queued button changes per POST, then waits 20 ms plus 20 ms per pending
  report (capped); motion beyond one report that piles up meanwhile is
  dropped on purpose. Without mouse events (3.15a on a U64-II, reported by
  the contributor of the joystick mode) or with `--mouse-joy`, the mouse
  steers joystick levels instead (`mousejoy_tick`): a direction is held
  while the mouse moves and released `--mouse-hold` ms after it stops, two
  POSTs per gesture (one per pixel made the REST server miss answers).
  Their hardware notes: GEOS's built-in JOYSTICK driver reads control port
  1 and accelerates while a direction is held (`$8507` ramps), a
  pulse/duty-cycle mode felt steppy and was dropped; the speed control is
  GEOS's own mouse-speed setting.
- **Menu view over REST**: `machine:menu_screen` answers 404 with
  "Menu screen unavailable." while the menu is closed; firmware without
  the call answers 404 with other text, which is how the probe tells them
  apart (`g_menu_rest`). `rmenu_thread` polls every 100 ms (at once after a
  key), presses `menu_button` only when the first poll says closed, and
  ends the view when a live menu turns 404 (left with RUN/STOP, something
  was started). `menu_button` toggles, so closing first checks that the
  menu is still open. Commands and states carry a generation number so a
  stale state never answers a new command. Rendering (`menu_render` in
  term.c): colour byte = foreground in the low nibble, background in the
  high one, where 0 (nearly everywhere) means the menu's screen
  background. That colour is not in the payload: the keepalive probe reads
  `configs/User Interface Settings/Color Scheme` ("Commodore Blue" = light
  blue 14 in a blue 6 border, as seen in the stream and on HDMI; "Ultimate
  Black" = black; the other four, "Commodore 1-3" and "C128 Style", are
  unknown here and fall back to `menu_guess_bg`, the high nibble of the
  selected row). `machine:readmem` of `$D020/$D021` does not help: with
  the menu open it returns the frozen program's values, bit 7 of the character = reverse video,
  codes below 0x20 = the firmware's frame shapes, drawn from their arms
  (the logo pieces 0x14-0x1F have no drawing here and stay blank; the
  firmware's own character set is GPL, so it is not embedded). The
  selected row has bit 7 in both bytes (colour 0xE1); headings and the
  highlighted button of a popup are reversed with a plain colour byte, so
  `menu_row_selected` needs both bits. `tests/menu_screen.bin` is a real
  1.2.1 main menu. Verified on hardware 2026-11-19 (wired): F9 open and
  close, keys reaching the menu, Ctrl+M attach, RUN/STOP at the top level
  ending the view, quit closing the menu, and the headless loop `--do
  menu` / `--keys` / `--menu-screen`. The menu samples the keyboard
  slowly: a press released 10 ms later is missed, 50 ms is seen. Mouse
  events reach the open menu too (a `left` tap answered a popup).
- **The menu can be in the VIC stream after all**: on the C64 Ultimate
  with 1.2.1 a `--dump` taken while the menu was open shows the menu in
  the stream (light blue background, white text, the logo header), so the
  claim "the menu overlay is not in the VIC stream" only holds for the
  HDMI-overlay interface type of the Ultimate 64. With the stream up, the
  menu is therefore visible without F9, and typed keys reach it through
  `machine:input`. The REST view still earns its place without a stream
  (WiFi) and in overlay mode.
- **A dead stream link is named**: `compat_arp_prime` reports whether the
  echo was answered. REST answering while two primes in a row get no reply
  out of the stream's interface sets `g_link_dead`: log line plus a status
  screen that names the interface. Found 2026-11-19 after a reboot:
  NetworkManager's address-conflict check on the wired interface was
  answered by the laptop's own WiFi card (same LAN, ARP flux), it removed
  the wired subnet route, all traffic to the machine went over WiFi and
  the 22 Mbit/s stream kept cutting out ("stream stopped" / "stream
  resumed" in a loop). The test runs in `unshare -Urn` with
  `icmp_echo_ignore_all=1`.
- **Notices are toasts**: a gamepad that is plugged in before the first
  frame is announced once the picture is up, not over the connecting
  screen. `status_set` text doubles as a banner over the
  lower edge of the picture while video is live (`toast_render`, lines up
  to 46 characters); without a picture the status screen shows it. SDL has
  no notification API of its own, only a modal message box.
- **Matrix typing (`--keys`, `--type --matrix`)**: press, 50 ms hold,
  release, 50 ms gap per stroke (`matrix_stroke`); never `tap` back to
  back. Verified on 1.2.1: a BASIC line with shifted characters arrived
  intact at about 10 characters a second. `--type` without `--matrix` stays on the KERNAL buffer, which is
  faster and works on every firmware.
- **Telnet menu (TCP :23)**: firmware `screen_vt100.cc` emits exactly: `ESC c`
  (RIS), `ESC[y;xH`, a fixed SGR set (`0;3X` + `;1`/`;2`, `7`/`27`), `ESC(0`
  / `ESC(B` charset switches, `ESC[2J`, `ESC[r`. Screen is fixed **60x24**.
  Colors are C64 colors round-tripped through ANSI - term.c inverts the
  firmware's `set_color` table back to VIC colors. Input parsing
  (`keyboard_vt100.cc`) wants xterm-style: `ESC[A-D`, `ESC[N~` (old-style
  F-keys: 11-15 = F1-F5, 17-19 = F6-F8), bare `ESC` = back. The menu overlay
  is not in the VIC stream; before firmware 1.2 telnet is the only remote
  view of it.
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
- **Machine vanishing mid-session** (power-off) reproduces without root:
  `unshare -Urn`, a veth pair between that netns and a second one
  (`unshare -n sleep inf` + `ip link set vB netns <pid>`), the fakes
  (fakeultimate.py, a telnet stub, mockstream.py) on the far end, the
  windowed viewer on the near end (the Wayland socket works across
  netns), then `ip addr del` on the far end: packets go unanswered exactly
  like a dead box on the LAN, unlike killing a fake (which sends FIN/RST).
  Hyprland can press keys in the window without focusing it:
  `hyprctl dispatch 'hl.dsp.send_shortcut({ mods = "", key = "F9", window
  = "class:c64uv" })'`. Found this way (2026-09-03): the main thread must
  never block for long, Hyprland shows its "Application Not Responding"
  dialog after about 8 s of unanswered pings, and the old exit path joined
  the keepalive thread through up to 13 s of REST timeouts; hence the
  cancellable keepalive calls and the event-pumping join at exit.

## Roadmap

The six 2026-08 milestones (discovery, multicast, machine:input, machine
control + password, drag-and-drop run, help overlay) shipped in v0.2.0.

1. **Platform compat layer** (done 2026-09): `src/compat.h` +
   `compat_posix.c` hold sockets, interface enumeration, neighbor/ARP
   lookup, and the ARP prime (`ping -I` on Linux for policy routing; a
   plain datagram does NOT suffice anywhere, see protocol facts). Linux stays the reference
   implementation and sole CI target. Audit 2026-09-27: main.c and
   discover.c are free of POSIX calls (file loading via `SDL_LoadFile`,
   dropped paths split on both separators, no errno/unistd), so a port is
   compat_win32.c (the ~240 lines of compat_posix.c: Winsock,
   `GetAdaptersAddresses`, `GetIpNetTable`, prime = `ping -S` or a
   datagram) plus `make COMPAT=src/compat_win32.c` under MSYS2 (SDL3 and
   curl come from its pacman; `<stdatomic.h>` needs MinGW or VS 2022
   17.5+). Windows port written 2026-09-27 on that basis:
   `compat_win32.c` (Winsock, `WSAPoll`, non-blocking connect + select for
   the connect timeout since `SO_SNDTIMEO` does not bound `connect()` on
   Winsock, `GetAdaptersAddresses` with `OnLinkPrefixLength` for the mask,
   `GetIpNetTable` for the neighbor MAC, prime = `IcmpSendEcho`), `compat_sock`
   is `uintptr_t` there, `make TARGET=win32` cross-builds with MinGW and
   release.yml ships `c64uv-<tag>-windows-x86_64.zip` (exe + SDL3.dll from
   the official MinGW package + static curl, console subsystem so the CLI
   flags work). Verified on Michal's Windows box 2026-09-27: discovery,
   REST, DMA keyboard, video + audio streams (once the prime became an
   ICMP echo, see protocol facts). Unit/integration tests stay Linux-only (bash + loopback). A macOS
   port (compat_posix.c mostly builds as-is: BSD sockets +
   `getifaddrs`, but `/proc/net/arp` and `ping -I` need `arp -n` /
   `ping -b` equivalents) only on request.
2. **Gamepad -> machine:input joysticks** (done 2026-11-18, verified on
   firmware 1.2.1 with a virtual pad, lines read back at `$DC00`/`$DC01`,
   then played by Michal with an Xbox pad over Bluetooth): SDL_Gamepad with hotplug,
   two pads: the first drives `mi.joy_port`, the second the other port,
   and a pad keeps its slot when the other one leaves. Each pad is polled
   into JOY_* bits on every pad event (`pad_state` in main.c) and
   `minput_joy` POSTs only the difference per port (`joy_sent[2]`),
   release before press in one batch; mapping and JSON live in
   keys.c (`pad_button_joy`, `pad_stick_joy`, `joy_event_json`). D-pad +
   left stick (dead zone 16000) = directions, A = fire, B = up, X/Y =
   fire2/fire3. Port 2 by default (`--joy-port`), Ctrl+J or the pad's Back
   button swaps. `--joy [PORT:]inputs` is the headless twin (press, 150 ms,
   release). `release_all` (focus loss, F9, exit) drops the joystick lines
   too, so it zeroes `joy_sent`; the next pad event resyncs. SDL delivers
   no pad events to an unfocused window (tests set
   `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`; the same variable lets a user
   play with another window focused). "No button works" is that focus
   rule first: `--verbose` logs every state change (`joystick: port N
   state XX`), so an empty log means no events arrive. Start is F9; in the
   REST menu view the pad sends keys instead of joystick lines
   (`pad_menu_key`). `tests/virtualpad.py` is a
   dependency-free uinput pad (Xbox 360 ids, so SDL maps it); the
   integration test using it is skipped where `/dev/uinput` is not
   writable (CI). The F10 overlay's height is computed from the binding table
   (`help_height`), after a fixed 256 px buffer overflowed by one row.
   The new Steam Controller is SDL's job: code against generic
   SDL_Gamepad.

3. **Persistent drop storage** (implemented and verified on hardware
   2026-09-27, Windows client against firmware 1.1.0: upload, path mount,
   reset and typed autostart all went through): `--store <folder>` / `C64U_STORE` switches image drops
   from the firmware's temp area (RAM disk, gone at power-off) to
   FTP-upload-then-mount-by-path, see `store_image` in main.c. FTP is the
   only upload route: the REST files API has no upload on any firmware
   (verified: `curl -T` to `ftp://<ult>/Temp/` works, `files/<path>:info`
   then sees the file, the FTP service is on by default on 1.1.0). Sequence:
   libcurl FTP upload (anonymous; STOR replaces a same-named file, which
   is what Michal wants for re-drops), `PUT drives/a:mount?image=<path>&mode=readwrite`, then
   `machine:reset` + readiness gate + `LOAD"*",8,1` / `RUN` typed over the
   keyboard channel in 8-byte batches (no firmware autostart for a
   path mount; the KERNAL load runs at ~400 bytes/s, so the second gate
   allows 120 s). The static release build now keeps FTP in curl. Michal's
   preference: upload to `/Temp` and move the file from the Ultimate menu
   himself. SDL does report a modifier held during a drag on Wayland
   (Hyprland, verified 2026-09-27 via the `--verbose` drop log), so a
   modifier-selected store is possible. Verified: `image=`
   takes a literal `/`-separated path; `files/<path>:info` answers
   non-200 for a missing file (no longer used). The typed autostart needs
   a boot head start: reset zeroes the zero page, so the `$CC` gate can
   pass mid-boot and the KERNAL init then wipes the typed buffer (seen as
   "1", "RUN", READY with the first batch gone). Follow-up on top
   of it: in the F9 view, upload into the folder the menu currently shows
   (path line parse; truncated long paths need a fallback). Test hooks:
   `C64U_FTP_PORT` (fakeultimate.py serves a passive-mode FTP stub as its
   fifth argument and logs `FTP STOR <path> len=N`).

4. **0.4.0 (2026-11-19)**: REST menu view, 1351 mouse forwarding with the
   contributed joystick mode as fallback, second gamepad, Joystick Swapper
   port mapping, matrix-level `--keys` / `--type --matrix`, services-off
   hints. Details are in the protocol facts above.

Open follow-ups: uploading a dropped image into the folder the menu shows
(the path line is now readable through `machine:menu_screen`), re-checking
the matrix-keyboard mapping in a matrix-scanning game with the stream
running, and the colours of the four menu colour schemes not seen yet.
