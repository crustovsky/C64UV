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

python3 tests/fakeultimate.py 127.0.0.42 8064 "$out/disc.log" 8065 8067 &
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

# ------------------------------------------------------------ stored images
# With --store the image is FTP-uploaded into the folder (replacing a
# same-named file), mounted by path read-write, and autostarted by the
# viewer: reset (with cartridge parking), readiness gate, LOAD"*",8,1 and
# RUN typed in <= 8-byte KEYB batches.

: > "$out/disc.log"
C64U_DMA_PORT=8065 C64U_FTP_PORT=8067 timeout 60 \
    ./c64uv --host 127.0.0.42:8064 --store "/Usb0/my games" --run "$out/disk.d64"
python3 - "$out/disc.log" <<'EOF'
import sys
log = open(sys.argv[1]).read().splitlines()
want = ["PUT /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge?value=",
        "FTP STOR /Usb0/my games/disk.d64 len=174848",
        "PUT /v1/drives/a:mount?image=/Usb0/my%20games/disk.d64&mode=readwrite",
        "PUT /v1/machine:reset",
        "GET /v1/machine:readmem?address=00CC&length=1",
        "DMA cmd=FF03 len=8",
        "DMA cmd=FF03 len=4",
        "GET /v1/machine:readmem?address=00CC&length=1",
        "DMA cmd=FF03 len=4",
        "PUT /v1/configs/C64%20and%20Cartridge%20Settings/Cartridge?value=Retro%20Replay"]
i = 0
for line in log:
    if i < len(want) and line == want[i]:
        i += 1
assert i == len(want), f"missing/mis-ordered step {i}: {want[i]}\nlog: {log}"
assert not any("FF0B" in l for l in log), log
EOF
echo "stored image test passed"

# ------------------------------------------------------------ type + screen
# --type goes out as KEYB frames in <= 8-byte batches with \n as RETURN;
# --screen prints the 40x25 text screen read from $0400.

: > "$out/disc.log"
C64U_DMA_PORT=8065 timeout 10 ./c64uv --host 127.0.0.42:8064 --type 'PRINT "HI"\n'
grep -c "DMA cmd=FF03 len=8" "$out/disc.log" | grep -q '^1$'
grep -q "DMA cmd=FF03 len=3$" "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --screen > "$out/screen.txt"
test "$(wc -l < "$out/screen.txt")" = 25
head -1 "$out/screen.txt" | grep -q '^READY\. *$'
echo "type/screen test passed"

# ------------------------------------------------------------- matrix keys
# --keys and --type --matrix go through machine:input as press/release
# pairs (never tap, which overlaps); a bad key name is a usage error that
# sends nothing.

matrix_log() { # the logged machine:input bodies must be exactly these strokes
    python3 - "$out/disc.log" "$@" <<'PYEOF'
import sys
log = [l.split(" ", 3)[3] for l in open(sys.argv[1]).read().splitlines()
       if l.startswith("POST /v1/machine:input")]
k = '{"events":[{"kind":"keyboard","inputs":[%s],"transition":"%s"}]}'
want = [k % (",".join('"%s"' % n for n in stroke.split("+")), t)
        for stroke in sys.argv[2:] for t in ("press", "release")]
assert log == want, log
PYEOF
}
: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --keys "f1,left_shift+a:200 run_stop"
matrix_log f1 left_shift+a run_stop
: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --matrix --type 'A"\n' --screen > "$out/screen.txt"
matrix_log a left_shift+2 return
test "$(wc -l < "$out/screen.txt")" = 25
grep -q "DMA cmd" "$out/disc.log" && exit 1 # nothing through the buffer
: > "$out/disc.log"
rc=0; ./c64uv --host 127.0.0.42:8064 --keys "f1,enter" 2>/dev/null || rc=$?
test "$rc" = 2
! grep -q 'machine:input' "$out/disc.log"
echo "matrix keys test passed"

# -------------------------------------------------------------- menu screen
# --menu-screen prints the open menu as text with the selected row marked;
# with the menu closed (404) it fails. --keys drives the menu.

if timeout 10 ./c64uv --host 127.0.0.42:8064 --menu-screen 2>/dev/null; then
    echo "--menu-screen succeeded with the menu closed"; exit 1
