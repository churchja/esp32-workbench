#!/usr/bin/env python3
"""
Tests for parse_chip_banner() -- the esptool output parser.

This is the code that decides a board's identity, and identity is the backup
key. If it fails, the profile is empty and the backup gate cannot function.

Fixtures are generated from esptool's OWN format strings, transcribed from the
installed source (esptool/__init__.py lines ~496-503, esptool/cmds.py
read_mac/flash_id), rather than from memory. That is what makes them
conformance fixtures instead of self-consistency fixtures.

If a real recorded banner exists in tests/fixtures/, it is tested too:
    ESP32_RECORD_FIXTURE=tests/fixtures/c6.txt python3 tools/esp32ident.py

Run:  python3 tools/test_banner.py
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from esp32ident import parse_chip_banner  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


def v(d, key):
    return d.get(key, {}).get("value")


# --- fixture builders using esptool's literal f-string layouts --------------

def pad(label):
    """esptool: f"{label + ':':<20}" """
    return f"{label + ':':<20}"


def banner_5x(chip_desc, features, xtal, macs, usb_mode=None, port="/dev/cu.usbmodem101"):
    name = chip_desc.split(" (")[0]
    out = [f"esptool v5.2.0", f"Connected to {name} on {port}:"]
    out.append(f"{pad('Chip type')}{chip_desc}")
    out.append(f"{pad('Features')}{', '.join(features)}")
    out.append(f"{pad('Crystal frequency')}{xtal}MHz")
    if usb_mode:
        out.append(f"{pad('USB mode')}{usb_mode}")
    for label, mac in macs:
        out.append(f"{pad(label)}{mac}")
    return "\n".join(out) + "\n"


def flash_id_tail(mfr="c8", dev="4017", size="8MB",
                  ftype="quad (4 data lines)", volt="3.3V"):
    # Layout taken verbatim from a real esptool 5.2.0 run against an ESP32-S3
    # (tests/fixtures/board1.txt), not from documentation.
    return (f"Manufacturer: {mfr}\nDevice: {dev}\n"
            f"Detected flash size: {size}\n"
            f"Flash type set in eFuse: {ftype}\n"
            f"Flash voltage set by eFuse: {volt}\n")


print("esptool 5.x banner (the format actually installed)")

# ESP32-C6: has EUI64, so esptool prints THREE MAC lines. This is the case the
# old parser got wrong -- it clipped the 8-byte EUI64 into a fake 6-byte MAC.
c6 = banner_5x(
    "ESP32-C6 (QFN40) (revision v0.1)",
    ["WiFi 6", "BT 5", "IEEE802.15.4", "Single Core", "160MHz"],
    40,
    [("MAC", "60:55:f9:ff:fe:f7:2c:a2"),
     ("BASE MAC", "60:55:f9:f7:2c:a2"),
     ("MAC_EXT", "ff:fe")],
    usb_mode="USB-Serial/JTAG",
) + flash_id_tail()
d = parse_chip_banner(c6)

check("chip parsed from 'Chip type:'", v(d, "chip"), "ESP32-C6 (QFN40) (revision v0.1)")
check("chip_family stripped", v(d, "chip_family"), "ESP32-C6")
check("revision", v(d, "chip_revision"), "0.1")
check("features list", v(d, "chip_features"),
      ["WiFi 6", "BT 5", "IEEE802.15.4", "Single Core", "160MHz"])
check("crystal from 'Crystal frequency:'", v(d, "crystal"), "40MHz")
check("USB mode captured (new in 5.x)", v(d, "usb_mode"), "USB-Serial/JTAG")
check("*** BASE MAC wins over EUI64 ***", v(d, "mac"), "60:55:f9:f7:2c:a2")
check("EUI64 is NOT truncated into the mac field",
      v(d, "mac") == "60:55:f9:ff:fe:f7", False)
check("flash manufacturer", v(d, "flash_manufacturer_id"), "0xc8")
check("flash device", v(d, "flash_device_id"), "0x4017")
check("flash size", v(d, "flash_size"), "8MB")
check("flash mode (quad/octal)", v(d, "flash_mode"), "quad (4 data lines)")
check("flash voltage", v(d, "flash_voltage"), "3.3V")

# ESP32-S3: no EUI64, single MAC line.
s3 = banner_5x("ESP32-S3 (QFN56) (revision v0.2)",
               ["WiFi", "BLE", "Embedded PSRAM 8MB (AP_3v3)"], 40,
               [("MAC", "f4:12:fa:41:9c:20")],
               usb_mode="USB-Serial/JTAG") + flash_id_tail(size="16MB")
d = parse_chip_banner(s3)
check("S3 chip", v(d, "chip_family"), "ESP32-S3")
check("S3 single MAC line", v(d, "mac"), "f4:12:fa:41:9c:20")
check("S3 PSRAM feature survives", "Embedded PSRAM 8MB (AP_3v3)" in v(d, "chip_features"), True)
check("S3 flash size", v(d, "flash_size"), "16MB")

print("\nesptool 4.x banner (older installs must still work)")

old = ("esptool.py v4.7.0\n"
       "Chip is ESP32-D0WD-V3 (revision v3.1)\n"
       "Features: WiFi, BT, Dual Core, 240MHz, VRef calibration in efuse\n"
       "Crystal is 40MHz\n"
       "MAC: 24:6f:28:1a:2b:3c\n") + flash_id_tail(size="4MB")
d = parse_chip_banner(old)
check("4.x chip", v(d, "chip"), "ESP32-D0WD-V3 (revision v3.1)")
check("4.x chip_family", v(d, "chip_family"), "ESP32-D0WD-V3")
check("4.x crystal", v(d, "crystal"), "40MHz")
check("4.x MAC", v(d, "mac"), "24:6f:28:1a:2b:3c")
check("4.x has no USB mode line", v(d, "usb_mode"), None)

print("\ndegenerate input must not fabricate")
check("empty input yields nothing", parse_chip_banner(""), {})
check("unrelated text yields nothing", parse_chip_banner("hello\nworld\n"), {})
d = parse_chip_banner("A fatal error occurred: Failed to connect\n")
check("error text yields no chip", v(d, "chip"), None)
check("error text yields no mac", v(d, "mac"), None)

print("\nprovenance is set correctly")
d = parse_chip_banner(c6)
check("every field is 'probed'",
      {f["provenance"] for f in d.values()}, {"probed"})

print("\nrecorded real-hardware fixtures")
fx = sorted(glob.glob(os.path.join(REPO, "tests", "fixtures", "*.txt")))
if not fx:
    print("  SKIP  none recorded yet — with a board attached, run:")
    print("        ESP32_RECORD_FIXTURE=tests/fixtures/<chip>.txt \\")
    print("          python3 tools/esp32ident.py")
    print("        then re-run this file to convert these into conformance tests")
else:
    for f in fx:
        d = parse_chip_banner(open(f).read())
        name = os.path.basename(f)
        check(f"{name}: chip identified", bool(v(d, "chip")), True)
        check(f"{name}: MAC is exactly 6 octets",
              len((v(d, "mac") or "").split(":")), 6)
        check(f"{name}: flash size read", bool(v(d, "flash_size")), True)
        check(f"{name}: flash mode read", bool(v(d, "flash_mode")), True)


# ---------------------------------------------------------------------------
# idf.py set-target derivation
# ---------------------------------------------------------------------------
print("\nIDF target derivation from probed chip_family")

from esp32ident import idf_target_for  # noqa: E402

for fam, want in [
    ("ESP32-S3 (QFN56) (revision v0.2)", "esp32s3"),   # the board we validated
    ("ESP32-S3", "esp32s3"), ("ESP32-C3", "esp32c3"),
    ("ESP32-C6", "esp32c6"), ("ESP32-C5", "esp32c5"),
    ("ESP32-S2", "esp32s2"), ("ESP32-H2", "esp32h2"),
    ("ESP32-P4", "esp32p4"),
]:
    check(f"{fam!r} -> {want}", idf_target_for(fam)[0], want)

# Trap 1: classic ESP32 part numbers do NOT normalise to a valid target.
# "esp32d0wdv3" is rejected by set-target; only "esp32" is accepted.
for fam in ("ESP32-D0WD-V3", "ESP32-D0WDQ6", "ESP32-U4WDH", "ESP32-PICO-D4"):
    check(f"{fam!r} falls back to plain esp32", idf_target_for(fam)[0], "esp32")
check("note explains the fallback",
      "longest-prefix" in (idf_target_for("ESP32-D0WD-V3")[1] or ""), True)

# Trap 2: esp32c6 is a PREFIX of esp32c61. A shortest-match or dict-order
# lookup silently builds a C61 as a C6 -- different silicon, identical-looking
# build. Longest-prefix is what prevents that.
check("ESP32-C61 does NOT collapse to esp32c6",
      idf_target_for("ESP32-C61")[0], "esp32c61")
check("ESP32-C6 stays esp32c6", idf_target_for("ESP32-C6")[0], "esp32c6")

got, note = idf_target_for("ESP32-H21")
check("preview target still resolves", got, "esp32h21")
check("preview target is labelled PREVIEW", "PREVIEW" in (note or ""), True)
check("preview note mentions --preview", "--preview" in (note or ""), True)

# Unknown silicon must yield nothing rather than a plausible guess.
check("unknown chip yields no target", idf_target_for("ESP8266")[0], None)
check("empty chip yields no target", idf_target_for("")[0], None)
check("None chip yields no target", idf_target_for(None)[0], None)

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all banner + target tests passed")
