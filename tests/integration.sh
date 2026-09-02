#!/usr/bin/env bash
# End-to-end tests without hardware. Each section is independent; the mock
# stream test exercises the UDP pipeline, the others run against the fake
# REST server (tests/fakeultimate.py).
set -euo pipefail
cd "$(dirname "$0")/.."

out=$(mktemp -d)
pids=()
cleanup() {
    rm -rf "$out"
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
}
trap cleanup EXIT

# Off the default 11000/11001 so a viewer the user left running cannot
# collide with the suite.
VPORT=21000

# ---------------------------------------------------------------- mock stream
# mockstream -> c64uv --dump, then verify geometry, palette, nibble order.

python3 tools/mockstream.py 127.0.0.1 "$VPORT" 10 >/dev/null &
pids+=($!)
sleep 0.3
timeout 8 ./c64uv --no-start --port "$VPORT" --dump "$out/frame.ppm"

python3 - "$out/frame.ppm" <<'EOF'
import sys
pal = [0xFF000000,0xFFFFFFFF,0xFF813338,0xFF75CEC8,0xFF8E3C97,0xFF56AC4D,
       0xFF2E2C9B,0xFFEDF171,0xFF8E5029,0xFF553800,0xFFC46C71,0xFF4A4A4A,
       0xFF7B7B7B,0xFFA9FF9F,0xFF706DEB,0xFFB2B2B2]
with open(sys.argv[1], "rb") as f:
    assert f.readline().strip() == b"P6"
    w, h = map(int, f.readline().split())
    f.readline()
    data = f.read()
assert (w, h) == (384, 272), (w, h)
def px(x, y):
    o = (y * w + x) * 3
    return (data[o], data[o + 1], data[o + 2])
def rgb(c):
    return (c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF)
errors = 0
for y in (10, 100, 250):  # mock pattern: 16 bars of 24px, plus a sweep line
    for bar in range(16):
        got = px(bar * 24 + 12, y)
        if got != rgb(pal[bar]) and got != (255, 255, 255):
            print(f"MISMATCH y={y} bar={bar}: {got}")
            errors += 1
# bar boundary proves nibble order (bar0 black | bar1 white)
errors += px(23, 10) != (0, 0, 0) or px(24, 10) != (255, 255, 255)
sys.exit(1 if errors else 0)
EOF
echo "mock stream test passed"

# ----------------------------------------------------------------- discovery
# A fake Ultimate on one loopback address must be found; a plain web server
# on another must be rejected (real subnets are full of port-80 responders).

python3 tests/fakeultimate.py 127.0.0.42 8064 "$out/disc.log" 8065 &
pids+=($!)
python3 -m http.server 8064 --bind 127.0.0.99 >/dev/null 2>&1 &
pids+=($!)
sleep 0.3
C64U_DISCOVER_NET=127.0.0.0 C64U_DISCOVER_PORT=8064 \
    timeout 30 ./c64uv --discover > "$out/disc.out"
grep -q "127.0.0.42.*Ultimate 64.*fakeultimate" "$out/disc.out"
! grep -q "127.0.0.99" "$out/disc.out"
echo "discovery test passed"

# ----------------------------------------------------------------- multicast
# Two viewers join the video group and must both assemble the same stream;
# proves IP_ADD_MEMBERSHIP plus the shared SO_REUSEADDR port binding.

python3 tools/mockstream.py 239.0.1.64 "$VPORT" 10 >/dev/null &
pids+=($!)
sleep 0.3
timeout 8 ./c64uv --no-start --multicast --port "$VPORT" --dump "$out/mc1.ppm" &
v1=$!
timeout 8 ./c64uv --no-start --multicast --port "$VPORT" --dump "$out/mc2.ppm"
wait "$v1"
for f in "$out/mc1.ppm" "$out/mc2.ppm"; do
    head -2 "$f" | grep -qx "384 272"
done
echo "multicast test passed"

# ------------------------------------------------------- REST keepalive/probe
# Full keepalive cycle against the fake server: stream start with the right
# destination, plus the one-time machine:input capability probe.

python3 tools/mockstream.py 127.0.0.1 "$VPORT" 10 >/dev/null &
pids+=($!)
sleep 0.3
timeout 8 ./c64uv --host 127.0.0.42:8064 --dest 127.0.0.1 \
    --port "$VPORT" --dump "$out/rest.ppm" 2> "$out/rest.err"
grep -q "GET /v1/machine:input" "$out/disc.log"
grep -q "PUT /v1/streams/video:start?ip=127.0.0.1:$VPORT" "$out/disc.log"
grep -q "machine:input available" "$out/rest.err"
echo "rest keepalive/probe test passed"

# ------------------------------------------------- machine control + password
# --do issues exactly one PUT /v1/machine:<action>; X-Password must reach the
# wire from both the flag and the environment.