fi
timeout 8 ./c64uv --host 127.0.0.42:8064 --do menu
timeout 10 ./c64uv --host 127.0.0.42:8064 --menu-screen > "$out/menu.txt"
test "$(wc -l < "$out/menu.txt")" = 25
head -1 "$out/menu.txt" | grep -q '^  +--------------------------------------+$'
grep -q '^> |.Disk File Browser' "$out/menu.txt"
timeout 10 ./c64uv --host 127.0.0.42:8064 --keys cursor_up_down
timeout 10 ./c64uv --host 127.0.0.42:8064 --menu-screen > "$out/menu.txt"
grep -q '^> |.Assembly 64 Query' "$out/menu.txt"
grep -q '^  |.Disk File Browser' "$out/menu.txt"
timeout 10 ./c64uv --host 127.0.0.42:8064 --keys run_stop # leaves the menu
timeout 10 ./c64uv --host 127.0.0.42:8064 --menu-screen 2>/dev/null && exit 1
echo "menu screen test passed"

# -------------------------------------------------------------------- mouse
# --mouse-send turns its items into machine:input mouse events; a bad item
# is a usage error that sends nothing.

: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --mouse-send "move:40:-10 left, move:300:0"
grep -q '{"events":\[{"kind":"mouse","move":{"x":40,"y":-10}}\]}' "$out/disc.log"
grep -q '{"kind":"mouse","inputs":\["left"\],"transition":"tap"}' "$out/disc.log"
grep -q '{"kind":"mouse","path":\[\[100,0\],\[100,0\],\[100,0\]\]}' "$out/disc.log"
: > "$out/disc.log"
rc=0; ./c64uv --host 127.0.0.42:8064 --mouse-send "move:5:5 click" 2>/dev/null || rc=$?
test "$rc" = 2
! grep -q 'machine:input' "$out/disc.log"
echo "mouse send test passed"

# ------------------------------------------------------- windowed, scripted
# The window itself, driven through the C64U_TEST_EVENTS hook under SDL's
# dummy video driver. Ctrl+G grabs the mouse as a 1351 (the fake lists a
# mouse in machine:input): motion and buttons go out as mouse events, and
# stop when released. F9 then opens the REST menu view (one menu_button
# press), cursor down reaches the menu as a key, F9 closes it (the second
# press). Ctrl+M (the third) attaches the same view, RUN/STOP leaves the
# menu on the machine and the view follows without another press; opened
# once more (the fourth), quitting closes it on the machine (the fifth).

viewer() { # viewer <logname> <events> [flags...]: runs until the script ends
    local name=$1 events=$2; shift 2
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy C64U_TEST_EVENTS="$events" \
        timeout 30 ./c64uv --host 127.0.0.42:8064 --dest 127.0.0.1 \
        --port "$VPORT" --no-audio "$@" 2> "$out/$name.err" &
    vpid=$!
    pids+=($vpid)
}
: > "$out/disc.log"
viewer win "ready wait=300 key=ctrl+p wait=200 key=ctrl+p wait=200 \
key=ctrl+g wait=200 move=12,-4 wait=300 \
button=left:1 wait=200 button=left:0 wait=300 key=ctrl+g wait=200 move=50,50 \
wait=300 key=f9 wait=700 key=down wait=400 key=f9 wait=600 key=ctrl+m \
wait=700 key=escape wait=700 key=f9 wait=700"
for _ in $(seq 1 100); do
    [ "$(grep -c 'menu view: showing' "$out/win.err")" = 3 ] && break
    sleep 0.2
done
sleep 0.3
test "$(grep -c 'PUT /v1/machine:menu_button' "$out/disc.log")" = 4
kill -TERM $vpid; wait $vpid || true
grep -q "mouse grabbed: 1351 mouse in port 1" "$out/win.err"
# Ctrl+P twice: pause, then resume (the toggle follows the answers)
python3 - "$out/disc.log" <<'PYEOF2'
import sys
log = [l for l in open(sys.argv[1]).read().splitlines()
       if l.startswith("PUT /v1/machine:pause") or l.startswith("PUT /v1/machine:resume")]
