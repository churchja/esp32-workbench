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
import contextlib
import hashlib
import inspect
import io
import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esp32flash  # noqa: E402
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
rc, _blob, used, _at = read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o",
                                          tf, after="no-reset")
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
rc, _b, _u, _a = read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf)
check("a chip that will not answer is not retried slower", len(esp.calls), 1)
check("and the failure is returned", rc, 1)

esp = FakeEsp([(1, CORRUPT), (1, CORRUPT), (1, CORRUPT)])
rc, _b, _u, _a = read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf)
check("exhausts the whole ladder on repeated corruption",
      len(esp.calls), len(BAUD_LADDER))
check("gives up with a failure", rc, 1)

esp = FakeEsp([(1, CORRUPT)])
read_with_fallback(esp, "/dev/x", 0, 1024, "/tmp/o", tf, baud=230400)
check("an explicitly pinned baud is NOT laddered", len(esp.calls), 1)

print("\nevery rung is timed separately, not the ladder as a whole")

# The bug this replaced: cmd_backup wrapped the WHOLE ladder in one timer and
# stored that as read_seconds, so a discarded fast attempt was charged to the
# slower read that actually produced the image. Every USB-Serial/JTAG board in
# this repo falls back, so every one of them recorded an inflated figure.
# Ticks are the real LilyGo numbers: 460800 died at 5.3s, 230400 read 16MB.
class FakeClock:
    """read_with_fallback calls the clock twice per rung: start, then end."""
    def __init__(self, ticks):
        self.ticks = list(ticks)

    def __call__(self):
        return self.ticks.pop(0)


esp = FakeEsp([(1, CORRUPT), (0, "")])
rc, _b, used, attempts = read_with_fallback(
    esp, "/dev/x", 0, 1024, "/tmp/o", tf,
    clock=FakeClock([0.0, 5.3, 5.3, 756.5]))
check("one record per rung attempted", len(attempts), 2)
check("each record names its rung",
      [a["baud"] for a in attempts], [BAUD_LADDER[0], BAUD_LADDER[1]])
check("the discarded rung is timed on its own", attempts[0]["seconds"], 5.3)
check("the successful rung excludes the discarded one",
      attempts[1]["seconds"], 751.2)
check("only the rung that worked is marked ok",
      [a["ok"] for a in attempts], [False, True])

# The invariant that makes both numbers safe to publish side by side.
check("ladder time is exactly the sum of its rungs",
      round(sum(a["seconds"] for a in attempts), 1), 756.5)
check("and it exceeds the transfer time by the discarded attempt",
      round(sum(a["seconds"] for a in attempts) - attempts[1]["seconds"], 1),
      5.3)

esp = FakeEsp([(0, "")])
_rc, _b, _u, attempts = read_with_fallback(
    esp, "/dev/x", 0, 1024, "/tmp/o", tf, clock=FakeClock([10.0, 22.5]))
check("a first-try read records a single attempt", len(attempts), 1)
check("with no discarded time to subtract", attempts[0]["seconds"], 12.5)

# Rounding happens at record time, not at report time. Storing the raw float
# instead would leave ladder_seconds == sum(rungs) true only to within float
# error, and the README publishes both numbers side by side.
esp = FakeEsp([(0, "")])
_rc, _b, _u, attempts = read_with_fallback(
    esp, "/dev/x", 0, 1024, "/tmp/o", tf, clock=FakeClock([0.0, 1.0 / 3]))
check("each rung is rounded as it is recorded, not when reported",
      attempts[0]["seconds"], 0.3)

esp = FakeEsp([(1, CORRUPT), (1, CORRUPT), (1, CORRUPT)])
_rc, _b, _u, attempts = read_with_fallback(
    esp, "/dev/x", 0, 1024, "/tmp/o", tf,
    clock=FakeClock([0, 1, 1, 3, 3, 6]))
check("an exhausted ladder still reports every rung it burned",
      [(a["baud"], a["seconds"]) for a in attempts],
      [(BAUD_LADDER[0], 1.0), (BAUD_LADDER[1], 2.0), (BAUD_LADDER[2], 3.0)])
check("and marks none of them ok", any(a["ok"] for a in attempts), False)

# A dead chip is not retried, but the attempt it did make is still on record.
esp = FakeEsp([(1, DEAD)])
_rc, _b, _u, attempts = read_with_fallback(
    esp, "/dev/x", 0, 1024, "/tmp/o", tf, clock=FakeClock([0, 4.0]))
check("a non-speed failure is still timed and recorded",
      [(a["baud"], a["seconds"], a["ok"]) for a in attempts],
      [(BAUD_LADDER[0], 4.0, False)])

