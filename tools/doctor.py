#!/usr/bin/env python3
"""
doctor.py -- is the workbench operational on THIS machine, right now?

Unit tests prove the logic is correct. They say nothing about whether esptool
is installed, which CLI dialect it speaks, whether the upload gate is actually
wired into the template, or whether backups/ is writable. That is the layer
that rots silently when a tool gets upgraded, and it is what "does it work?"
usually means.

Exit 0 = operational.  1 = degraded (warnings).  2 = broken (failures).
"""

import glob
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

OK, WARN, FAIL = "OK", "WARN", "FAIL"
RESULTS = []


def report(status, area, msg, fix=None):
    RESULTS.append((status, area, msg, fix))


def run(cmd, timeout=60):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except Exception as e:  # noqa: BLE001
        return 1, f"{type(e).__name__}: {e}"


# --------------------------------------------------------------------------

def check_python():
    v = sys.version_info
    if v < (3, 8):
        report(FAIL, "python", f"{v.major}.{v.minor} too old",
               "Install Python 3.8+")
    else:
        report(OK, "python", f"{v.major}.{v.minor}.{v.micro}")


def check_modules():
    for mod, why, hard in (("serial", "port enumeration", True),
                           ("yaml", "profile read/write", True)):
        if importlib.util.find_spec(mod):
            report(OK, f"py:{mod}", f"present ({why})")
        else:
            report(FAIL if hard else WARN, f"py:{mod}", f"MISSING -- {why} broken",
                   "pip install esptool  (pulls pyserial and PyYAML)")


def check_esptool():
    exe = shutil.which("esptool") or shutil.which("esptool.py")
    if not exe:
        report(FAIL, "esptool", "not on PATH -- nothing can talk to a board",
               "pip install esptool")
        return
    rc, out = run([exe, "version"])
    if rc != 0:
        rc, out = run([exe, "--help"])
    m = re.search(r"v?(\d+)\.(\d+)\.(\d+)", out)
    ver = ".".join(m.groups()) if m else "unknown"

    if "read-flash" in out:
        dialect = "hyphenated (5.x)"
    elif "read_flash" in out:
        dialect = "underscored (4.x)"
    else:
        rc2, h = run([exe, "--help"])
        dialect = ("hyphenated (5.x)" if "read-flash" in h
                   else "underscored (4.x)" if "read_flash" in h else "UNKNOWN")

    if dialect == "UNKNOWN":
        report(WARN, "esptool", f"{ver} -- could not detect subcommand dialect",
               "Tools auto-detect; if flashing fails, check `esptool --help`")
    else:
        report(OK, "esptool", f"{ver}, {dialect}")

    # Multiple esptool installs are a live hazard: ESP-IDF's export.sh PREPENDS
    # its own bundled esptool to PATH, so which binary wins can change between
    # shells. Version detection adapts, but a silent swap should be visible.
    #
    # Scan PATH directly rather than shelling out: /bin/sh on macOS rejects
    # `command -v -a` as an invalid option, and an earlier version of this check
    # swallowed that error and cheerfully reported "single install" while three
    # were present. A check that reports OK when the hazard exists is worse than
    # no check at all.
    found, seen_real = [], set()
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for nm in ("esptool", "esptool.py"):
            cand = os.path.join(d, nm)
            if os.path.isfile(cand) and os.access(cand, os.X_OK):
                real = os.path.realpath(cand)
                if real not in seen_real:
                    seen_real.add(real)
                    found.append(cand)
    if len(found) > 1:
        vers = []
        for o in found:
            _rc, o3 = run([o, "version"], timeout=30)
            mm = re.search(r"v?(\d+\.\d+(?:\.\d+)?)", o3)
            vers.append(f"{os.path.basename(o)} {mm.group(1) if mm else '?'} "
                        f"[{os.path.dirname(o)}]")
        report(WARN, "esptool:conflict",
               f"{len(found)} distinct installs on PATH -- " + "; ".join(vers),
               f"'{exe}' wins in this shell. ESP-IDF prepends its own bundled "
               f"copy; re-run doctor inside an IDF shell to see which wins there.")
    else:
        report(OK, "esptool:conflict", "single install on PATH")

    # Does OUR detector agree with what we just observed independently?
    try:
        from esp32ident import Esptool
        e = Esptool()
        ours = "hyphenated (5.x)" if e.hyphenated else "underscored (4.x)"
        if ours == dialect:
            report(OK, "esptool:detect", f"tool agrees: {ours}")
        else:
            report(FAIL, "esptool:detect",
                   f"MISMATCH -- doctor sees {dialect}, Esptool() says {ours}",
                   "Every flash command will use the wrong spelling")
    except Exception as ex:  # noqa: BLE001
        report(FAIL, "esptool:detect", f"Esptool() raised {type(ex).__name__}: {ex}")


