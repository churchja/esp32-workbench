#!/usr/bin/env python3
"""
Tests for the ESP-IDF pre-flash gate (templates/idf-base/idf_ext.py).

Importable without ESP-IDF, SCons, or a board, because the extension keeps
module scope free of fallible imports. Each test below corresponds to a defect
the adversarial review found in the original proposal.

Run:  python3 tools/test_idf_gate.py
"""
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "templates", "idf-base"))

import idf_ext  # noqa: E402

FAIL = []


def check(name, got, want):
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


class Task:
    def __init__(self, name, action_args=None):
        self.name = name
        self.action_args = dict(action_args or {})


class Args(dict):
    """Stand-in for idf.py's PropertyDict."""
    def get(self, k, d=None):
        return dict.get(self, k, d)


def run_gate(tasks, global_args=None, env=None):
    """Invoke the callback, capturing abort + stderr, restoring env after."""
    ga = Args(global_args or {})
    saved = {k: os.environ.get(k) for k in
             ("ESP32_NO_GATE", "ESP32_EFUSE_I_UNDERSTAND", "ESP32_WORKBENCH_ROOT")}
    for k, v in (env or {}).items():
        os.environ[k] = v
    for k in saved:
        if k not in (env or {}) and saved[k] is not None:
            os.environ.pop(k, None)

    out, aborted = [], []
    real_say, real_abort = idf_ext._say, idf_ext._abort
    idf_ext._say = lambda t: out.append(t)

    def fake_abort(lines):
        aborted.append(lines)
    idf_ext._abort = fake_abort
    try:
        idf_ext.backup_gate(None, ga, tasks)
    finally:
        idf_ext._say, idf_ext._abort = real_say, real_abort
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    return {"out": "\n".join(out), "aborted": aborted, "tasks": tasks, "ga": ga}


print("write-action detection")
check("plain flash is a write", idf_ext._is_write_action("flash"), True)
check("app-flash is a write", idf_ext._is_write_action("app-flash"), True)
check("erase-flash is a write", idf_ext._is_write_action("erase-flash"), True)
check("unknown *-flash target caught by suffix",
      idf_ext._is_write_action("some-custom-flash"), True)
check("build is not a write", idf_ext._is_write_action("build"), False)
check("monitor is not a write", idf_ext._is_write_action("monitor"), False)
check("efuse-burn NOT in the backup-gated set",
      idf_ext._is_write_action("efuse-burn"), False)
check("efuse actions are their own category",
      sorted(idf_ext.EFUSE_ACTIONS),
      ["efuse-burn", "efuse-burn-key", "efuse-read-protect", "efuse-write-protect"])

print("\nno-op cases (must never block)")
r = run_gate([Task("build")])
check("build alone: no abort", r["aborted"], [])
check("build alone: silent", r["out"], "")

r = run_gate([Task("monitor"), Task("size")])
check("monitor+size: no abort", r["aborted"], [])

# DEFECT: callbacks run before idf.py honours --dry-run, so without the guard
# the gate blocked a command that writes nothing.
r = run_gate([Task("flash")], {"dry_run": True})
check("--dry-run flash: NOT blocked", r["aborted"], [])
check("--dry-run flash: stays silent", r["out"], "")

print("\nbypass")
r = run_gate([Task("flash")], env={"ESP32_NO_GATE": "1"})
check("bypass: no abort", r["aborted"], [])
check("bypass: warns loudly", "BYPASSED" in r["out"], True)

print("\neFuse — a flash backup cannot undo a burned fuse")
r = run_gate([Task("efuse-burn")])
check("efuse without opt-in: ABORTS", len(r["aborted"]), 1)
body = "\n".join(r["aborted"][0]) if r["aborted"] else ""
check("efuse: says permanent", "PERMANENT" in body, True)
check("efuse: names its own opt-in", "ESP32_EFUSE_I_UNDERSTAND=1" in body, True)
check("efuse: states NO_GATE does not unlock it",
      "ESP32_NO_GATE=1 deliberately does NOT unlock" in body, True)
check("efuse: never mentions taking a backup as the fix",
      "backup --port" in body, False)

r = run_gate([Task("efuse-burn")], env={"ESP32_EFUSE_I_UNDERSTAND": "1"})
check("efuse with opt-in: not aborted for the burn itself",
      any("PERMANENT" in " ".join(a) for a in r["aborted"]), False)
check("efuse with opt-in: still warns", "IRREVERSIBLE" in r["out"], True)

