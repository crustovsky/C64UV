# CLAUDE.md — Commodore 64 Ultimate Viewer

Linux SDL3 viewer for the C64 Ultimate / Ultimate 64: streams the machine's
video and audio into a window, forwards keystrokes, and shows the Ultimate's
menu over telnet. User-facing docs live in README.md; this file holds what a
developer (or Claude) needs beyond that.

## Architecture

Single binary, three source files, two dependencies (SDL3, libcurl):

```
src/main.c   event loop, UDP receive, frame assembly, audio, REST keepalive,
             keyboard (TCP :64), telnet plumbing
src/term.c   minimal VT100 emulator matched to the firmware's remote screen
src/font8x8.h  public-domain 8x8 bitmap font (rendering for term.c)
```

```
UDP :11000 → frame assembler → VIC palette LUT → SDL streaming texture
UDP :11001 → SDL_AudioStream (resample + latency servo → 60 ms target)
SDL events → PETSCII → TCP :64 KEYB   |   F9 view: VT100 keys → TCP :23
keepalive thread → ARP prime (ping -I) + PUT streams/{video,audio}:start / 5 s
```

Deliberate choice: no further file splitting while the whole program is
~1k lines — the section banners in main.c are the module boundaries.

## Protocol facts (hard-won, verified on real hardware)

- Stream wire formats are in README.md. Pixel packing: **low nibble =
  leftmost pixel**. The `ip=<addr>:<port>` form works on `streams/*:start`;
  there is no separate `port` parameter.
- **Streams only leave the Ultimate's wired Ethernet port** (FPGA-generated).
  On WiFi-only, start fails with HTTP 500 "No Operational Network Interface".
- **The firmware never ARPs on demand**: `streams/*:start` returns HTTP 404
  "Network Host Resolve Error" unless the destination is already in its ARP
  table. Hence the `ping -I <iface>` prime before every keepalive start — a
  plain UDP send is not enough when policy routing (e.g. a VPN with
  accept-routes covering the local subnet) sends LAN traffic through a
  tunnel, making packets arrive from the wrong MAC. Interface selection is
  by subnet match (getifaddrs), preferring wired over `wl*`.
- **Audio queue needs a servo, not a buffer**: input and output rates match,
  so startup fill (~170 ms observed) persists forever unless actively
  drained. `SDL_SetAudioStreamFrequencyRatio` nudges (±2 %, 1×/s) hold the
  queue at the 60 ms target.
- **Keyboard**: TCP :64 `KEYB` (0xFF03, frame `03 FF <len16 LE> <chars>`)
  DMA-writes into the KERNAL buffer `$0277` + count `$C6`. The firmware does
  NOT chunk — keep batches ≤ 10 chars (buffer size). RUN/STOP is not a buffer
  char: poke `$91 = $7F` via `DMAWRITE` (0xFF06), repeated to win the race
  against the KERNAL restoring it (the vendor web UI does the same). The
  vendor web UI itself types via `writemem $0277`, so this is the sanctioned
  mechanism. Works only for KERNAL-read input, not matrix-scanning games —
  upgrade to `machine:input` (CIA1-level, in upstream 3.15 beta) once the
  official firmware ships it; probe `PUT /v1/machine:input` after firmware
  updates.
- **Telnet menu (TCP :23)**: firmware `screen_vt100.cc` emits exactly: `ESC c`
  (RIS), `ESC[y;xH`, a fixed SGR set (`0;3X` + `;1`/`;2`, `7`/`27`), `ESC(0`
  / `ESC(B` charset switches, `ESC[2J`, `ESC[r`. Screen is fixed **60×24**.
  Colors are C64 colors round-tripped through ANSI — term.c inverts the
  firmware's `set_color` table back to VIC colors. Input parsing
  (`keyboard_vt100.cc`) wants xterm-style: `ESC[A-D`, `ESC[N~` (old-style
  F-keys: 11-15 = F1-F5, 17-19 = F6-F8), bare `ESC` = back. The menu overlay
  is not in the VIC stream; telnet is the only remote view of it.
- Screen RAM is remotely readable: `GET /v1/machine:readmem?address=0400&
  length=1000` — used to close the loop when testing typed input.

## Dev workflow (no hardware needed)

- `tools/mockstream.py [ip] [port] [secs]` sends synthetic video (color bars
  + sweep line) and audio (440 Hz tone) in the exact wire format.
- `./c64uv --no-start` = listen-only viewer against the mock.
- `./c64uv --dump f.ppm` (headless single-frame grab) and `--term-test`
  (headless menu-screen dump) are the two self-verification modes.
- `--verbose` logs fps / packet counts / gaps / audio queue depth every 5 s.

## Roadmap

1. Tests + CI (next): term.c parser fed captured session bytes, PETSCII
   mapping table, frame assembler against mockstream packets.
2. `machine:input` keyboard upgrade when official firmware ships it.
3. Convenience hotkeys: mount .d64, run .prg, reset, menu button.