def check_esp_idf():
    """
    ESP-IDF is the primary framework. This check must work OUTSIDE an IDF
    shell, because that is where doctor normally runs -- export.sh mutates a
    shell, and a shell does not survive between tool invocations.
    """
    want = None
    vf = os.path.join(REPO, ".idf-version")
    if os.path.isfile(vf):
        want = open(vf).read().strip()
        report(OK, "idf:pinned", f"{want} (from .idf-version)")
    else:
        report(WARN, "idf:pinned", "no .idf-version file -- version is remembered, "
                                   "not checked",
               "echo v6.0.3 > .idf-version")

    exported = os.environ.get("IDF_PATH")
    if exported and os.path.isdir(exported):
        rc, out = run(["/bin/sh", "-c",
                       f". {exported}/export.sh >/dev/null 2>&1; idf.py --version"],
                      timeout=120)
        m = re.search(r"(v\d+\.\d+(?:\.\d+)?)", out)
        have = m.group(1) if m else out.strip()[:30]
        if want and have != want:
            report(FAIL, "idf:active", f"shell has {have}, repo pins {want}",
                   "Wrong IDF exported; source the matching export.sh")
        else:
            report(OK, "idf:active", f"{have} exported in this shell")
        return

    # Not exported. Find an install matching the pin.
    candidates = sorted(glob.glob(os.path.expanduser("~/esp/esp-idf-*")))
    match = [c for c in candidates if want and want in os.path.basename(c)]
    if match:
        root = match[0]
        tools = os.path.expanduser(f"~/.espressif-{want.lstrip('v')}")
        have_tools = os.path.isdir(os.path.join(tools, "tools"))
        report(OK if have_tools else WARN, "idf:installed",
               f"{os.path.basename(root)}"
               + ("" if have_tools else "  (toolchains NOT installed)"),
               None if have_tools else f"cd {root} && IDF_TOOLS_PATH={tools} ./install.sh")
        if have_tools:
            chips = []
            td = os.path.join(tools, "tools")
            if os.path.isdir(td):
                names = os.listdir(td)
                if any("riscv32" in n for n in names):
                    chips.append("riscv32 (C3/C5/C6/H2)")
                if any("xtensa" in n for n in names):
                    chips.append("xtensa (ESP32/S2/S3)")
            report(OK, "idf:toolchains", ", ".join(chips) or "present")
        report(WARN, "idf:shell", "not exported in this shell",
               f"IDF_TOOLS_PATH={tools} . {root}/export.sh")
    elif candidates:
        report(FAIL, "idf:installed",
               f"found {[os.path.basename(c) for c in candidates]} but none match "
               f"the pin {want}",
               "Install the pinned version, or update .idf-version deliberately")
    else:
        report(FAIL, "idf:installed", "ESP-IDF not found -- it is the primary "
                                      "framework for this workbench",
               "See README 'Installing ESP-IDF'")


def check_idf_gate():
    """The IDF counterpart of check_gate(). Same claim, different mechanism."""
    ext = os.path.join(REPO, "templates", "idf-base", "idf_ext.py")
    if not os.path.isfile(ext):
        report(FAIL, "idf-gate:script", "idf_ext.py MISSING",
               "idf.py flash would write with no backup check")
        return
    report(OK, "idf-gate:script", "present")
    txt = open(ext).read()
    # Markers that MUST be present.
    required = [
        ("global_action_callbacks", "registers a global callback"),
        ("EFUSE_ACTIONS", "eFuse handled separately from backups"),
        ("dry_run", "--dry-run not blocked"),
        ("global_args['port'] = port", "resolved port pinned via global_args"),
    ]
    # Markers that MUST NOT reappear. Writing a global-scope option into a
    # task's action_args re-adds something idf.py deliberately removes, and
    # every real `idf.py flash` then dies with
    #   TypeError: flash() got an unexpected keyword argument 'port'
    # Verified on hardware. Unit tests cannot catch it: a fake Task's
    # action_args is a plain dict that accepts anything.
    forbidden = [
        ("action_args['port']", "writes port into task action_args "
                                "(breaks every flash)"),
    ]
    missing = [d for token, d in required if token not in txt]
    present = [d for token, d in forbidden if token in txt]
    if missing or present:
        parts = []
        if missing:
            parts.append("missing: " + "; ".join(missing))
        if present:
            parts.append("REGRESSION: " + "; ".join(present))
        report(FAIL, "idf-gate:integrity", " | ".join(parts),
               "Re-check templates/idf-base/idf_ext.py against the "
               "hardware-verified design")
    else:
        report(OK, "idf-gate:integrity",
               "4 corrections present, 1 known regression absent")


