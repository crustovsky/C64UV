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

# ---------------------------------------------------------------- mock stream
# mockstream -> c64uv --dump, then verify geometry, palette, nibble order.

python3 tools/mockstream.py 127.0.0.1 11000 10 >/dev/null &
pids+=($!)
sleep 0.3
timeout 8 ./c64uv --no-start --dump "$out/frame.ppm"

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

python3 tests/fakeultimate.py 127.0.0.42 8064 "$out/disc.log" &
pids+=($!)
python3 -m http.server 8064 --bind 127.0.0.99 >/dev/null 2>&1 &
pids+=($!)
sleep 0.3
C64U_DISCOVER_NET=127.0.0.0 C64U_DISCOVER_PORT=8064 \
    timeout 30 ./c64uv --discover > "$out/disc.out"
grep -q "127.0.0.42.*Ultimate 64.*fakeultimate" "$out/disc.out"
! grep -q "127.0.0.99" "$out/disc.out"
echo "discovery test passed"