# Durations must not be able to run backwards when NTP steps the system clock.
check("the default clock is monotonic, not wall time",
      inspect.signature(read_with_fallback).parameters["clock"].default,
      time.monotonic)


print("\nthe manifest records the split, not just the function")

# read_with_fallback returning the right numbers is worth nothing if cmd_backup
# writes the wrong ones, and cmd_backup is where the original bug lived. Every
# assertion above passes against a cmd_backup that reinstates the whole-ladder
# value -- verified by mutation -- because nothing exercised the manifest.
class Args:
    port, size, baud, after, timeout = "/dev/x", None, None, "no-reset", 60


class WritingEsp(FakeEsp):
    """FakeEsp that also produces the image file esptool would have written."""
    version = "5.2.0-fake"

    def run(self, port, subcmd, *args, **kw):
        rc, so, se = super().run(port, subcmd, *args, **kw)
        if rc == 0 and subcmd == "read-flash":
            with open(args[2], "wb") as fh:
                fh.write(b"\0" * int(args[1]))
        return rc, so, se


SILICON = ({"mac": {"value": "aa:bb:cc:dd:ee:ff"},
            "flash_size": {"value": "1024"},
            "chip": {"value": "ESP32-S3 (fake)"}}, {})


def run_backup(outcomes, ticks):
    """Drive cmd_backup end to end and return the manifest entry it wrote."""
    tmp = tempfile.mkdtemp()
    real_backups, real_probe = esp32flash.BACKUPS, esp32flash.probe_silicon
    esp32flash.BACKUPS = tmp
    esp32flash.probe_silicon = lambda esp, port: SILICON
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            esp32flash.cmd_backup(WritingEsp(outcomes), Args(),
                                  clock=FakeClock(ticks))
        with open(os.path.join(tmp, "aabbccddeeff", "manifest.json")) as fh:
            return json.load(fh)["backups"][-1]
    finally:
        esp32flash.BACKUPS, esp32flash.probe_silicon = real_backups, real_probe
        shutil.rmtree(tmp, ignore_errors=True)


entry = run_backup([(1, CORRUPT), (0, "")], [0.0, 5.3, 5.3, 756.5])
check("read_seconds is the successful rung ALONE", entry["read_seconds"], 751.2)
check("ladder_seconds is the wall time including the discard",
      entry["ladder_seconds"], 756.5)
check("the two differ by exactly the discarded attempt",
      round(entry["ladder_seconds"] - entry["read_seconds"], 1), 5.3)
check("every rung reached the manifest",
      [(a["baud"], a["seconds"], a["ok"]) for a in entry.get("attempts", [])],
      [(BAUD_LADDER[0], 5.3, False), (BAUD_LADDER[1], 751.2, True)])
check("and the recorded baud is the one that worked",
      entry["baud"], BAUD_LADDER[1])
check("ladder_seconds equals the sum of the recorded rungs",
      entry["ladder_seconds"],
      round(sum(a["seconds"] for a in entry["attempts"]), 1))

entry = run_backup([(0, "")], [100.0, 142.5])
check("a first-try backup records the two figures as equal",
      (entry["read_seconds"], entry["ladder_seconds"]), (42.5, 42.5))
check("and still carries an attempts list",
      [(a["baud"], a["ok"]) for a in entry.get("attempts", [])],
      [(BAUD_LADDER[0], True)])

# Absence of "attempts" is what marks the eight pre-change entries as
# whole-ladder. A new entry must never be written without it.
check("no entry is written without the marker key",
      all(k in entry for k in ("read_seconds", "ladder_seconds", "attempts")),
      True)


print("\nevery write path forwards --after and --baud to esptool")

# These three functions had NO tests, which is exactly why the --after bug
# survived here after being fixed in backup. An option that is parsed, accepted
# and then dropped is worse than one that does not exist: the operator is told
# nothing and gets the opposite behaviour. Measured consequence -- a restore run
# with --after no-reset hard-reset a Satellite1 anyway, its firmware booted and
# wrote 65 bytes to nvs, and the delta read as firmware behaviour until the
# source was read.


class RecordingEsp(FakeEsp):
    version = "5.2.0-fake"

    def run(self, port, subcmd, *args, **kw):
        rc, so, se = super().run(port, subcmd, *args, **kw)
        if subcmd == "write-flash" and len(args) >= 2 and os.path.isdir(
                os.path.dirname(args[1]) or "."):
            pass
        return rc, so, se


