#!/usr/bin/env python3
"""A scripted virtual gamepad through /dev/uinput (no dependencies).

Presents itself as an Xbox 360 pad, which SDL maps out of the box, then
plays the steps given on the command line:

    virtualpad.py wait=2 a=1 a=0 left=1 a=1 a=0 left=0 stick=32767,0 back=1

name=1/0 presses/releases a button (a b x y back start up down left right;
the last four are the d-pad), stick=X,Y moves the left stick, wait=S
sleeps. A step prefixed "2:" goes to a second pad (created when any step
names it), and unplug=1 removes a pad mid-script. Every step is followed by
a short pause so the viewer sees each state. Used by tests/integration.sh when /dev/uinput is writable.
"""
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
# X and Y follow the xpad driver (BTN_X = 0x133, BTN_Y = 0x134), which is
# what SDL's mapping for this pad expects
BUTTONS = {"a": 0x130, "b": 0x131, "x": 0x133, "y": 0x134, "lb": 0x136,
           "rb": 0x137, "back": 0x13A, "start": 0x13B, "guide": 0x13C,
           "ls": 0x13D, "rs": 0x13E}
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ = 0, 1, 2, 3, 4, 5
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11
HAT = {"left": (ABS_HAT0X, -1), "right": (ABS_HAT0X, 1),
       "up": (ABS_HAT0Y, -1), "down": (ABS_HAT0Y, 1)}

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_SETUP, UI_ABS_SETUP = 0x405C5503, 0x401C5504
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502


def emit(fd, etype, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, etype, code, value))
    os.write(fd, struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))


def make_pad(name):
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for code in BUTTONS.values():
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    for code, lo, hi in ((ABS_X, -32768, 32767), (ABS_Y, -32768, 32767),
                         (ABS_RX, -32768, 32767), (ABS_RY, -32768, 32767),
                         (ABS_Z, 0, 255), (ABS_RZ, 0, 255),
                         (ABS_HAT0X, -1, 1), (ABS_HAT0Y, -1, 1)):
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        # uinput_abs_setup: code, pad, then input_absinfo
        fcntl.ioctl(fd, UI_ABS_SETUP,
                    struct.pack("HHiiiiii", code, 0, 0, lo, hi, 0, 0, 0))
    # uinput_setup: bus USB, Microsoft X-Box 360 pad
    fcntl.ioctl(fd, UI_DEV_SETUP,
                struct.pack("HHHH80sI", 3, 0x045E, 0x028E, 0x0114, name, 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def main():
    steps = sys.argv[1:]
    fds = [make_pad(b"c64uv test pad")]
    if any(s.startswith("2:") for s in steps):
        time.sleep(0.7)  # the viewer must see the first pad first
        fds.append(make_pad(b"c64uv test pad 2"))
    try:
        for step in steps:
            fd = fds[0]
            if step.startswith("2:"):
                fd, step = fds[1], step[2:]
            name, _, val = step.partition("=")
            if name == "wait":
                time.sleep(float(val))
                continue
            if name == "unplug":  # this pad goes away, the other stays
                fcntl.ioctl(fd, UI_DEV_DESTROY)
                os.close(fd)
                fds[fds.index(fd)] = None
                time.sleep(0.3)
                continue
            if name == "stick":
                x, y = (int(v) for v in val.split(","))
                emit(fd, EV_ABS, ABS_X, x)
                emit(fd, EV_ABS, ABS_Y, y)
            elif name in HAT:
                axis, direction = HAT[name]
                emit(fd, EV_ABS, axis, direction if int(val) else 0)
            else:
                emit(fd, EV_KEY, BUTTONS[name], int(val))
            time.sleep(0.15)
    finally:
        for fd in fds:
            if fd is not None:
                fcntl.ioctl(fd, UI_DEV_DESTROY)
                os.close(fd)


if __name__ == "__main__":
    main()