assert log == ["PUT /v1/machine:pause", "PUT /v1/machine:resume"], log
PYEOF2
grep -q '{"kind":"mouse","move":{"x":12,"y":-4}}' "$out/disc.log"
grep -q '{"kind":"mouse","inputs":\["left"\],"transition":"press"}' "$out/disc.log"
grep -q '{"kind":"mouse","inputs":\["left"\],"transition":"release"}' "$out/disc.log"
! grep -q '"x":50' "$out/disc.log" # released: motion stays on the PC
grep -q "machine:menu_screen available" "$out/win.err"
grep -q "menu view closed" "$out/win.err"
grep -q '"inputs":\["cursor_up_down"\],"transition":"press"' "$out/disc.log"
grep -q "menu view: the menu closed on the machine" "$out/win.err"
test "$(grep -c 'PUT /v1/machine:menu_button' "$out/disc.log")" = 5 # quit closed it
timeout 10 ./c64uv --host 127.0.0.42:8064 --menu-screen 2>/dev/null && exit 1
echo "windowed mouse + menu view test passed"

# The same grab on firmware without mouse events (and with --mouse-joy on
# any): the mouse steers the joystick, on port 1 for GEOS's JOYSTICK
# driver, held while it moves and released after --mouse-hold.
curl -s -X PUT "http://127.0.0.42:8064/test/mouse?value=0" >/dev/null
: > "$out/disc.log"
viewer mjoy "ready wait=300 key=ctrl+g wait=200 move=30,0 wait=500 \
button=left:1 wait=200 button=left:0 wait=400"
for _ in $(seq 1 100); do
    grep -q '"inputs":\["fire"\],"transition":"release"' "$out/disc.log" && break
    sleep 0.2
done
kill -TERM $vpid; wait $vpid || true
curl -s -X PUT "http://127.0.0.42:8064/test/mouse?value=1" >/dev/null
grep -q "mouse grabbed: joystick on port 1" "$out/mjoy.err"
grep -q '"port":1,"inputs":\["right"\],"transition":"press"' "$out/disc.log"
grep -q '"port":1,"inputs":\["right"\],"transition":"release"' "$out/disc.log"
grep -q '"port":1,"inputs":\["fire"\],"transition":"press"' "$out/disc.log"
! grep -q '"kind":"mouse"' "$out/disc.log"
: > "$out/disc.log"
viewer mjoy2 "ready wait=300 key=ctrl+g wait=200 move=0,30 wait=500" --mouse-joy --joy-port 2
for _ in $(seq 1 100); do
    grep -q '"inputs":\["down"\],"transition":"release"' "$out/disc.log" && break
    sleep 0.2
done
kill -TERM $vpid; wait $vpid || true
grep -q '"port":2,"inputs":\["down"\],"transition":"press"' "$out/disc.log"
echo "windowed mouse joystick test passed"

# ---------------------------------------------------------------- joystick
# --joy presses and releases the named inputs on the given control port
# through machine:input; a bad input name is a usage-level refusal.

: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --joy 1:left,fire
grep -q 'POST /v1/machine:input' "$out/disc.log"
grep -q '"port":1,"inputs":\["left","fire"\],"transition":"press"' "$out/disc.log"
grep -q '"port":1,"inputs":\["left","fire"\],"transition":"release"' "$out/disc.log"
: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --joy-port 1 --joy up
grep -q '"port":1,"inputs":\["up"\],"transition":"press"' "$out/disc.log"
: > "$out/disc.log"
if timeout 10 ./c64uv --host 127.0.0.42:8064 --joy jump 2>/dev/null; then
    echo "--joy accepted a bad input name"; exit 1
fi
! grep -q 'machine:input' "$out/disc.log"
# Joystick Swapper on "Swapped" (the machine's default): the API's ports
# trade places, so the C64's port 2 goes out as API port 1.
curl -s -X PUT "http://127.0.0.42:8064/v1/configs/U64%20Specific%20Settings/Joystick%20Swapper?value=Swapped" >/dev/null
: > "$out/disc.log"
timeout 10 ./c64uv --host 127.0.0.42:8064 --joy 2:fire 2> "$out/swap.err"
grep -q '"port":1,"inputs":\["fire"\],"transition":"press"' "$out/disc.log"
grep -q 'joystick port 2: fire OK' "$out/swap.err"
curl -s -X PUT "http://127.0.0.42:8064/v1/configs/U64%20Specific%20Settings/Joystick%20Swapper?value=Normal" >/dev/null
echo "joystick test passed"

# ------------------------------------------------------------ services off
# After a firmware update the Ultimate answers pings but refuses every TCP
# service. A closed loopback port stands in: each path must say which
# service is off instead of "no response", and discovery must name a
# machine whose wired MAC it saw while port 80 refused.