def make_board(tmp, mac="aa:bb:cc:dd:ee:ff", payload=b"\x00" * 4096):
    """A board dir with one backup whose sha256 matches, so the gate passes."""
    d = os.path.join(tmp, mac.replace(":", ""))
    os.makedirs(d, exist_ok=True)
    name = "full-test-4096.bin"
    with open(os.path.join(d, name), "wb") as fh:
        fh.write(payload)
    digest = hashlib.sha256(payload).hexdigest()
    with open(os.path.join(d, "manifest.json"), "w") as fh:
        json.dump({"mac": mac, "backups": [{
            "file": name, "sha256": digest, "bytes": len(payload),
            "created": "2026-09-05T00:00:00+00:00"}]}, fh)
    return d


def drive(cmd, **overrides):
    """Run one esp32flash command against a fake chip; return the esptool calls."""
    tmp = tempfile.mkdtemp()
    make_board(tmp)
    real_backups, real_probe = esp32flash.BACKUPS, esp32flash.probe_silicon
    esp32flash.BACKUPS = tmp
    esp32flash.probe_silicon = lambda esp, port: SILICON
    esp = RecordingEsp([(0, "")] * 4)

    class A:
        port, timeout, baud, after = "/dev/x", 60, None, "hard-reset"
        yes, no_backup_i_accept_the_risk = True, False
        file, image, address, size = None, None, "0x10000", None
    a = A()
    for k, v in overrides.items():
        setattr(a, k, v)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            {"restore": esp32flash.cmd_restore,
             "flash": esp32flash.cmd_flash,
             "erase": esp32flash.cmd_erase}[cmd](esp, a)
    finally:
        esp32flash.BACKUPS, esp32flash.probe_silicon = real_backups, real_probe
        shutil.rmtree(tmp, ignore_errors=True)
    return [c for c in esp.calls if c["subcmd"] in ("write-flash", "erase-flash")]


IMG = os.path.join(tempfile.mkdtemp(), "payload.bin")
open(IMG, "wb").write(b"\xa5" * 2048)

for cmd, extra in (("restore", {}), ("flash", {"image": IMG}), ("erase", {})):
    calls = drive(cmd, after="no-reset", **extra)
    check(f"{cmd} reaches esptool once", len(calls), 1)
    check(f"{cmd} forwards --after no-reset", calls[0].get("after"), "no-reset")

    calls = drive(cmd, after="hard-reset", **extra)
    check(f"{cmd} forwards --after hard-reset", calls[0].get("after"), "hard-reset")

    calls = drive(cmd, baud=921600, after="no-reset", **extra)
    check(f"{cmd} forwards --baud", calls[0].get("baud"), 921600)

# The specific regression: esptool's OWN default is hard-reset, so a dropped
# after= is not a missing flag, it is the opposite behaviour applied silently.
calls = drive("restore", after="no-reset")
check("a dropped after= would read as None, not as no-reset",
      calls[0].get("after") is not None, True)


print("\nevery write path states what the board is doing afterwards")

# report_board_state existed but only backup called it. After a restore, flash
# or erase the port may have moved or the board may be parked, and the operator
# is the one who has to know.
def stdout_of(cmd, **kw):
    tmp = tempfile.mkdtemp()
    make_board(tmp)
    real_backups, real_probe = esp32flash.BACKUPS, esp32flash.probe_silicon
    esp32flash.BACKUPS = tmp
    esp32flash.probe_silicon = lambda esp, port: SILICON

    class A:
        port, timeout, baud, after = "/dev/x", 60, None, "hard-reset"
        yes, no_backup_i_accept_the_risk = True, False
        file, image, address, size = None, None, "0x10000", None
    a = A()
    for k, v in kw.items():
        setattr(a, k, v)
    err = io.StringIO()
    try:
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(err):
            {"restore": esp32flash.cmd_restore,
             "flash": esp32flash.cmd_flash,
             "erase": esp32flash.cmd_erase}[cmd](
                 RecordingEsp([(0, "")] * 4), a)
    finally:
        esp32flash.BACKUPS, esp32flash.probe_silicon = real_backups, real_probe
        shutil.rmtree(tmp, ignore_errors=True)
    return err.getvalue()

for cmd, extra in (("restore", {}), ("flash", {"image": IMG}), ("erase", {})):
    out = stdout_of(cmd, after="no-reset", **extra)
    check(f"{cmd} says the board is parked", "DOWNLOAD MODE" in out, True)
    out = stdout_of(cmd, after="hard-reset", **extra)
    check(f"{cmd} warns the port may vanish after a reset",
          "may no longer exist" in out, True)


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