def check_platformio():
    exe = shutil.which("pio") or shutil.which("platformio")
    if not exe:
        report(OK, "platformio", "not installed (optional -- Arduino path only)")
        return
    rc, out = run([exe, "--version"])
    m = re.search(r"version\s+(\S+)", out)
    report(OK, "platformio", f"{m.group(1) if m else out.strip()[:40]}")

    rc, out = run([exe, "platform", "list"], timeout=120)
    if "espressif32" in out:
        m = re.search(r"espressif32\s*@\s*(\S+)", out)
        v = m.group(1) if m else "?"
        report(OK, "pio:espressif32", f"{v} installed")
        # The C5/C6/H2/P4 Arduino gap -- verified, not assumed
        bd = os.path.expanduser("~/.platformio/platforms/espressif32/boards/"
                                "esp32-c6-devkitc-1.json")
        if os.path.isfile(bd):
            try:
                fw = json.load(open(bd)).get("frameworks", [])
                if "arduino" not in fw:
                    report(WARN, "pio:c5/c6/h2/p4",
                           f"official platform declares {fw} for C6 -- no Arduino",
                           "Use the pinned pioarduino env in the template for "
                           "C5/C6/H2/P4 (see references/flashing.md)")
                else:
                    report(OK, "pio:c5/c6", "official platform supports arduino")
            except Exception:  # noqa: BLE001
                pass
    else:
        report(WARN, "pio:espressif32", "platform not installed",
               "First build will download it (~1GB)")


def check_fs_tools():
    found = []
    root = os.path.expanduser("~/.platformio/packages")
    for name in ("mklittlefs", "mkspiffs"):
        p = shutil.which(name)
        if not p and os.path.isdir(root):
            for d in os.listdir(root):
                c = os.path.join(root, d, name)
                if os.path.isfile(c) and os.access(c, os.X_OK):
                    p = c
                    break
        if p:
            found.append(name)
    if importlib.util.find_spec("littlefs"):
        found.append("littlefs-python")
    if found:
        report(OK, "fs-extract", ", ".join(found))
    else:
        report(WARN, "fs-extract", "no unpacker -- filesystem partitions dump "
                                   "as raw .bin only",
               "pip install littlefs-python, or build any LittleFS project once")


def check_repo():
    for d in ("tools", "boards", "templates", "docs"):
        path = os.path.join(REPO, d)
        report(OK if os.path.isdir(path) else FAIL, f"repo:{d}",
               "present" if os.path.isdir(path) else "MISSING")
    bk = os.path.join(REPO, "backups")
    try:
        os.makedirs(bk, exist_ok=True)
        probe = os.path.join(bk, ".doctor_write_probe")
        with open(probe, "w") as fh:
            fh.write("x")
        os.remove(probe)
        report(OK, "repo:backups", "writable -- backups can be taken")
    except Exception as e:  # noqa: BLE001
        report(FAIL, "repo:backups", f"NOT writable: {e}",
               "Every write to a board will be blocked by the gate")


def check_gate():
    """The claim most likely to silently become false."""
    ini = os.path.join(REPO, "templates", "pio-base", "platformio.ini")
    script = os.path.join(REPO, "templates", "pio-base", "scripts",
                          "backup_gate.py")
    if not os.path.isfile(script):
        report(FAIL, "gate:script", "backup_gate.py MISSING",
               "pio upload writes to boards with no backup")
        return
    report(OK, "gate:script", "present")
    if not os.path.isfile(ini):
        report(FAIL, "gate:wiring", "platformio.ini missing")
        return
    text = open(ini).read()
    if "extra_scripts" in text and "backup_gate.py" in text:
        report(OK, "gate:wiring", "wired into [env] via extra_scripts")
    else:
        report(FAIL, "gate:wiring", "NOT wired -- pio upload is unguarded",
               "Add: extra_scripts = pre:scripts/backup_gate.py")
    try:
        from gate import evaluate, BLOCK
        v, _l, _c = evaluate("/repo", "aa:bb", "ESP32", "/dev/x", [])
        report(OK if v == BLOCK else FAIL, "gate:logic",
               "blocks an unbacked board" if v == BLOCK
               else "DOES NOT BLOCK an unbacked board")
    except Exception as e:  # noqa: BLE001
        report(FAIL, "gate:logic", f"{type(e).__name__}: {e}")


