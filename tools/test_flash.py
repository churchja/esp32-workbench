#!/usr/bin/env python3
"""
Tests for esp32flash.py's transfer logic.

This file exists because the --after bug shipped: esptool defaults to
--after hard-reset, so a backup ended by rebooting the board. On a native-USB
part that re-enumerates under a different identity on a different path, and the
`idf.py flash` that followed opened a port that no longer existed. Nothing in
this file's logic had a test.

Run:  python3 tools/test_flash.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from esp32flash import (read_with_fallback, looks_like_baud_failure,  # noqa: E402
                        report_board_state, BAUD_LADDER, parse_size)

FAIL = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


class FakeEsp:
    """Records every invocation; replays a scripted list of outcomes."""
    def __init__(self, outcomes):
        self.outcomes = list(outcomes)
        self.calls = []

    def run(self, port, subcmd, *args, **kw):
        self.calls.append({"port": port, "subcmd": subcmd, "args": args, **kw})
        rc, blob = self.outcomes.pop(0) if self.outcomes else (0, "")
        return rc, blob, ""


CORRUPT = "A fatal error occurred: Serial data stream stopped: Possible serial noise"
DEAD = "A fatal error occurred: Failed to connect to ESP32: No serial data received"


def tf(_b):
    return 60


print("--after is threaded through every attempt")

esp = FakeEsp([(0, "")])
read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf, baud=460800,
                   after="no-reset")
check("single attempt carries after", esp.calls[0].get("after"), "no-reset")
check("and carries the pinned baud", esp.calls[0].get("baud"), 460800)

# The regression that mattered: a retry must not silently drop it.
esp = FakeEsp([(1, CORRUPT), (0, "")])
rc, _blob, used = read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf,
                                     after="no-reset")
check("ladder stepped down after corruption", len(esp.calls), 2)
check("every attempt carries after",
      [c.get("after") for c in esp.calls], ["no-reset", "no-reset"])
check("second attempt used the next rung down",
      [c.get("baud") for c in esp.calls], [BAUD_LADDER[0], BAUD_LADDER[1]])
check("returns the baud that worked", used, BAUD_LADDER[1])

esp = FakeEsp([(0, "")])
read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf)
check("after=None is not passed as a string", esp.calls[0].get("after"), None)

print("\nladder only retries speed failures")

esp = FakeEsp([(1, DEAD)])
rc, _b, _u = read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf)
check("a chip that will not answer is not retried slower", len(esp.calls), 1)
check("and the failure is returned", rc, 1)

esp = FakeEsp([(1, CORRUPT), (1, CORRUPT), (1, CORRUPT)])
rc, _b, _u = read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf)
check("exhausts the whole ladder on repeated corruption",
      len(esp.calls), len(BAUD_LADDER))
check("gives up with a failure", rc, 1)

esp = FakeEsp([(1, CORRUPT)])
read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf, baud=230400)
check("an explicitly pinned baud is NOT laddered", len(esp.calls), 1)

print("\nfailure classification")
check("serial corruption is a speed failure", looks_like_baud_failure(CORRUPT), True)
check("no-serial-data is not", looks_like_baud_failure(DEAD), False)
check("empty output is not", looks_like_baud_failure(""), False)

print("\nthe board's post-op state is stated, not left to be discovered")

import io, contextlib  # noqa: E402


def say(after):
    buf = io.StringIO()
    with contextlib.redirect_stderr(buf):
        report_board_state(after, "/dev/cu.usbmodem01")
    return buf.getvalue()


hard, stay = say("hard-reset"), say("no-reset")
check("hard-reset warns the port may vanish", "may no longer exist" in hard, True)
check("hard-reset names the escape hatch", "--after no-reset" in hard, True)
# Written while debugging two native-USB boards, and wrong for the very next
# board type tested: a CH340 bridge keeps its port across a reset, because the
# bridge chip stays enumerated regardless of what the SoC does.
check("hard-reset does not assume native USB",
      "bridge" in hard.lower(), True)
check("hard-reset states the bridge case is stable",
      "stable" in hard.lower(), True)
check("no-reset says the board is parked", "DOWNLOAD MODE" in stay, True)
check("no-reset warns against leaving it parked", "drop USB" in stay, True)
check("both name the port", "/dev/cu.usbmodem01" in hard and
      "/dev/cu.usbmodem01" in stay, True)

print("\nsize parsing")
for raw, want in (("8MB", 8388608), ("4MB", 4194304), ("512KB", 524288),
                  ("1024", 1024), ("nonsense", None), ("", None)):
    check(f"parse_size({raw!r})", parse_size(raw), want)

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all flash tests passed")
