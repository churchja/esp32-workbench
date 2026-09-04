#!/usr/bin/env python3
"""
Tests for the backup gate decision.

This is the most safety-critical logic in the repo: it decides whether firmware
gets overwritten. It lives in tools/gate.py rather than inside the PlatformIO
extra script precisely so these can exist.

Run:  python3 tools/test_gate.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate import evaluate, render, find_repo_root, ALLOW, BLOCK  # noqa: E402

FAIL = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


BK = [{"file": "full-20260904T090000Z-8388608.bin",
       "created": "2026-09-04T09:00:00+00:00"},
      {"file": "full-20260901T090000Z-8388608.bin",
       "created": "2026-09-01T09:00:00+00:00"}]

print("gate decision")

# The case the whole gate exists for.
v, lines, char = evaluate("/repo", "30:ae:a4:11:22:33", "ESP32-C6",
                          "/dev/cu.usbmodem101", backups=[])
check("blocks with no backup", v, BLOCK)
check("names the board in the message",
      any("30:ae:a4:11:22:33" in l for l in lines), True)
check("names the chip", any("ESP32-C6" in l for l in lines), True)
check("tells you how to fix it", any("backup --port" in l for l in lines), True)
check("tells you how to bypass", any("ESP32_NO_GATE=1" in l for l in lines), True)

v, lines, _ = evaluate("/repo", "aa:bb:cc:dd:ee:ff", "ESP32-S3",
                       "/dev/cu.usbmodem1", backups=BK)
check("allows with a verified backup", v, ALLOW)
check("reports the NEWEST backup, not the first",
      any("20260904T090000Z" in l for l in lines), True)

v, lines, _ = evaluate("/repo", None, None, None, [], bypass=True)
check("bypass allows", v, ALLOW)
check("bypass still warns loudly", any("BYPASSED" in l for l in lines), True)

v, lines, _ = evaluate(None, "aa:bb:cc:dd:ee:ff", "ESP32", "/dev/x", [])
check("outside repo: allows", v, ALLOW)
check("outside repo: says INACTIVE, does not claim safety",
      any("INACTIVE" in l for l in lines), True)

v, lines, _ = evaluate("/repo", "aa:bb", "ESP32", "/dev/x", [],
                       esptool_found=False)
check("no esptool: allows but declares itself inactive", (v, any("INACTIVE" in l for l in lines)),
      (ALLOW, True))

# A single backup is enough; the gate is not asking for redundancy.
v, _l, _c = evaluate("/repo", "aa", "ESP32", "/dev/x", [BK[0]])
check("one backup suffices", v, ALLOW)

print("\nrepo root discovery")
with tempfile.TemporaryDirectory() as td:
    deep = os.path.join(td, "projects", "thing", "src")
    os.makedirs(deep)
    os.makedirs(os.path.join(td, "tools"))
    open(os.path.join(td, "tools", "esp32flash.py"), "w").close()
    check("finds root from a nested project", find_repo_root(deep), os.path.abspath(td))
    check("returns None when marker absent",
          find_repo_root(deep, marker="tools/nonexistent.py"), None)

with tempfile.TemporaryDirectory() as td:
    check("None outside any repo", find_repo_root(td), None)

print("\nrendering")
out = render(["a", "b"], "=")
check("banner is fenced", out.count("=" * 74), 2)
check("plain line has no fence", render(["ok"], None), "ok")

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all gate tests passed")