def check_skills():
    proj = os.path.join(REPO, ".claude", "skills", "esp32-workbench", "SKILL.md")
    user = os.path.expanduser("~/.claude/skills/esp32/SKILL.md")
    for label, p, note in (("skill:project", proj, "loads inside this repo"),
                           ("skill:router", user, "fires from any directory")):
        if not os.path.isfile(p):
            report(WARN, label, f"missing -- {note} will not happen")
            continue
        head = open(p).read()[:8192]
        m = re.match(r"^---\n(.*?)\n---\n", head, re.S)
        if m and re.search(r"^name:", m.group(1), re.M) \
              and re.search(r"^description:", m.group(1), re.M):
            report(OK, label, note)
        else:
            report(FAIL, label, "frontmatter invalid -- skill will not load")


def check_profiles():
    files = sorted(glob.glob(os.path.join(REPO, "boards", "*.yaml")))
    real = [f for f in files if not os.path.basename(f).startswith("_")]
    if not real:
        report(WARN, "profiles", f"{len(files)} file(s), none from real hardware",
               "Run: python3 tools/esp32ident.py --save  with a board attached")
    else:
        report(OK, "profiles", f"{len(real)} board profile(s)")
    try:
        from validate_profiles import validate
        errs = sum(len(validate(f)[0]) for f in files)
        report(OK if not errs else FAIL, "profiles:schema",
               "all valid" if not errs else f"{errs} violation(s)",
               None if not errs else "python3 tools/validate_profiles.py")
    except Exception as e:  # noqa: BLE001
        report(FAIL, "profiles:schema", f"{type(e).__name__}: {e}")


def check_backups():
    bk = os.path.join(REPO, "backups")
    if not os.path.isdir(bk):
        return
    boards = [d for d in os.listdir(bk)
              if not d.startswith(".") and os.path.isdir(os.path.join(bk, d))]
    if not boards:
        report(WARN, "backups", "none stored -- any upload will be blocked",
               "That is the gate working, not a bug. Take one when a board is on.")
        return
    try:
        from esp32flash import verified_backups
        total = ok = 0
        for b in boards:
            mac = ":".join(b[i:i + 2] for i in range(0, len(b), 2))
            man = os.path.join(bk, b, "manifest.json")
            if os.path.exists(man):
                total += len(json.load(open(man)).get("backups", []))
                ok += len(verified_backups(mac))
        report(OK if ok == total else FAIL, "backups",
               f"{ok}/{total} verified across {len(boards)} board(s)",
               None if ok == total else "Some images are missing or corrupt")
    except Exception as e:  # noqa: BLE001
        report(WARN, "backups", f"could not verify: {e}")


def check_ports():
    try:
        from esp32ident import enumerate_ports
        ports = enumerate_ports()
        if ports:
            for p in ports:
                vid = f"{p['vid']:04x}" if p["vid"] is not None else "????"
                pid = f"{p['pid']:04x}" if p["pid"] is not None else "????"
                report(OK, "board", f"{p['device']}  {vid}:{pid}  "
                                    f"{p.get('description') or ''}")
        else:
            report(WARN, "board", "none attached -- contact paths untested",
                   "Plug a board in and re-run to exercise the serial layer")
    except Exception as e:  # noqa: BLE001
        report(FAIL, "board", f"enumerate_ports raised {type(e).__name__}: {e}")


def main():
    for fn in (check_python, check_modules, check_esptool,
               check_esp_idf, check_idf_gate,
               check_platformio, check_fs_tools, check_repo, check_gate,
               check_skills, check_profiles, check_backups, check_ports):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            report(FAIL, fn.__name__, f"check itself crashed: "
                                      f"{type(e).__name__}: {e}")

    width = max(len(a) for _s, a, _m, _f in RESULTS) + 2
    fails = warns = 0
    for status, area, msg, fix in RESULTS:
        print(f"  {status:<5} {area:<{width}} {msg}")
        if fix:
            print(f"        {'':<{width}} -> {fix}")
        fails += status == FAIL
        warns += status == WARN

    print()
    if fails:
        print(f"BROKEN: {fails} failure(s), {warns} warning(s)")
        return 2
    if warns:
        print(f"DEGRADED: {warns} warning(s), no failures. "
              f"Core paths are operational.")
        return 1
    print("OPERATIONAL: all checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
