# C64UV — C64 Ultimate Viewer

Linux desktop viewer for the Commodore 64 Ultimate: streams the machine's video
(and later audio) into a window and will eventually pass keyboard input back, so
the machine can be used without a second monitor. Broader context: the machine
is used to teach BASIC to a 6-year-old on real hardware; this tool is for lesson
prep on the laptop. Later possible additions: disk image mounting, running PRGs,
reset/menu buttons — all available via the same REST API.

## Device

- C64 Ultimate (Commodore-branded). **Wired: `192.168.8.236`** (use this — it's
  the streaming interface and the viewer's default host); WiFi: `192.168.8.173`.
  Both DHCP, so they can move.
- Firmware **1.1.0**, FPGA 122, core 1.49 (checked 2026-08-06 via `GET /v1/info`)
- Drive A: 1541, bus 8, enabled. Drive B and SoftIEC disabled.

## Hard-won findings (keep updated)

- **Streams only work over the Ultimate's wired Ethernet port.** With the device
  on WiFi, `PUT /v1/streams/video:start` returns HTTP 500
  `"No Operational Network Interface"`. The VIC/audio streams are generated in
  the FPGA and go out the Ethernet MAC directly. → Plug in a cable to stream.
- **No `machine:input` endpoint in firmware 1.1.0** (returns 404). That endpoint
  (CIA1-level key/joystick injection, ~13 ms latency) exists only in Gideon's
  upstream 3.15 beta for Ultimate 64 and hasn't landed in the Commodore firmware
  yet. Until it does, keyboard passthrough must use the KERNAL keyboard buffer:
  `PUT /v1/machine:writemem` to `$0277` (buffer) + `$C6` (count). Works for
  BASIC/KERNAL input only — not games, not the Ultimate menu. Re-probe
  `machine:input` after each firmware update.
- **This laptop's routing quirk:** Tailscale (`table 52`) claims
  `192.168.8.0/24`, so `ip route get 192.168.8.173` says `tailscale0` even
  though eth0 (`192.168.8.197`) and wlan0 (`192.168.8.124`) are on that LAN
  directly. Incoming stream UDP is unaffected (`rp_filter` is loose: eth0=2).
  Use `192.168.8.197` as the stream destination.
- **`streams/*:start` fails with HTTP 404 `"Network Host Resolve Error"` if the
  destination IP is not in the Ultimate's ARP table.** It doesn't ARP on
  demand. A plain UDP poke from the viewer does NOT fix it on this laptop,
  because Tailscale's `table 52` (rule 5270, before main) routes the whole
  home LAN through the tunnel — someone on the tailnet advertises
  `192.168.8.0/24` and this machine runs accept-routes. The packet then
  reaches the Ultimate from the router, not from eth0's MAC. Working fix
  (implemented): `ping -n -q -c1 -W1 -I eth0 <ultimate>` — `ping` may force
  the egress interface without root — run before every keepalive start, which
  also keeps the Ultimate's ARP entry fresh. The `ip=<addr>:<port>` combined
  syntax works fine; there is no separate `port` parameter.
- **Audio queue needs active latency control.** Input and output rates match
  exactly, so whatever queue builds during startup (~170 ms observed) persists
  forever. The viewer runs a servo on `SDL_SetAudioStreamFrequencyRatio`
  (±2 % max, adjusted 1×/s) steering toward a 60 ms target.
- Default stream destinations in device config are multicast
  (`239.0.1.64:11000` video, `239.0.1.65:11001` audio); we override per-start
  with a unicast `ip[:port]` query param instead.
- Firewall status on this laptop unverified (nft list needs sudo). If packets
  don't arrive once Ethernet is plugged in, check nftables first.

## Protocol reference

REST API docs: <https://1541u-documentation.readthedocs.io/en/latest/api/api_calls.html>
Stream format: <https://1541u-documentation.readthedocs.io/en/latest/data_streams.html>

- Start/stop: `PUT /v1/streams/video:start?ip=<dest>[:port]`, `...:stop`.
  Idempotent — re-send start every ~5 s as keepalive (survives resets).
- **Video** UDP :11000 — 780-byte datagrams: 12-byte header, all LE:
  seq u16, frame u16, line u16 (bit 15 = last packet of frame), pixels-per-line
  u16 (384), lines-per-packet u8 (4), bits-per-pixel u8 (4), encoding u16 (0).
  Payload: 4 lines × 384 px × 4 bpp = 768 bytes. **Low nibble = leftmost
  pixel** (verified in c64stream source). PAL 384×272 @ 50 Hz (68 pkts/frame),
  NTSC 384×240 @ 60 Hz. Values are VIC color indices 0–15.
- **Audio** UDP :11001 — 2-byte seq + 192 stereo s16le frames (770 bytes).
  ~47983 Hz PAL / ~47940 Hz NTSC.

## Stack & architecture

C + SDL3 (video/audio/input) + libcurl (REST). Arch packages: `sdl3`, `curl`.

```
UDP :11000 → frame assembler → palette LUT → SDL streaming texture → window
UDP :11001 → SDL_AudioStream (resample + latency servo → 60 ms) → device
SDL key events → C64 matrix map → writemem $0277 (machine:input later) [not yet]
keepalive thread → ARP prime (ping -I) + PUT streams/{video,audio}:start every 5 s
```

- `src/main.c` — everything so far. `Makefile` — `make` then `./c64uv`.
- `tools/mockstream.py` — sends synthetic PAL frames in the exact wire format
  to 127.0.0.1:11000; use with `./c64uv --no-start` to test without hardware.
- `./c64uv --dump frame.ppm` exits after writing the first complete frame —
  headless end-to-end test.

## Status log

- **2026-08-06** Stack chosen; device probed; video viewer written and verified
  end-to-end against `tools/mockstream.py` (correct palette per test bar).
- **2026-08-06 (later)** Ethernet plugged in (wired IP `192.168.8.236`). Live
  video confirmed: BASIC boot screen decoded pixel-perfect, ~50 fps, no packet
  loss to eth0 (`192.168.8.197`). Discovered + fixed the ARP-priming
  requirement (see findings). Video milestone done.
- **2026-08-06 (evening)** Audio milestone done: second UDP stream into
  SDL_AudioStream, latency servo converges to ~60 ms, zero packet gaps.
  Root-caused the Tailscale hairpin (see findings) — auto-detection now picks
  the LAN interface by subnet match (wired preferred) and primes ARP via
  `ping -I`, so plain `./c64uv` works with no flags. Repo:
  github.com/crustovsky/C64UV (private).

## Roadmap

1. ✅ Video viewer
2. ✅ Audio
3. Keyboard via `writemem` $0277; switch to `machine:input` when firmware ships it
4. Convenience: mount .d64 / run .prg / reset / menu_button hotkeys
