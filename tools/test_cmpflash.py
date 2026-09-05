#!/usr/bin/env python3
"""
Tests for cmpflash.py.

This file exists because the tool's whole value is a DISTINCTION -- `nvs`
moving means the board booted, an app slot moving means the restore failed --
and a comparison that reported "differs" for both would be no better than the
sha256 it replaces. So classify() is tested in both directions.

The second thing tested is the reconciliation property: regions must tile the
image exactly. A gap swallows differences silently, which is the same defect
shape as a test sweep that counts FAIL lines and never notices a crash. That
was caught once already in this repo, so it gets an assertion here rather than
a comment.

Run:  python3 tools/test_cmpflash.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cmpflash import (build_regions, classify, count_diffs,  # noqa: E402
                      is_writable, parse_partitions, PART_TABLE_OFFSET)

FAIL = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


def entry(label, ptype, subtype, offset, size):
    return (b"\xaa\x50" + struct.pack("<BBII", ptype, subtype, offset, size)
            + label.encode().ljust(16, b"\x00") + b"\x00" * 4)


print("partition table parsing")

TABLE = (entry("nvs", 1, 2, 0x9000, 0x5000)
         + entry("otadata", 1, 0, 0xe000, 0x2000)
         + entry("app0", 0, 0x10, 0x10000, 0x640000)
         + b"\xff" * 64)
parts = parse_partitions(TABLE)
check("parses every entry", len(parts), 3)
check("labels survive", [p["label"] for p in parts], ["nvs", "otadata", "app0"])
check("subtypes are named", [p["subtype"] for p in parts],
      ["nvs", "otadata", "ota_0"])
check("offsets and sizes are ints",
      (parts[2]["offset"], parts[2]["size"]), (0x10000, 0x640000))
check("stops at the first non-magic entry", len(parse_partitions(b"\xff" * 96)), 0)
check("an empty table is not an error", parse_partitions(b""), [])


print("\nregions tile the image with no gaps")

# The reconciliation property, asserted directly: every byte belongs to exactly
# one region. If this fails, per-region counts under-report and the tool lies.
SIZE = 0x800000        # big enough to actually hold the table above
regions = build_regions(SIZE, parse_partitions(TABLE))
covered = sum(sz for _, _, sz in regions)
check("regions cover the whole image exactly", covered, SIZE)

edges = sorted((start, start + sz) for _, start, sz in regions if sz)
contiguous = all(edges[i][1] == edges[i + 1][0] for i in range(len(edges) - 1))
check("regions are contiguous, no overlap or hole", contiguous, True)
check("the first region starts at zero", edges[0][0], 0)
check("the last region ends at the image end", edges[-1][1], SIZE)

names = [n for n, _, _ in regions]
check("bootloader is named", "bootloader" in names, True)
check("partition table is named", "partition-table" in names, True)
check("the space after the last partition is an explicit gap",
      "(unallocated)" in names, True)

# A table whose partitions do not start right after the header must still tile.
SPARSE = entry("app0", 0, 0x10, 0x20000, 0x10000) + b"\xff" * 32
r2 = build_regions(0x40000, parse_partitions(SPARSE))
check("a hole before the first partition is filled",
      sum(sz for _, _, sz in r2), 0x40000)

# A partition declaring more space than the image holds. Real cause: a
# truncated image, or a read taken at the wrong flash size. Tiling past the end
# would report sizes for bytes that were never read, so it is clamped and the
# anomaly is named rather than hidden.
OVER = entry("app0", 0, 0x10, 0x10000, 0x640000) + b"\xff" * 32
r3 = build_regions(0x100000, parse_partitions(OVER))
check("an oversized partition is clamped to the image",
      sum(sz for _, _, sz in r3), 0x100000)
check("and the clipping is named, not silent",
      any("CLIPPED" in n for n, _, _ in r3), True)

BEYOND = entry("nvs", 1, 2, 0x9000, 0x1000) + entry("app0", 0, 0x10, 0x900000, 0x1000) + b"\xff" * 32
r4 = build_regions(0x100000, parse_partitions(BEYOND))
check("a partition entirely past the end still tiles",
      sum(sz for _, _, sz in r4), 0x100000)
check("and is named as beyond the image",
      any("BEYOND IMAGE END" in n for n, _, _ in r4), True)


print("\nclassify separates a booted board from a failed restore")

check("no differences is bit-identical", classify([("app0 (ota_0)", 0)])[0], 0)
check("and says so", classify([("app0 (ota_0)", 0)])[1], "bit-identical")

# The distinction the tool exists for.
check("nvs alone is exit 1, not a failure",
      classify([("nvs (nvs)", 50), ("app0 (ota_0)", 0)])[0], 1)
check("an app slot is exit 2",
      classify([("nvs (nvs)", 0), ("app0 (ota_0)", 1)])[0], 2)
check("ONE byte in an app slot is still a failure",
      classify([("app0 (ota_0)", 1)])[0], 2)
check("a filesystem partition is exit 2, not writable",
      classify([("spiffs (spiffs)", 4)])[0], 2)
check("bootloader is exit 2",
      classify([("bootloader", 2)])[0], 2)

# A hard failure must not be masked by a soft one appearing alongside it.
check("nvs plus an app slot reports the app slot",
      classify([("nvs (nvs)", 50), ("app0 (ota_0)", 3)])[0], 2)
check("and names the hard region, not the soft one",
      "app0" in classify([("nvs (nvs)", 50), ("app0 (ota_0)", 3)])[1]
      and "nvs" not in classify([("nvs (nvs)", 50), ("app0 (ota_0)", 3)])[1].split("--")[0],
      True)

check("otadata is writable", is_writable("otadata (otadata)"), True)
check("coredump is writable", is_writable("coredump (coredump)"), True)
check("an unnamed region is not writable", is_writable("bootloader"), False)
check("an ota slot is not writable", is_writable("app1 (ota_1)"), False)


print("\ndiff counting")

A = bytes(range(256)) * 4
B = bytearray(A)
B[10] = (B[10] + 1) % 256
B[300] = (B[300] + 1) % 256
check("counts only bytes that differ", count_diffs(A, bytes(B), 0, len(A)), 2)
check("respects the span given", count_diffs(A, bytes(B), 0, 256), 1)
check("a span with no differences is zero", count_diffs(A, bytes(B), 512, 256), 0)
check("identical input is zero", count_diffs(A, A, 0, len(A)), 0)

# The property the tool reconciles against.
whole = count_diffs(A, bytes(B), 0, len(A))
per_region = sum(count_diffs(A, bytes(B), s, 256) for s in range(0, len(A), 256))
check("per-span counts sum to the whole", per_region, whole)


print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all cmpflash tests passed")
