#!/usr/bin/env python3
"""
Tests that README's Layout table matches the repository.

This file exists because that table was hand-maintained while everything around
it was generated. It silently drifted through a framework migration, three new
tools and a 150-assertion increase, until an audit found four defects at once:

  - templates/idf-base was missing entirely -- the PRIMARY framework's template
    absent while the demoted PlatformIO one was listed
  - a blank line sat inside the table, which splits a markdown table in two so
    every later row renders header-less
  - "98 assertions across 4 files" against an actual 249 across 7
  - tools/doctor.py and tools/usbwatch.py absent

Every one is mechanically checkable, so none should have needed an audit.

Run:  python3 tools/test_docs.py
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
README = os.path.join(REPO, "README.md")
FAIL = []
COUNT = {"n": 0}          # our own assertions, for the total below
DEFERRED = {}             # the total check, run last (see bottom)


def check(name, got, want):
    COUNT["n"] += 1
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


def layout_section():
    s = open(README).read()
    try:
        return s[s.index("## Layout"):s.index("## The two rules")]
    except ValueError:
        return ""


SECTION = layout_section()
# first column of each row: | `path` | description |
LISTED = set(re.findall(r"^\|\s*`([^`]+)`\s*\|", SECTION, re.M))


def covered(path):
    """A path is covered by an exact entry or by a listed glob (tools/test_*.py)."""
    if path in LISTED:
        return True
    return any(("*" in e) and glob.fnmatch.fnmatch(path, e) for e in LISTED)


print("Layout lists everything that exists")

check("section exists", bool(SECTION), True)

for p in sorted(glob.glob(os.path.join(REPO, "tools", "*.py"))):
    rel = os.path.relpath(p, REPO)
    check(f"{rel} is in Layout", covered(rel), True)

for d in sorted(glob.glob(os.path.join(REPO, "templates", "*"))):
    if not os.path.isdir(d):
        continue
    rel = os.path.relpath(d, REPO) + "/"
    check(f"{rel} is in Layout", rel in LISTED, True)

print("\nLayout lists nothing that does not exist")

for entry in sorted(LISTED):
    if "*" in entry:
        matches = glob.glob(os.path.join(REPO, entry))
        check(f"glob {entry} matches something", len(matches) > 0, True)
    else:
        check(f"{entry} exists", os.path.exists(os.path.join(REPO, entry)), True)

print("\nclaimed counts match reality")

m = re.search(r"\|\s*`tools/test_\*\.py`\s*\|\s*(\d+)\s+assertions across\s+(\d+)\s+files",
              SECTION)
check("the test row states a count", bool(m), True)
if m:
    claimed_a, claimed_f = int(m.group(1)), int(m.group(2))
    files = sorted(glob.glob(os.path.join(REPO, "tools", "test_*.py")))
    check("file count is current", claimed_f, len(files))

    # NEVER run ourselves. glob("tools/test_*.py") matches this file, and the
    # first version of this test spawned itself recursively until it was
    # killed. Our own contribution is counted in-process instead.
    SELF = os.path.abspath(__file__)
    others = [f for f in files if os.path.abspath(f) != SELF]
    check("we are excluded from the subprocess run", len(others), len(files) - 1)
    subtotal = 0
    for f in others:
        out = subprocess.run([sys.executable, f], capture_output=True, text=True,
                             cwd=REPO).stdout
        subtotal += out.count("  PASS")
    DEFERRED["claimed"] = claimed_a
    DEFERRED["subtotal"] = subtotal

print("\nthe table actually renders as one table")

lines = SECTION.splitlines()
splitters = [i for i in range(1, len(lines) - 1)
             if lines[i].strip() == ""
             and lines[i - 1].startswith("|") and lines[i + 1].startswith("|")]
check("no blank line splits the table", splitters, [])

print("\nthe template chooser references real templates")

chooser = SECTION[SECTION.index("### Which template"):] if "### Which template" in SECTION else ""
check("chooser exists", bool(chooser), True)
for t in re.findall(r"`(idf-base|idf-usb-console|pio-base)`", chooser):
    check(f"chooser names a real template: {t}",
          os.path.isdir(os.path.join(REPO, "templates", t)), True)

# Deferred to the end: the claimed total covers every test file INCLUDING this
# one, and this file's own count is only known once its checks have run. The +1
# accounts for the assertion on the next line.
if "claimed" in DEFERRED:
    check("assertion count is current",
          DEFERRED["claimed"], DEFERRED["subtotal"] + COUNT["n"] + 1)

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all docs tests passed")
