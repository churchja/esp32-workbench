#!/usr/bin/env python3
"""
Tests for usbwatch's arrival detection.

This file exists because --wait shipped broken twice, both times reporting
success for the wrong reason:

    v1  "is any device present?"      -> fired on Bluetooth / paired-audio ports
    v2  "is any USB device present?"  -> fired on a board already plugged in

Each fix patched the trigger that had been observed rather than asking what
--wait means. It means: a device ARRIVED that was not there before.

Run:  python3 tools/test_usbwatch.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from usbwatch import new_usb_devices  # noqa: E402

FAIL = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


class P:
    def __init__(self, vid=None, pid=None):
        self.vid, self.pid = vid, pid


# Real device shapes from this session.
S2   = P(0x303A, 0x0002)   # ESP32-S2 native CDC
QTPY = P(0x239A, 0x8111)   # tinyuf2
CH340 = P(0x1A86, 0x7523)  # the bridge board we are waiting for
BT   = P()                 # Bluetooth / debug-console: no USB identity

print("arrival, not presence")

check("nothing attached, nothing arrives",
      new_usb_devices({}, {}), {})

check("a board arriving on an empty bus is detected",
      list(new_usb_devices({}, {"/dev/a": CH340})), ["/dev/a"])

# v2's bug: a board already attached must NOT count as an arrival.
base = {"/dev/s2": S2}
check("a board already attached is NOT an arrival",
      new_usb_devices(base, base), {})
check("a SECOND board arriving alongside it IS",
      list(new_usb_devices(base, {"/dev/s2": S2, "/dev/ch": CH340})), ["/dev/ch"])

# v1's bug: non-USB ports must never trigger anything.
bt_base = {"/dev/bt": BT, "/dev/audio": BT}
check("non-USB ports present at start are not arrivals",
      new_usb_devices(bt_base, bt_base), {})
check("a non-USB port APPEARING is still not an arrival",
      new_usb_devices(bt_base, {**bt_base, "/dev/bt2": BT}), {})
check("a USB board appearing among them is",
      list(new_usb_devices(bt_base, {**bt_base, "/dev/ch": CH340})), ["/dev/ch"])

print("\nre-enumeration counts as arrival (the native-USB case)")

# An ESP32-S2 that reboots comes back on a DIFFERENT path under a DIFFERENT
# VID/PID. That is a genuine arrival -- it is how we caught the board coming
# back after being invisible.
check("same board, new path, is an arrival",
      list(new_usb_devices({"/dev/usbmodem01": S2},
                           {"/dev/usbmodem1101": QTPY})), ["/dev/usbmodem1101"])

print("\ndisappearance is not arrival")
check("a board going away triggers nothing",
      new_usb_devices({"/dev/ch": CH340}, {}), {})

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all usbwatch tests passed")