timeout 8 ./c64uv --host 127.0.0.42:8064 --password sekret --do reset
grep -q "PUT /v1/machine:reset pw=sekret" "$out/disc.log"
C64U_PASSWORD=envpw timeout 8 ./c64uv --host 127.0.0.42:8064 --do menu
grep -q "PUT /v1/machine:menu_button pw=envpw" "$out/disc.log"
timeout 8 ./c64uv --host 127.0.0.42:8064 --do pause
grep -q "PUT /v1/machine:pause$" "$out/disc.log"
./c64uv --host 127.0.0.42:8064 --do frobnicate 2>/dev/null && exit 1
echo "machine control test passed"

# ------------------------------------------------------------------ file run
# --run must pick the runner from the extension and, for DMA runs, park the
# configured cartridge first and restore it only after the readiness gate.

printf '\x01\x08\x0b\x08\x0a\x00\x99\x22\x48\x49\x22\x00\x00\x00' > "$out/hi.prg"
: > "$out/disc.log"
timeout 30 ./c64uv --host 127.0.0.42:8064 --run "$out/hi.prg"
python3 - "$out/disc.log" <<'EOF'
import sys
log = open(sys.argv[1]).read().splitlines()
want = ["GET /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge",
        "PUT /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge?value=",
        "POST /v1/runners:run_prg body=14",
        "GET /v1/machine:readmem?address=00CC&length=1",
        "PUT /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge?value=Retro%20Replay"]
i = 0
for line in log:
    if i < len(want) and line == want[i]:
        i += 1
assert i == len(want), f"missing/mis-ordered step {i}: {want[i]}\nlog: {log}"
EOF
head -c 64 /dev/urandom > "$out/game.crt"
timeout 8 ./c64uv --host 127.0.0.42:8064 --run "$out/game.crt"
grep -q "POST /v1/runners:run_crt body=64" "$out/disc.log"
head -c 32 /dev/urandom > "$out/tune.sid"
timeout 30 ./c64uv --host 127.0.0.42:8064 --run "$out/tune.sid"
grep -q "POST /v1/runners:sidplay body=32" "$out/disc.log"
touch "$out/note.txt"
./c64uv --host 127.0.0.42:8064 --run "$out/note.txt" 2>/dev/null && exit 1
echo "file run test passed"

# ------------------------------------------------------------- disk images
# A .d64 goes out as one RUN_IMG frame on the DMA socket (24-bit length),
# with the same cartridge parking and readiness gate as a program run; with
# a password set the connection authenticates first. Other image types are
# mounted over REST without touching the machine.

head -c 174848 /dev/urandom > "$out/disk.d64"
: > "$out/disc.log"
C64U_DMA_PORT=8065 timeout 30 ./c64uv --host 127.0.0.42:8064 --run "$out/disk.d64"
python3 - "$out/disc.log" <<'EOF'
import sys
log = open(sys.argv[1]).read().splitlines()
want = ["PUT /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge?value=",
        "DMA cmd=FF0B len=174848",
        "GET /v1/machine:readmem?address=00CC&length=1",
        "PUT /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge?value=Retro%20Replay"]
i = 0
for line in log:
    if i < len(want) and line == want[i]:
        i += 1
assert i == len(want), f"missing/mis-ordered step {i}: {want[i]}\nlog: {log}"
assert not any("drives" in l for l in log), log
EOF
: > "$out/disc.log"
C64U_DMA_PORT=8065 C64U_PASSWORD=envpw timeout 30 ./c64uv --host 127.0.0.42:8064 --run "$out/disk.d64"
python3 - "$out/disc.log" <<'EOF'
import sys
log = open(sys.argv[1]).read().splitlines()
assert log.index("DMA cmd=FF1F pw=envpw") < log.index("DMA cmd=FF0B len=174848"), log
EOF
head -c 819200 /dev/urandom > "$out/disk.d81"
: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --run "$out/disk.d81"
grep -q "POST /v1/drives/a:mount?type=d81 body=819200" "$out/disc.log"
grep -q "Cartridge" "$out/disc.log" && exit 1 # a plain mount parks nothing
echo "disk image test passed"

# ---------------------------------------------------------------------- help
# --help must print the shared binding table (same rows the F10 overlay
# renders), so a missing row here means the overlay lost it too.

./c64uv --help 2> "$out/help.out" || true
grep -q "F10" "$out/help.out"
grep -q "Ctrl+Shift+R" "$out/help.out"
grep -q "RUN/STOP" "$out/help.out"
echo "help test passed"

# ------------------------------------------------- windowed async discovery
# A bare windowed start (dummy video driver) must open without blocking,
# discover the fake Ultimate in the background, and start the stream.

: > "$out/disc.log"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    C64U_DISCOVER_NET=127.0.0.0 C64U_DISCOVER_PORT=8064 \
    ./c64uv --dest 127.0.0.1 --port "$VPORT" --no-audio 2> "$out/async.err" &
cpid=$!
for _ in $(seq 1 150); do
    grep -q "PUT /v1/streams/video:start" "$out/disc.log" 2>/dev/null && break
    sleep 0.2
done
kill "$cpid" 2>/dev/null
wait "$cpid" 2>/dev/null || true
grep -q "using 127.0.0.42" "$out/async.err"
grep -q "PUT /v1/streams/video:start?ip=127.0.0.1:$VPORT" "$out/disc.log"
echo "async discovery test passed"