# The ordering defect: efuse-burn alone has no flash action, so an eFuse check
# placed after the `if not writes: return` exit would never run.
r = run_gate([Task("efuse-burn")], env={"ESP32_NO_GATE": "1"})
check("NO_GATE does not smuggle an eFuse burn through", len(r["aborted"]), 1)

print("\nport pinning — global_args only, never action_args")


class StrictTask:
    """
    action_args that REFUSES unknown keys, the way a real click callback does.

    A plain-dict fake accepts any assignment, which is how an earlier version
    of the gate shipped a mutation that made every real `idf.py flash` die with
    TypeError. The double must be at least as strict as production.
    """
    def __init__(self, name):
        self.name = name
        self.action_args = self._Strict()

    class _Strict(dict):
        def __setitem__(self, k, v):
            raise TypeError(
                f"flash() got an unexpected keyword argument {k!r} -- "
                f"global-scope options must not be written into action_args")


st = StrictTask("flash")
ga = Args({"port": "/dev/cu.usbmodem101"})
_saved_say, _saved_abort = idf_ext._say, idf_ext._abort
idf_ext._say = lambda t: None
idf_ext._abort = lambda lines: None
try:
    os.environ["ESP32_NO_GATE"] = "1"      # stop before any probing
    idf_ext.backup_gate(None, ga, [st])
    _ok = True
except TypeError as e:
    _ok = False
    print(f"          raised: {e}")
finally:
    os.environ.pop("ESP32_NO_GATE", None)
    idf_ext._say, idf_ext._abort = _saved_say, _saved_abort
check("never writes into task.action_args", _ok, True)

t = Task("flash")
r = run_gate([t], {"port": "/dev/cu.usbmodem101"},
             env={"ESP32_WORKBENCH_ROOT": "/nonexistent-on-purpose"})
# root is unresolvable, so the gate goes INACTIVE before probing; the point
# here is only that an explicit -p is read from global_args, not re-derived.
check("explicit -p is honoured, not re-resolved",
      idf_ext._resolve_port(Args({"port": "/dev/cu.usbmodem101"})),
      "/dev/cu.usbmodem101")

print("\nbad ESP32_WORKBENCH_ROOT is reported, not silently ignored")
# Must run from a directory with no workbench above it, or the upward search
# finds the real repo and the gate proceeds to probe -- which is exactly what
# the first version of this test did wrong.
import tempfile
_cwd = os.getcwd()
with tempfile.TemporaryDirectory() as _td:
    os.chdir(_td)
    try:
        r = run_gate([Task("flash")],
                     env={"ESP32_WORKBENCH_ROOT": "/nonexistent-on-purpose"})
    finally:
        os.chdir(_cwd)
check("no abort when the override is bad", r["aborted"], [])
check("warns that the override was unusable",
      "does not contain" in r["out"], True)
# NOTE: do NOT assert on what the gate says AFTER the warning here.
# _find_root falls through to the real repo, so with a board attached the gate
# legitimately proceeds, probes it, and reports its verified backup. An earlier
# version asserted "verified backup" was absent -- which passed only while no
# hardware was connected. A test whose result depends on whether a board
# happens to be plugged in is testing the room, not the code.

# _find_root also anchors on the extension's OWN location, so a project copied
# out of templates/ still finds the workbench regardless of cwd. That is
# deliberate. To exercise the genuinely-not-found path, neutralise that anchor.
print("\ntruly outside any workbench")
_real_file = idf_ext.__file__
with tempfile.TemporaryDirectory() as _td:
    os.chdir(_td)
    idf_ext.__file__ = os.path.join(_td, "idf_ext.py")
    try:
        check("_find_root returns None with no workbench anywhere",
              idf_ext._find_root(_td), None)
        r2 = run_gate([Task("flash")])
        check("gate does not abort", r2["aborted"], [])
        check("gate declares itself INACTIVE", "INACTIVE" in r2["out"], True)
        check("and says it is proceeding unguarded",
              "unguarded" in r2["out"], True)
    finally:
        idf_ext.__file__ = _real_file
        os.chdir(_cwd)

print("\nextension registration")
ext = idf_ext.action_extensions({}, "/tmp/project")
check("returns a dict", isinstance(ext, dict), True)
check("declares 'version' (v6.x merge_action_lists requires it)",
      "version" in ext, True)
check("registers a global callback", "global_action_callbacks" in ext, True)
check("callback is ours", idf_ext.backup_gate in ext["global_action_callbacks"], True)

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all IDF gate tests passed")
