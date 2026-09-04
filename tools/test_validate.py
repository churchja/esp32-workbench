#!/usr/bin/env python3
"""
Tests for validate_profiles.py.

A validator that only ever passes is not a validator. Each case below is a
profile that MUST be rejected, and the reason it must be rejected.

Run:  python3 tools/test_validate.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from validate_profiles import validate  # noqa: E402

FAIL = []


def run(name, yaml_text, want_error_substr=None, want_warn_substr=None):
    with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as fh:
        fh.write(yaml_text)
        path = fh.name
    try:
        errors, warnings = validate(path)
    finally:
        os.unlink(path)

    if want_error_substr is None:
        ok = not errors
        detail = f"expected clean, got {errors}"
    else:
        ok = any(want_error_substr in e for e in errors)
        detail = f"expected error containing {want_error_substr!r}, got {errors}"
    if ok and want_warn_substr is not None:
        ok = any(want_warn_substr in w for w in warnings)
        detail = f"expected warning containing {want_warn_substr!r}, got {warnings}"

    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok:
        print(f"          {detail}")
        FAIL.append(name)


HEAD = "schema_version: 1\nprofile_id: t\n"

print("rejects what it must reject")

run("bare value with no provenance",
    HEAD + "pinmap:\n  lcd_bl: 22\n",
    "bare value with no provenance")

run("unknown provenance level",
    HEAD + "identity:\n  chip: {value: X, provenance: vibes}\n",
    "unknown provenance")

run("vendor_doc with no source",
    HEAD + "board:\n  vendor: {value: Waveshare, provenance: vendor_doc}\n",
    "no source URL")

run("community with no source",
    HEAD + "pinmap:\n  sda: {value: 8, provenance: community}\n",
    "no source URL")

run("source that is not a URL",
    HEAD + "board:\n  vendor: {value: W, provenance: vendor_doc, source: 'the datasheet'}\n",
    "source is not a URL")

run("verified with no verification_log at all",
    HEAD + "display:\n  row_offset: {value: 34, provenance: verified}\n",
    "no verification_log entry")

run("verified whose log entry tests something else",
    HEAD + "display:\n  row_offset: {value: 34, provenance: verified}\n"
         + "verification_log:\n  - {tested: pinmap.lcd_bl, result: verified}\n",
    "no verification_log entry")

print("\naccepts what it must accept")

run("probed needs no source",
    HEAD + "identity:\n  mac: {value: 'aa:bb', provenance: probed}\n",
    None)

run("vendor_doc with a real URL",
    HEAD + "board:\n  vendor: {value: W, provenance: vendor_doc, source: 'https://x.com/y'}\n",
    None)

run("verified with a matching log entry",
    HEAD + "display:\n  row_offset: {value: 34, provenance: verified}\n"
         + "verification_log:\n  - {tested: display.row_offset, result: verified}\n",
    None)

run("non-fact sections are not policed",
    HEAD + "research_queue:\n  - {field: pinmap, status: unknown}\n"
         + "identified_at: '2026-09-04'\n",
    None)

print("\nsurfaces hazards without failing the build")

run("unverified pin warns but does not error",
    HEAD + "pinmap:\n  lcd_bl: {value: 22, provenance: unverified}\n",
    None, want_warn_substr="do not drive this pin")

print("\nthe shipped example must satisfy its own schema")
repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ex = os.path.join(repo, "boards", "_example.yaml")
if os.path.exists(ex):
    e, _w = validate(ex)
    print(f"  {'PASS' if not e else 'FAIL'}  boards/_example.yaml validates")
    if e:
        FAIL.append("example")
        print(f"          {e}")
else:
    print("  FAIL  boards/_example.yaml missing")
    FAIL.append("example missing")


# ---------------------------------------------------------------------------
# merge_profile -- re-probing must not destroy researched knowledge
# ---------------------------------------------------------------------------
print("\nprofile merge on re-identify")

from esp32ident import merge_profile  # noqa: E402


def check(name, got, want):
    """This file's other helper is run(); merge tests compare values directly."""
    if got == want:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}\n          got:  {got!r}\n          want: {want!r}")
        FAIL.append(name)


OLD = {
    "profile_id": "aabbcc",
    "identified_at": "2026-09-01T00:00:00+00:00",
    "identity": {
        "chip_family": {"value": "ESP32-S3", "provenance": "probed"},
        "hand_added": {"value": 1, "provenance": "community",
                       "source": "https://example.com"},
    },
    "board": {"read_baud_max": {"value": 230400, "provenance": "verified"}},
    "pinmap": {"lcd_bl": {"value": 22, "provenance": "vendor_doc",
                          "source": "https://x"}},
    "verification_log": [{"date": "2026-09-01", "tested": "board.read_baud_max",
                          "result": "verified"}],
    "research_queue": [{"field": "power", "status": "resolved"},
                       {"field": "display", "status": "unknown"}],
}
NEW = {
    "profile_id": "aabbcc",
    "identified_at": "2026-09-04T00:00:00+00:00",
    "identity": {
        "chip_family": {"value": "ESP32-S3", "provenance": "probed"},
        "flash_size": {"value": "8MB", "provenance": "probed"},
    },
    "research_queue": [{"field": "pinmap", "status": "unknown"},
                       {"field": "power", "status": "unknown"},
                       {"field": "display", "status": "unknown"}],
}
m = merge_profile(OLD, NEW)

check("researched 'board' survives a re-probe", m["board"], OLD["board"])
check("researched 'pinmap' survives", m["pinmap"], OLD["pinmap"])
check("verification_log survives", len(m["verification_log"]), 1)
check("newly probed field is present", m["identity"]["flash_size"]["value"], "8MB")
check("hand-added identity field survives",
      m["identity"]["hand_added"]["value"], 1)
check("identified_at refreshes to the new probe",
      m["identified_at"], NEW["identified_at"])
check("first_identified_at keeps the original",
      m["first_identified_at"], OLD["identified_at"])

fields = {i["field"] for i in m["research_queue"]}
check("already-answered section drops out of the queue", "pinmap" in fields, False)
check("resolved item stays resolved", "power" in fields, False)
check("genuinely open item remains", "display" in fields, True)

check("no prior profile -> new returned unchanged", merge_profile({}, NEW), NEW)
check("None prior -> new returned unchanged", merge_profile(None, NEW), NEW)

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all validator tests passed")
