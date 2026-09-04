#!/usr/bin/env python3
"""
usbwatch.py -- watch USB serial devices appear and disappear, and say what
each identity MEANS.

Written because an ESP32-S2 presents at least three different USB identities
depending on what is running, and "the board vanished" turned out to mean four
different things over one session:

    239a:8111  tinyuf2 / CircuitPython   -- names the BOARD
    303a:0002  ESP32-S2 native USB CDC   -- ROM bootloader OR an app using CDC
    (absent)   no USB device at all      -- often FINE: firmware that does not
                                            drive USB-OTG simply never enumerates

That last row is the important one. On a native-USB part, a board running
perfectly good firmware that does not bring up a USB device stack is
indistinguishable from an unplugged one -- and looks identical to bricked.

Usage:
    python3 tools/usbwatch.py                 # follow changes until Ctrl-C
    python3 tools/usbwatch.py --once          # snapshot and exit
    python3 tools/usbwatch.py --wait          # exit 0 when a device appears
    python3 tools/usbwatch.py --wait --timeout 60
    python3 tools/usbwatch.py --all           # include non-USB serial ports
"""

import argparse
import os
import sys
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from serial.tools import list_ports
except ImportError:
    print("pyserial not installed (pip install esptool)", file=sys.stderr)
    sys.exit(3)

# What a given VID:PID tells you -- and, just as importantly, what it does not.
MEANING = {
    (0x303A, 0x0002): ("ESP32-S2 native USB CDC",
                       "ROM bootloader OR an app using native CDC -- "
                       "these are INDISTINGUISHABLE from the descriptor"),
    (0x303A, 0x1001): ("Espressif USB JTAG/serial debug unit",
                       "S3/C3/C6/C5/H2 -- ROM or an app; esptool can usually "
                       "auto-enter download mode"),
    (0x1A86, 0x7523): ("CH340/CH341 UART bridge", "bridge chip; the SoC has no USB"),
    (0x1A86, 0x55D4): ("CH9102F/CH343 UART bridge", "bridge chip"),
    (0x10C4, 0xEA60): ("CP2102/CP2102N UART bridge", "bridge chip"),
    (0x0403, 0x6001): ("FTDI FT232R UART bridge", "bridge chip"),
}
VENDOR = {0x303A: "Espressif", 0x239A: "Adafruit", 0x2886: "Seeed",
          0x1B4F: "SparkFun", 0x1A86: "QinHeng", 0x10C4: "Silicon Labs",
          0x0403: "FTDI"}


def describe(p):
    vid, pid = p.vid, p.pid
    if vid is None:
        return "(no USB identity -- Bluetooth or virtual port)", ""
    known = MEANING.get((vid, pid))
    if known:
        return known
    vendor = VENDOR.get(vid)
    if vendor == "Adafruit":
        return (f"{p.product or 'Adafruit board'}",
                "Adafruit VID -- usually tinyuf2/CircuitPython. This mode "
                "names the BOARD, which ROM mode does not")
    return (p.product or p.description or "unknown device",
            f"{vendor + ' VID' if vendor else 'unknown VID'} -- not in the table")


def snapshot(include_all):
    out = {}
    for p in list_ports.comports():
        if not include_all and (p.vid is None or p.pid is None):
            continue
        out[p.device] = p
    return out


def stamp():
    return datetime.now().strftime("%H:%M:%S")


def show(p, prefix):
    vid = f"{p.vid:04x}" if p.vid is not None else "????"
    pid = f"{p.pid:04x}" if p.pid is not None else "????"
    name, meaning = describe(p)
    print(f"  {stamp()} {prefix} {p.device}  {vid}:{pid}  {name}")
    if meaning:
        print(f"           {'':<1} -> {meaning}")
    if p.serial_number:
        n = "".join(c for c in p.serial_number.lower() if c in "0123456789abcdef")
        extra = "  (MAC-shaped -- vendors often publish the eFuse MAC here)" \
            if len(n) == 12 else ""
        print(f"           {'':<1}    serial: {p.serial_number}{extra}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--once", action="store_true", help="print current state, exit")
    ap.add_argument("--wait", action="store_true",
                    help="exit 0 as soon as a device appears")
    ap.add_argument("--timeout", type=float, default=0,
                    help="give up after N seconds (0 = never)")
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--all", action="store_true",
                    help="include ports with no USB identity")
    args = ap.parse_args()

    cur = snapshot(args.all)
    if args.wait and args.all:
        print("  note: --wait ignores non-USB ports; --all only widens display")
    if cur:
        print(f"  {stamp()} present:")
        for p in cur.values():
            show(p, "  ")
    else:
        print(f"  {stamp()} no USB serial devices")
        print("           A board running firmware that never brings up a USB")
        print("           device stack looks exactly like this -- and like an")
        print("           unplugged one. It is not evidence of damage.")
        print("           To reach the ROM: hold BOOT, tap RESET, release BOOT.")

    # --wait triggers on a USB device specifically, never on the non-USB ports
    # that always exist (Bluetooth, debug-console, paired audio). --all is a
    # DISPLAY option; letting it widen --wait made `--wait --all` a silent
    # no-op that exits 0 the instant it starts.
    def usb_only(d):
        return {k: v for k, v in d.items() if v.vid is not None}

    if args.once:
        return 0 if usb_only(cur) else 1
    if args.wait and usb_only(cur):
        return 0

    print(f"  {stamp()} watching (Ctrl-C to stop)...")
    start = time.time()
    try:
        while True:
            time.sleep(args.interval)
            new = snapshot(args.all)
            for dev, p in new.items():
                if dev not in cur:
                    show(p, "+ ")
                    if args.wait and p.vid is not None:
                        return 0
            for dev, p in cur.items():
                if dev not in new:
                    print(f"  {stamp()} - {dev}  gone")
            cur = new
            if args.timeout and (time.time() - start) > args.timeout:
                print(f"  {stamp()} timeout after {args.timeout:.0f}s")
                return 2
    except KeyboardInterrupt:
        print(f"\n  {stamp()} stopped")
        return 0


if __name__ == "__main__":
    sys.exit(main())