if ./c64uv --host 127.0.0.44:8064 --do reset 2> "$out/off.err"; then
    echo "--do against a closed port succeeded"; exit 1
fi
grep -q "Web Remote Control service is off" "$out/off.err"
C64U_DMA_PORT=8068 ./c64uv --host 127.0.0.42:8064 --type hi 2> "$out/off.err" && exit 1
grep -q "Ultimate DMA Service is off" "$out/off.err"
./c64uv --host 127.0.0.44 --term-test 2> "$out/off.err" && exit 1
grep -q "Telnet Remote Menu Service is off" "$out/off.err"
printf '%s\n%s\n' \
    "IP address       HW type     Flags       HW address            Mask     Device" \
    "127.0.0.77       0x1         0x2         02:15:41:aa:bb:cc     *        eth0" \
    > "$out/arp.txt"
C64U_ARP_TABLE="$out/arp.txt" C64U_DISCOVER_NET=127.0.0.0 \
    C64U_DISCOVER_PORT=8068 ./c64uv --discover 2> "$out/off.err" && exit 1
grep -q "switched on at 127.0.0.77" "$out/off.err"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout 5 ./c64uv --host 127.0.0.44:8064 --dest 127.0.0.1 \
    --port "$VPORT" --no-audio 2> "$out/off.err" || true
grep -q "stream start: connection refused" "$out/off.err"
echo "services-off test passed"

# ----------------------------------------------------------------- gamepad
# The windowed path with a virtual pad (uinput; skipped where that is not
# writable, e.g. CI containers): buttons, d-pad and stick become joystick
# events on port 2, Back swaps to port 1, unplugging releases what is held.

# A real controller connected to this PC would take the first pad slot, so
# the viewers under test only accept the virtual pad's USB ids.
if [ -w /dev/uinput ]; then
    : > "$out/disc.log"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
        SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x045E/0x028E \
        timeout 30 ./c64uv --host 127.0.0.42:8064 --dest 127.0.0.1 \
        --port "$VPORT" --no-audio 2> "$out/pad.err" &
    padpid=$!
    pids+=($padpid)
    for _ in $(seq 1 80); do # the keepalive thread probes machine:input
        grep -q "machine:input available" "$out/pad.err" && break
        sleep 0.25
    done
    python3 tests/virtualpad.py wait=1.5 a=1 a=0 left=1 a=1 a=0 left=0 \
        stick=32767,0 b=1 stick=0,0 b=0 x=1 x=0 back=1 y=1 wait=0.3
    kill -TERM $padpid; wait $padpid || true
    j='{"kind":"joystick","port":%s,"inputs":\["%s"\],"transition":"%s"}'
    for want in "2 fire press" "2 fire release" "2 left press" \
        "2 left release" "2 right press" "2 up press" "2 right release" \
        "2 up release" "2 fire2 press" "1 fire3 press" "1 fire3 release"; do
        # shellcheck disable=SC2086,SC2059
        grep -q "$(printf "$j" $want)" "$out/disc.log"
    done
    grep -q "joystick on port 1" "$out/pad.err"
    echo "gamepad test passed"

    # Two pads: the first drives port 2, the second port 1; Back on either
    # swaps both, and unplugging one releases only its own port.
    : > "$out/disc.log"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
        SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x045E/0x028E \
        timeout 30 ./c64uv --host 127.0.0.42:8064 --dest 127.0.0.1 \
        --port "$VPORT" --no-audio 2> "$out/pad2.err" &
    padpid=$!
    pids+=($padpid)
    for _ in $(seq 1 80); do
        grep -q "machine:input available" "$out/pad2.err" && break
        sleep 0.25
    done
    python3 tests/virtualpad.py wait=1.5 a=1 2:left=1 a=0 2:left=0 \
        2:back=1 2:back=0 a=1 2:x=1 unplug=1 wait=0.3 2:x=0 wait=0.3
    kill -TERM $padpid; wait $padpid || true
    for want in "2 fire press" "1 left press" "2 fire release" \
        "1 left release" "1 fire press" "2 fire2 press" "1 fire release" \
        "2 fire2 release"; do
        # shellcheck disable=SC2086,SC2059
        grep -q "$(printf "$j" $want)" "$out/disc.log"
    done
    grep -q "drives joystick port 1" "$out/pad2.err"
    grep -q "gamepad on port 1 unplugged" "$out/pad2.err"
    echo "two-gamepad test passed"

    # The REST menu view, driven by the pad: Start opens the menu (one
    # menu_button press, then the screen is polled), the d-pad becomes
    # cursor keys instead of joystick lines, Start closes it again (the
    # second press), and A is fire again.
    : > "$out/disc.log"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
        SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x045E/0x028E \
        timeout 30 ./c64uv --host 127.0.0.42:8064 --dest 127.0.0.1 \
        --port "$VPORT" --no-audio 2> "$out/menu.err" &
    padpid=$!
    pids+=($padpid)
    for _ in $(seq 1 80); do
        grep -q "machine:menu_screen available" "$out/menu.err" && break
        sleep 0.25
    done
    python3 tests/virtualpad.py wait=1.5 start=1 start=0 wait=0.6 down=1 \
        down=0 wait=0.3 start=1 start=0 wait=0.6 a=1 a=0 wait=0.3
    grep -q "menu view: showing the Ultimate menu" "$out/menu.err"
    grep -q "menu view closed" "$out/menu.err"
    test "$(grep -c 'PUT /v1/machine:menu_button' "$out/disc.log")" = 2
    grep -q '"inputs":\["cursor_up_down"\],"transition":"press"' "$out/disc.log"
    ! grep -q '"kind":"joystick".*"down"' "$out/disc.log" # not a joystick there
    grep -q '"port":2,"inputs":\["fire"\],"transition":"press"' "$out/disc.log"
    # quitting with the view open closes the menu on the machine
    python3 tests/virtualpad.py wait=1 start=1 start=0 wait=0.8
    test "$(grep -c 'PUT /v1/machine:menu_button' "$out/disc.log")" = 3
    kill -TERM $padpid; wait $padpid || true
    test "$(grep -c 'PUT /v1/machine:menu_button' "$out/disc.log")" = 4
    timeout 10 ./c64uv --host 127.0.0.42:8064 --menu-screen 2>/dev/null && exit 1
    echo "menu view test passed"
