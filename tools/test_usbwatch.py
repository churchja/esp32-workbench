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
import contextlib
import inspect
import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usbwatch  # noqa: E402
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

print("\n--timeout measures a duration, so it must not use wall time")


class ClockExhausted(RuntimeError):
    """The loop polled more times than the test scripted."""


class FakeClock:
    def __init__(self, ticks):
        self.ticks = list(ticks)

    def __call__(self):
        if not self.ticks:
            # A loop that never trips its deadline would spin forever. Raising
            # here turns that into a reported FAIL instead of a traceback that
            # aborts every assertion after it.
            raise ClockExhausted()
        return self.ticks.pop(0)


def run_watch(argv, ticks):
    """Drive main()'s watch loop with a scripted clock and no real devices."""
    real_argv, real_snapshot = sys.argv, usbwatch.snapshot
    sys.argv = ["usbwatch.py"] + argv
    usbwatch.snapshot = lambda include_all: {}
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            return usbwatch.main(clock=FakeClock(ticks))
    except ClockExhausted:
        return "never tripped the deadline (ran past the scripted clock)"
    finally:
        sys.argv, usbwatch.snapshot = real_argv, real_snapshot


# --interval 0 keeps time.sleep(0) instant; the clock, not the sleep, is what
# the deadline is measured against.
check("the deadline fires once the elapsed time passes --timeout",
      run_watch(["--timeout", "5", "--interval", "0"], [0.0, 10.0]), 2)

# The point of measuring from `start` rather than per-iteration: elapsed time
# accumulates across polls. Three polls at 1s, 2s and 9s -- only the last trips
# a 5s deadline.
check("elapsed time accumulates across polls rather than resetting",
      run_watch(["--timeout", "5", "--interval", "0"], [0.0, 1.0, 2.0, 9.0]), 2)

# A wall clock can step backwards when NTP corrects it; monotonic cannot.
check("the default clock is monotonic, not wall time",
      inspect.signature(usbwatch.main).parameters["clock"].default,
      time.monotonic)

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all usbwatch tests passed")
