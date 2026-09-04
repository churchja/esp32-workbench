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

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all validator tests passed")
