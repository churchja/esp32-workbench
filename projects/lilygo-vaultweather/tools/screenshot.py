#!/usr/bin/env python3
"""
Capture the LilyGo panel over USB and save it as a PNG.

WHY THIS EXISTS
There is no other way to see what is actually on this display, and reading the
layout arithmetic is not a substitute -- three separate reviews read it and
none caught that lv_font_unscii_16 is 16px per character rather than 8, which
was silently wrong by 2x throughout. It took one capture and a ruler.

Captures found, in order: a wrapped label overwriting the row beneath it, an
entire panel running 100px off the right edge, a clock showing UTC for a minute
after every boot, and a temperature whose digits ran together.

HOW THE TRIGGER WORKS
The firmware polls the USB-Serial-JTAG peripheral from its render loop and acts
on single keystrokes. It reads that peripheral DIRECTLY rather than stdin,
because the primary console on this board is UART0 on GPIO43/44 -- pins that
are not on the USB cable. The host only ever sees the secondary console, and
that direction is output-only.

    s   dump the screen      n   next panel
    r   force a data refresh ?   help

The dump is base64 RGB565, framed by SNAP_BEGIN / SNAP_END.

Run:
    python3 tools/screenshot.py -o shot.png
    python3 tools/screenshot.py -o current.png --panel current
"""
import argparse
import base64
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial required: python3 -m pip install pyserial")


def capture(port, baud=115200, timeout_s=40):
    """Send 's' and return (width, height, panel_index, raw RGB565 bytes)."""
    with serial.Serial(port, baud, timeout=2) as s:
        s.reset_input_buffer()
        s.write(b"s")
        s.flush()

        t0 = time.time()
        header, lines, capturing = None, [], False
        while time.time() - t0 < timeout_s:
            line = s.readline().decode("utf8", "replace").rstrip()
            if not line:
                continue
            if line.startswith("SNAP_BEGIN"):
                header, lines, capturing = line, [], True
                continue
            if line.startswith("SNAP_END"):
                break
            if capturing:
                lines.append(line)
        else:
            raise SystemExit("timed out waiting for SNAP_END; is the firmware "
                             "current, and does it print 'serial console ready'?")

    if not header:
        raise SystemExit("no SNAP_BEGIN seen. The board only answers 's' if the\n"
                         "console driver installed -- check the boot log for\n"
                         "'serial console ready'.")

    dims = dict(kv.split("=") for kv in header.split()[1:])
    w, h = int(dims["w"]), int(dims["h"])
    # The firmware reports which panel it caught. Older firmware does not, and
    # -1 makes that explicit rather than silently pretending it is panel 0.
    panel = int(dims.get("panel", -1))
    raw = base64.b64decode("".join(lines))
    want = w * h * 2
    if len(raw) != want:
        raise SystemExit("short read: got %d bytes, expected %d" % (len(raw), want))
    return w, h, panel, raw


def to_image(w, h, raw):
    from PIL import Image
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        base = y * w * 2
        for x in range(w):
            # Little-endian RGB565. The panel is driven as RGB565_SWAPPED, but
            # lv_snapshot_take() is asked for plain RGB565, so the swap does not
            # apply to what comes out here.
            v = (raw[base + x * 2 + 1] << 8) | raw[base + x * 2]
            px[x, y] = (((v >> 11) & 0x1F) * 255 // 31,
                        ((v >> 5) & 0x3F) * 255 // 63,
                        (v & 0x1F) * 255 // 31)
    return img


PANELS = {0: "current", 1: "current+", 2: "forecast"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/cu.usbmodem1101")
    ap.add_argument("-o", "--out", default="screenshot.png")
    ap.add_argument("--panel", choices=["any", "current", "current+", "forecast"],
                    default="any",
                    help="press 'n' until the named panel is showing")
    ap.add_argument("--raw", action="store_true",
                    help="also write the undecoded RGB565 alongside the PNG")
    a = ap.parse_args()

    # 6 tries, not 3: the panels also rotate on their own every 8 seconds, so a
    # press can land where the timer was about to move anyway.
    for _ in range(6):
        w, h, panel, raw = capture(a.port)
        if a.panel == "any" or PANELS.get(panel) == a.panel:
            break
        if panel < 0:
            print("warning: firmware does not report a panel index; "
                  "saving whatever is up", file=sys.stderr)
            break
        with serial.Serial(a.port, 115200, timeout=1) as s:
            s.write(b"n")
            s.flush()
        time.sleep(0.6)
    else:
        print("warning: never landed on %r (last was %r); saving anyway"
              % (a.panel, PANELS.get(panel, panel)), file=sys.stderr)

    img = to_image(w, h, raw)
    img.save(a.out)
    if a.raw:
        open(a.out + ".rgb565", "wb").write(raw)
    print("wrote %s (%dx%d, panel %s)"
          % (a.out, w, h, PANELS.get(panel, panel)))


if __name__ == "__main__":
    main()
