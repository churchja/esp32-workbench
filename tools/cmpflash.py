#!/usr/bin/env python3
"""
cmpflash.py -- compare two full-flash images region by region.

WHY NOT A WHOLE-IMAGE HASH
--------------------------
A sha256 over the whole image answers "same or not", which is the wrong
question after a restore. Firmware legitimately writes to `nvs` on first boot,
so a difference there means the board came up and did its job. A single byte
differing inside an app slot means the restore FAILED. Those two outcomes
deserve different answers, and a whole-image hash collapses them into one.

That distinction is not theoretical here: across five verified restores the
`nvs` delta has been 0, 50, 50, 49 and 0 bytes, while every code and filesystem
region was byte-perfect every time. A hash would have reported "mismatch" four
times and told you nothing about which kind.

THE CHECK THAT MAKES IT TRUSTWORTHY
-----------------------------------
Per-region counts are only meaningful if the regions cover the image with no
gaps. A gap silently swallows differences and under-reports -- the same defect
as a test sweep that counts FAIL lines and misses crashes. So the regions are
built to tile the image exactly, unallocated space included as explicit gap
entries, and the total is reconciled against an independent whole-image count.
A mismatch there is reported as an error, not a rounding difference.

Read-only. Takes no port and touches no hardware.

  python3 tools/cmpflash.py A.bin B.bin
  python3 tools/cmpflash.py --mac e4:b0:63:8a:ec:2c     # two newest backups

Exit codes:
  0  bit-identical
  1  differs ONLY in writable data regions (nvs/otadata/coredump) -- expected
     after the firmware has booted
  2  differs in a code or filesystem region -- the restore did not reproduce
     the image
  3  cannot compare (size mismatch, unreadable, or a reconciliation failure)
"""

import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BACKUPS = os.path.join(REPO, "backups")

PART_TABLE_OFFSET = 0x8000
PART_TABLE_SPAN = 0xC00
PART_MAGIC = b"\xaa\x50"

TYPES = {0: "app", 1: "data"}
SUBTYPES = {
    (0, 0x00): "factory", (0, 0x10): "ota_0", (0, 0x11): "ota_1",
    (1, 0x00): "otadata", (1, 0x01): "phy", (1, 0x02): "nvs",
    (1, 0x03): "coredump", (1, 0x04): "nvs_keys",
    (1, 0x80): "esphome", (1, 0x81): "fat", (1, 0x82): "spiffs",
    (1, 0x83): "littlefs",
}

# Regions the FIRMWARE is expected to write during normal operation. A
# difference here is evidence the board booted, not that the restore failed.
# Deliberately narrow: anything not listed is treated as must-match.
WRITABLE = {"nvs", "otadata", "coredump"}


def parse_partitions(blob):
    """Parse an ESP-IDF partition table. Pure: takes bytes, returns records."""
    out = []
    for i in range(0, len(blob), 32):
        entry = blob[i:i + 32]
        if len(entry) < 32 or entry[:2] != PART_MAGIC:
            break
        ptype, subtype, offset, size = struct.unpack("<BBII", entry[2:12])
        label = entry[12:28].rstrip(b"\x00").decode("ascii", "replace")
        out.append({
            "label": label,
            "type": TYPES.get(ptype, str(ptype)),
            "subtype": SUBTYPES.get((ptype, subtype), hex(subtype)),
            "offset": offset,
            "size": size,
        })
    return out


def build_regions(image_size, parts):
    """
    Tile [0, image_size) with named spans, gaps included explicitly.

    Pure, and the reason the totals can be reconciled: every byte of the image
    belongs to exactly one region, so per-region counts must sum to the
    whole-image count. Without the gap entries they would not.
    """
    spans = [("bootloader", 0, PART_TABLE_OFFSET),
             ("partition-table", PART_TABLE_OFFSET, PART_TABLE_SPAN)]
    for p in parts:
        spans.append((f"{p['label']} ({p['subtype']})", p["offset"], p["size"]))
    spans.sort(key=lambda s: s[1])

    tiled, cursor = [], 0
    for name, start, size in spans:
        if start >= image_size:
            # A partition declared entirely past the end of the image. Real
            # cause: the image was truncated, or read at the wrong flash size.
            # Naming it keeps the anomaly visible instead of silently dropping
            # a partition the operator can see in their own partition table.
            tiled.append((f"{name} !! BEYOND IMAGE END", image_size, 0))
            continue
        if start > cursor:
            tiled.append(("(unallocated)", cursor, start - cursor))
        elif start < cursor:
            tiled.append((f"!! OVERLAP before {name}", start, 0))
        # Clamp: regions must tile the IMAGE, not the partition table's idea of
        # it. Without this the size column reports bytes that were never read,
        # and the totals stop reconciling against the image length.
        end = min(start + size, image_size)
        clipped = " !! CLIPPED AT IMAGE END" if start + size > image_size else ""
        tiled.append((name + clipped, start, end - start))
        cursor = max(cursor, end)
    if cursor < image_size:
        tiled.append(("(unallocated)", cursor, image_size - cursor))
    return tiled


