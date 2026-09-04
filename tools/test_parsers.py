#!/usr/bin/env python3
"""
Tests for the pure byte-parsers in esp32ident.py.

These need no hardware. They take a buffer and return a dict, so their
correctness is fully verifiable with synthetic fixtures -- and they are the
highest-risk code in the repo, because hand-rolled struct parsing against fixed
offsets fails silently. A wrong offset yields plausible garbage, not an error.

Run:  python3 tools/test_parsers.py
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from esp32ident import (parse_partition_table, parse_app_desc,  # noqa: E402
                        human_bytes)
from esp32dump import parse_nvs, detect_fs, extract_strings  # noqa: E402

FAILURES = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAILURES.append(name)


def section(t):
    print(f"\n{t}")


# ---------------------------------------------------------------------------
# Fixture builders -- byte-exact against the documented on-flash formats
# ---------------------------------------------------------------------------

def part_entry(ptype, subtype, offset, size, label, flags=0):
    """32 bytes: magic(2) type(1) subtype(1) offset(4) size(4) label(16) flags(4)"""
    rec = struct.pack("<HBBII", 0x50AA, ptype, subtype, offset, size)
    rec += label.encode().ljust(16, b"\x00")
    rec += struct.pack("<I", flags)
    assert len(rec) == 32, len(rec)
    return rec


def md5_entry(digest=b"\xab" * 16):
    return struct.pack("<H", 0xEBEB) + b"\xff" * 14 + digest


def app_desc_blob(project, version, idf, date, time_):
    """esp_app_desc_t lives at offset 0x20 of an app image."""
    d = struct.pack("<I", 0xABCD5432)          # 0   magic
    d += struct.pack("<I", 7)                   # 4   secure_version
    d += b"\x00" * 8                            # 8   reserv1[2]
    d += version.encode().ljust(32, b"\x00")    # 16  version
    d += project.encode().ljust(32, b"\x00")    # 48  project_name
    d += time_.encode().ljust(16, b"\x00")      # 80  time
    d += date.encode().ljust(16, b"\x00")       # 96  date
    d += idf.encode().ljust(32, b"\x00")        # 112 idf_ver
    d += bytes(range(32))                       # 144 app_elf_sha256
    d += b"\x00" * 80                           # 176 reserv2[20]
    assert len(d) == 256, len(d)
    return b"\xe9" + b"\x00" * 0x1F + d + b"\xff" * 512   # header, then desc


def nvs_entry(ns, dtype, span, key, data8):
    rec = bytes([ns, dtype, span, 0xFF]) + b"\x00" * 4
    rec += key.encode().ljust(16, b"\x00")
    rec += data8.ljust(8, b"\x00")
    assert len(rec) == 32, len(rec)
    return rec


def nvs_page(entries, written_count):
    page = struct.pack("<I", 0xFFFFFFFE)        # state: active
    page += b"\xff" * 28                        # rest of the 32-byte header
    bitmap = bytearray(b"\xff" * 32)            # 0b11 == empty
    for i in range(written_count):
        byte, shift = i // 4, (i % 4) * 2
        bitmap[byte] &= ~(0b11 << shift) & 0xFF
        bitmap[byte] |= 0b10 << shift           # 0b10 == written
    page += bytes(bitmap)
    body = b"".join(entries)
    page += body + b"\xff" * (4096 - len(page) - len(body))
    assert len(page) == 4096, len(page)
    return page


# ---------------------------------------------------------------------------

section("partition table")

tbl = (part_entry(1, 0x02, 0x9000, 0x5000, "nvs")
       + part_entry(0, 0x00, 0x10000, 0x180000, "factory")
       + part_entry(1, 0x82, 0x190000, 0x70000, "spiffs")
       + md5_entry()
       + b"\xff" * 256)
entries, md5 = parse_partition_table(tbl)

check("entry count", len(entries), 3)
check("nvs label", entries[0]["label"], "nvs")
check("nvs type/subtype", (entries[0]["type"], entries[0]["subtype"]), ("data", "nvs"))
check("factory is app/factory", (entries[1]["type"], entries[1]["subtype"]), ("app", "factory"))
check("factory offset hex", entries[1]["offset"], "0x10000")
check("factory offset int", entries[1]["offset_int"], 0x10000)
check("factory size human", entries[1]["size_human"], "1.5MB")
check("spiffs subtype", entries[2]["subtype"], "spiffs")
check("md5 captured", md5, "ab" * 16)
check("stops at 0xFF padding", len(entries), 3)

# an entry with the encrypted flag set
enc = part_entry(0, 0x00, 0x10000, 0x1000, "app", flags=1) + b"\xff" * 32
check("encrypted flag", parse_partition_table(enc)[0][0]["encrypted"], True)

check("empty input", parse_partition_table(b"")[0], [])
check("garbage input", parse_partition_table(b"\x00" * 128)[0], [])
check("all-erased flash", parse_partition_table(b"\xff" * 512)[0], [])

section("app descriptor")

blob = app_desc_blob("paphos-porthole", "1.0.2", "v5.1.2", "Sep  2 2026", "11:30:00")
d = parse_app_desc(blob)
check("project_name", d["project_name"], "paphos-porthole")
check("app_version", d["app_version"], "1.0.2")
check("idf_version", d["idf_version"], "v5.1.2")
check("build_date", d["build_date"], "Sep  2 2026")
check("build_time", d["build_time"], "11:30:00")
check("secure_version", d["secure_version"], 7)
check("sha256 length", len(d["app_elf_sha256"]), 64)
check("sha256 value", d["app_elf_sha256"][:8], "00010203")

check("rejects wrong magic", parse_app_desc(b"\x00" * 512), None)
check("rejects short buffer", parse_app_desc(b"\xe9" * 16), None)

section("NVS")

page = nvs_page([
    nvs_entry(0, 0x01, 1, "wifi_cfg", struct.pack("<B", 1)),      # namespace decl
    nvs_entry(1, 0x04, 1, "boot_count", struct.pack("<I", 42)),   # u32
    nvs_entry(1, 0x21, 2, "ssid", struct.pack("<H", 11)),         # str header
    b"HomeNetwork".ljust(32, b"\x00"),                            # str payload
], written_count=4)

nvs = parse_nvs(page)
check("namespace resolved", nvs["namespaces"], {1: "wifi_cfg"})
by_key = {e["key"]: e for e in nvs["entries"]}
check("u32 value", by_key["boot_count"]["value"], 42)
check("u32 type", by_key["boot_count"]["type"], "u32")
check("u32 namespace name", by_key["boot_count"]["namespace"], "wifi_cfg")
check("str value", by_key["ssid"]["value"], "HomeNetwork")
check("span skips payload", "HomeNetwork" in by_key, False)

check("empty page ignored", parse_nvs(b"\xff" * 4096)["entries"], [])
check("truncated input", parse_nvs(b"\x00" * 100)["entries"], [])

section("filesystem detection")

check("littlefs magic", detect_fs(b"\x00" * 8 + b"littlefs" + b"\x00" * 500), "littlefs")
check("fat boot signature", detect_fs(b"\x00" * 510 + b"\x55\xaa" + b"\x00" * 10), "fat")
check("unknown blob", detect_fs(b"\x01\x02\x03"), "unknown")

section("strings")

check("finds long run", "HelloWorld" in extract_strings(b"\x00\x01HelloWorld\x00\xff"), True)
check("drops short run", extract_strings(b"\x00abc\x00", minlen=6), [])

section("human_bytes")

check("bytes", human_bytes(512), "512B")
check("exact KB", human_bytes(4096), "4KB")
check("fractional MB", human_bytes(1572864), "1.5MB")
check("8MB flash", human_bytes(8 * 1024 * 1024), "8MB")
check("zero", human_bytes(0), "0B")

section("conformance vs real toolchain artifacts")

# Fixtures above are built from our own reading of the format -- they prove
# internal consistency, not conformance. These parse binaries emitted by
# Espressif's own tooling, which can genuinely fail.
#
# Generate them:  cp -r templates/pio-base /tmp/b && cd /tmp/b && pio run -e esp32dev

CANDIDATES = [
    ".scratch_build/.pio/build/esp32dev",
    "projects/_validate/.pio/build/esp32dev",
    "/tmp/b/.pio/build/esp32dev",
]
build = next((d for d in CANDIDATES if os.path.isdir(d)), None)
if not build:
    print("  SKIP  no build artifacts found (see comment above to generate)")
else:
    pt = os.path.join(build, "partitions.bin")
    fw = os.path.join(build, "firmware.bin")
    bl = os.path.join(build, "bootloader.bin")
    if os.path.exists(pt):
        ents, md5 = parse_partition_table(open(pt, "rb").read())
        check("real table parses", len(ents) > 0, True)
        check("real table has an app partition",
              any(e["type"] == "app" for e in ents), True)
        check("real table md5 present", md5 is not None, True)
        check("offsets ascend",
              [e["offset_int"] for e in ents] == sorted(e["offset_int"] for e in ents),
              True)
    if os.path.exists(fw):
        d = parse_app_desc(open(fw, "rb").read())
        check("real firmware has a descriptor", d is not None, True)
        if d:
            check("idf_version looks like a version",
                  d["idf_version"].startswith("v"), True)
            check("sha256 is 64 hex chars", len(d["app_elf_sha256"]), 64)
            check("project_name printable", d["project_name"].isprintable(), True)
    if os.path.exists(bl):
        check("bootloader rejected", parse_app_desc(open(bl, "rb").read()), None)

print()
if FAILURES:
    print(f"{len(FAILURES)} FAILURE(S): {', '.join(FAILURES)}")
    sys.exit(1)
print("all parser tests passed")
