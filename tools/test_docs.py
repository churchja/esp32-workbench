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

The build log then drifted the same way, and worse -- it sat at "29 commits,
seven boards, 353 assertions" while the repo held nine boards and 360. Nothing
checked it, because this file only ever guarded README.md. It guards both now.

A later audit found a fifth of the same kind: "It runs four layers" with a
four-row table against a smoke.sh that has five. The missing row was the real
ESP-IDF layer -- the only one exercising the PRIMARY framework and the gate
path most flashing goes through, so the table undersold the suite by omitting
its strongest layer. That claim is checked here now too.

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


README_TEXT = open(README).read()
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

print("\nthe smoke table matches smoke.sh")

# smoke.sh names its layers with hdr "N. title"; the trailing hdr "result" has
# no number, which is what distinguishes a layer from the summary line.
SMOKE = open(os.path.join(REPO, "smoke.sh")).read()
LAYERS = re.findall(r'^hdr "(\d+)\.\s*(.+?)"', SMOKE, re.M)
check("smoke.sh declares numbered layers", len(LAYERS) > 0, True)

# Scope to the "Is it working?" section so the board and restore tables
# elsewhere in the README cannot satisfy the row regex by accident.
try:
    HOWTO = README_TEXT[README_TEXT.index("## Is it working?"):]
    HOWTO = HOWTO[:HOWTO.index("\n## ", 1)] if "\n## " in HOWTO[1:] else HOWTO
except ValueError:
    HOWTO = ""
check("the smoke section exists", bool(HOWTO), True)

WORDS = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
         "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10}
m_layers = re.search(r"It runs (\w+) layers", HOWTO)
check("the README states a layer count", bool(m_layers), True)
if m_layers:
    check("the stated layer count matches smoke.sh",
          WORDS.get(m_layers.group(1).lower()), len(LAYERS))

rows = re.findall(r"^\|\s*(\d+)\s*\|", HOWTO, re.M)
check("the table has one row per layer", len(rows), len(LAYERS))
check("the rows are numbered 1..N in order",
      rows, [str(i) for i in range(1, len(LAYERS) + 1)])

# A layer can be present and still be wrong about itself. The ESP-IDF row was
# the one that went missing, and it is the one whose absence matters most.
check("the ESP-IDF layer is named in the table",
      bool(re.search(r"ESP-IDF", HOWTO)), True)


print("\nthe build log's counts match the repo")

# docs/session-2026-09-04.md is a LIVING document -- it kept being extended --
# so its counts drift exactly like README's did. Note what is NOT checked: the
# commit count, which used to be in its header. A document cannot state its own
# post-commit total, because the commit that updates the number changes it. A
# check on it would fail on every single commit, which is a failure signal for a
# non-problem. It was removed from the doc instead.
LOG = os.path.join(REPO, "docs", "session-2026-09-04.md")
check("the build log exists", os.path.exists(LOG), True)
LOG_TEXT = open(LOG).read() if os.path.exists(LOG) else ""

profiles = [f for f in glob.glob(os.path.join(REPO, "boards", "*.yaml"))
            if not os.path.basename(f).startswith("_")]
n_boards = len(profiles)
check("there is at least one board profile", n_boards > 0, True)

NUMBER = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six",
          7: "seven", 8: "eight", 9: "nine", 10: "ten", 11: "eleven",
          12: "twelve"}
word = NUMBER.get(n_boards)
check("the board count has a word for it", bool(word), True)

if word:
    check("the board-count heading is current",
          bool(re.search(rf"^## {word.capitalize()} boards\s*$", LOG_TEXT, re.M | re.I)),
          True)
    check("the 'all N identified' line is current",
          bool(re.search(rf"All {word} identified", LOG_TEXT, re.I)), True)
    # The opening paragraph states the count too, and drifted to "Nine boards"
    # under a "## Ten boards" heading because only the heading was checked.
    # Every place the number appears in prose needs to be one of these.
    check("the header's board count is current",
          bool(re.search(rf"\b{word} boards\b", LOG_TEXT.split("## ")[0], re.I)),
          True)

# The table under that heading must have one row per profile. Scoped to the
# section so the failure-taxonomy tables further down cannot satisfy it.
m_sec = re.search(r"^## \w+ boards\s*$(.*?)^## ", LOG_TEXT, re.M | re.S)
check("the board section is delimited", bool(m_sec), True)
if m_sec:
    rows = [l for l in m_sec.group(1).splitlines()
            if l.startswith("| ") and not l.startswith("|---")
            and not l.startswith("| Board ")]
    check("the board table has one row per profile", len(rows), n_boards)

m_log_a = re.search(r"to \*\*(\d+) across (\d+)\*\*", LOG_TEXT)
check("the build log states an assertion count", bool(m_log_a), True)
if m_log_a and "claimed" in DEFERRED:
    check("the build log's assertion count matches README's",
          int(m_log_a.group(1)), DEFERRED["claimed"])
    check("the build log's file count matches the repo",
          int(m_log_a.group(2)),
          len(glob.glob(os.path.join(REPO, "tools", "test_*.py"))))

# The origin commit is a fixed historical fact, so it IS checkable and stays.
origin = re.search(r"from `([0-9a-f]{7,40})`", LOG_TEXT)
check("the build log names an origin commit", bool(origin), True)
if origin:
    rc = subprocess.run(["git", "cat-file", "-e", origin.group(1) + "^{commit}"],
                        cwd=REPO, capture_output=True).returncode
    check("that origin commit exists in history", rc, 0)


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