else
    echo "gamepad test skipped (/dev/uinput not writable)"
fi

# ------------------------------------------------------- dead stream link
# The machine answers REST but not a ping out of the interface the stream
# should arrive on (seen for real: the wired interface lost its subnet
# route to an address-conflict false alarm, and everything went over
# WiFi). In a private network namespace with echo replies switched off the
# viewer must say so. Skipped where unprivileged namespaces are not allowed.

if unshare -Urn true 2>/dev/null; then
    cat > "$out/linkdead.sh" <<'LDEOF'
set -e
ip link set lo up
sysctl -qw net.ipv4.icmp_echo_ignore_all=1
python3 tests/fakeultimate.py 127.0.0.42 80 "$1/ld.log" &
fp=$!
sleep 0.4
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 13 ./c64uv \
    --host 127.0.0.42 --dest 127.0.0.1 --port 21000 --no-audio \
    2> "$1/ld.err" || true
kill $fp
LDEOF
    unshare -Urn bash "$out/linkdead.sh" "$out"
    grep -q "answers its REST API but no ping sent out of lo" "$out/ld.err"
    echo "dead stream link test passed"
else
    echo "dead stream link test skipped (no unprivileged namespaces)"
fi

# ------------------------------------------------------- exit with host gone
# A powered-off Ultimate answers nothing: the tarpit accepts connections and
# never replies, which leaves the keepalive thread inside a REST call. A
# quit (SIGTERM = SDL_EVENT_QUIT) must still finish within a couple of
# seconds, or the compositor flags the window as not responding.

python3 - <<'EOF' &
import socket, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.43", 8066)); s.listen()
held = []
while True:
    c, _ = s.accept()
    held.append(c)  # keep it open, say nothing
EOF
pids+=($!)
sleep 0.3
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ./c64uv --host 127.0.0.43:8066 --dest 127.0.0.1 --port "$VPORT" \
    --no-audio 2> "$out/tarpit.err" &
cpid=$!
sleep 2 # the keepalive is now waiting on the tarpit
t0=$(date +%s%N)
kill "$cpid"
wait "$cpid" 2>/dev/null || true
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
echo "exit with the host gone took $ms ms"
[ "$ms" -lt 3000 ]
echo "exit with host gone test passed"

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