def count_diffs(a, b, start, size):
    """Differing bytes in one span. Pure."""
    return sum(1 for x, y in zip(a[start:start + size], b[start:start + size])
               if x != y)


def is_writable(region_name):
    """True if firmware is expected to write here during normal operation."""
    if "(" not in region_name:
        return False
    subtype = region_name.rsplit("(", 1)[1].rstrip(")")
    return subtype in WRITABLE


def classify(diffs):
    """
    Turn per-region diff counts into a verdict. Pure, and the whole point of
    the tool: `nvs` moving is a healthy board, an app slot moving is a failure.

    diffs: list of (region_name, differing_byte_count)
    """
    changed = [(n, d) for n, d in diffs if d > 0]
    if not changed:
        return 0, "bit-identical"
    hard = [(n, d) for n, d in changed if not is_writable(n)]
    if hard:
        return 2, ("differs in " + ", ".join(n for n, _ in hard)
                   + " -- these must match; the restore did not reproduce the image")
    return 1, ("differs only in " + ", ".join(n for n, _ in changed)
               + " -- expected once the firmware has booted")


def human(n):
    for unit in ("B", "KB", "MB"):
        if n < 1024 or unit == "MB":
            return f"{n}{unit}" if unit == "B" else f"{n:.4g}{unit}"
        n /= 1024


def newest_backups(mac):
    """The two newest backup images for a MAC, newest first."""
    d = os.path.join(BACKUPS, mac.replace(":", ""))
    man = os.path.join(d, "manifest.json")
    if not os.path.exists(man):
        return []
    entries = sorted(json.load(open(man)).get("backups", []),
                     key=lambda e: e.get("created", ""), reverse=True)
    return [os.path.join(d, e["file"]) for e in entries][:2]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="*", help="two image files to compare")
    ap.add_argument("--mac", help="compare the two newest backups for this MAC")
    args = ap.parse_args(argv)

    if args.mac:
        paths = newest_backups(args.mac)
        if len(paths) < 2:
            print(f"ERROR: need two backups for {args.mac}; found {len(paths)}",
                  file=sys.stderr)
            return 3
        b_path, a_path = paths[0], paths[1]      # older is A, newer is B
    elif len(args.images) == 2:
        a_path, b_path = args.images
    else:
        ap.error("give two image paths or --mac")

    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    print(f"A  {a_path}\n   {len(a):,} bytes  sha256 {hashlib.sha256(a).hexdigest()}")
    print(f"B  {b_path}\n   {len(b):,} bytes  sha256 {hashlib.sha256(b).hexdigest()}")

    if len(a) != len(b):
        print("\nERROR: size mismatch -- cannot compare region by region",
              file=sys.stderr)
        return 3

    parts = parse_partitions(a[PART_TABLE_OFFSET:PART_TABLE_OFFSET + PART_TABLE_SPAN])
    if not parts:
        print("\nERROR: no partition table found at 0x8000 in image A",
              file=sys.stderr)
        return 3
    regions = build_regions(len(a), parts)

    print(f"\n{'region':26}{'offset':>12}{'size':>10}{'differs':>12}")
    diffs = []
    for name, start, size in regions:
        d = count_diffs(a, b, start, size)
        diffs.append((name, d))
        mark = "" if d == 0 else "  <--"
        print(f"{name:26}{start:#012x}{human(size):>10}{d:>12,}{mark}")

    per_region = sum(d for _, d in diffs)
    whole = sum(1 for x, y in zip(a, b) if x != y)
    print(f"\nwhole-image differing bytes : {whole:,}")
    print(f"sum of per-region counts    : {per_region:,}")
    if per_region != whole:
        print(f"\nERROR: reconciliation failed -- {abs(whole - per_region):,} bytes "
              f"are unaccounted for. The regions do not tile the image, so the "
              f"breakdown above is under-reporting.", file=sys.stderr)
        return 3

    code, verdict = classify(diffs)
    print(f"\nVERDICT: {verdict}")
    return code


if __name__ == "__main__":
    sys.exit(main())
