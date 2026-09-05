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
    # These carry CONTENT deliberately. The bare versions this fixture used to
    # hold were why the merge could replace a researched entry with generated
    # boilerplate and pass: the assertions compared field NAMES, never bodies.
    "research_queue": [
        {"field": "power", "status": "resolved",
         "why": "answered on hardware", "how": "read the PMU over I2C"},
        {"field": "display", "status": "unknown",
         "why": "researched at length", "danger": "GPIO38 gates the panel rail"},
        {"field": "schematic_trust", "status": "caution",
         "why": "a vendor schematic disagreed with the silicon"},
    ],
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
byfield = {i["field"]: i for i in m["research_queue"]}
check("already-answered section drops out of the queue", "pinmap" in fields, False)
check("genuinely open item remains", "display" in fields, True)

# BEHAVIOUR CHANGE, made deliberately. This previously asserted that a resolved
# item is DELETED from the queue. It is now kept, with its status and body,
# because deleting it discards the record of how it was resolved -- and
# validate_profiles --todo already filters `status != "resolved"`, so keeping it
# does not pollute the open-work list.
check("a resolved item is kept, not deleted", "power" in fields, True)
check("and it stays resolved", byfield["power"]["status"], "resolved")

# The gap the old fixture could not see: an old entry's CONTENT must survive a
# re-probe. This is the regression that silently destroyed five researched
# entries on a real profile and went unnoticed into a commit.
check("a researched entry keeps its body",
      byfield["display"].get("danger"), "GPIO38 gates the panel rail")
check("and is not replaced by the generated template",
      byfield["display"].get("why"), "researched at length")
check("a resolved entry keeps its body too",
      byfield["power"].get("how"), "read the PMU over I2C")

# A field the generator does not emit at all must not vanish.
check("a custom queue field survives", "schematic_trust" in fields, True)
check("with its body intact", byfield["schematic_trust"].get("why"),
      "a vendor schematic disagreed with the silicon")

# And a genuinely new open item still gets added.
m2 = merge_profile(OLD, {**NEW, "research_queue": [{"field": "antenna",
                                                    "status": "unknown"}]})
check("a new generated item is still added",
      "antenna" in {i["field"] for i in m2["research_queue"]}, True)

check("no prior profile -> new returned unchanged", merge_profile({}, NEW), NEW)
check("None prior -> new returned unchanged", merge_profile(None, NEW), NEW)


# ---------------------------------------------------------------------------
# board identity: a failed probe must not orphan a second profile
# ---------------------------------------------------------------------------
print("\nboard identity and orphan adoption")

from esp32ident import (normalize_mac, usb_serial_mac, profile_key,  # noqa: E402
                        adopt_orphans)

for raw, want in [
    ("d4:f9:8d:66:13:64", "d4f98d661364"),   # QT Py S2, real
    ("E0:72:A1:FB:9C:5C", "e072a1fb9c5c"),   # ESP32-S3, real
    ("D4-F9-8D-66-13-64", "d4f98d661364"),   # dash separators
    ("d4f98d661364",      "d4f98d661364"),   # already bare
    ("0",                 None),  # REAL: the S2 reports this in download mode
    ("",                  None),
    (None,                None),
    ("not-a-mac",         None),
    ("d4:f9:8d:66:13",    None),  # 5 octets
    ("60:55:f9:ff:fe:f7:2c:a2", None),  # 8-byte EUI64 is NOT a MAC
]:
    check(f"normalize_mac({raw!r})", normalize_mac(raw), want)

# Key precedence. The USB-serial fallback is what stops a failed probe from
# writing unknown-<hash>.yaml and a later good probe writing a SECOND file.
check("eFuse MAC beats USB serial",
      profile_key({"mac": {"value": "aa:bb:cc:dd:ee:ff"},
                   "usb_serial_number": {"value": "11:22:33:44:55:66"}}),
      "aabbccddeeff")
check("USB serial used when no MAC probed",
      profile_key({"usb_serial_number": {"value": "d4:f9:8d:66:13:64"}}),
      "d4f98d661364")
check("failed probe + junk serial falls back to a hash",
      profile_key({"usb_serial_number": {"value": "0"}}).startswith("unknown-"),
      True)
check("a failed probe and a good probe of the SAME board agree on the key",
      profile_key({"usb_serial_number": {"value": "d4:f9:8d:66:13:64"}}),
      profile_key({"mac": {"value": "d4:f9:8d:66:13:64"},
                   "usb_serial_number": {"value": "0"}}))

# Adoption: narrow on purpose. Merging two different boards is worse than
# leaving an orphan.
import tempfile as _tf, os as _os, yaml as _yaml  # noqa: E402
with _tf.TemporaryDirectory() as _d:
    _yaml.safe_dump(
        {"identity": {"usb_serial_number": {"value": "d4:f9:8d:66:13:64",
                                            "provenance": "usb"}}},
        open(_os.path.join(_d, "unknown-aaaa111122.yaml"), "w"))
    _yaml.safe_dump(
        {"identity": {"usb_serial_number": {"value": "11:22:33:44:55:66",
                                            "provenance": "usb"}}},
        open(_os.path.join(_d, "unknown-bbbb333344.yaml"), "w"))
    _yaml.safe_dump({"identity": {}}, open(_os.path.join(_d, "d4f98d661364.yaml"), "w"))

    got, paths = adopt_orphans(_d, "d4f98d661364", {})
    check("adopts the matching orphan", len(got), 1)
    check("names the right file",
          _os.path.basename(paths[0]) if paths else None, "unknown-aaaa111122.yaml")
    check("leaves a DIFFERENT board's orphan alone",
          any("bbbb3333" in p for p in paths), False)
    check("never touches a real MAC-keyed profile",
          any("d4f98d661364.yaml" in p for p in paths), False)
    check("no match -> nothing adopted", adopt_orphans(_d, "ffffffffffff", {})[0], [])
    check("missing directory is safe",
          adopt_orphans(_os.path.join(_d, "nope"), "d4f98d661364", {})[0], [])


print("\nmerge: informative beats newer for mode-dependent USB fields")

# A board's USB identity depends on which MODE it is in. Adopting an app-mode
# profile into a ROM-mode probe must not downgrade these.
APP_MODE = {"identity": {
    "usb_product": {"value": "QT Py ESP32-S2", "provenance": "usb"},
    "usb_serial_number": {"value": "d4:f9:8d:66:13:64", "provenance": "usb"},
}}
ROM_MODE = {"identity": {
    "usb_product": {"value": "ESP32-S2", "provenance": "usb"},
    "usb_serial_number": {"value": "0", "provenance": "usb"},
    "mac": {"value": "d4:f9:8d:66:13:64", "provenance": "probed"},
}}
m = merge_profile(APP_MODE, ROM_MODE)
check("board name survives a ROM-mode re-probe",
      m["identity"]["usb_product"]["value"], "QT Py ESP32-S2")
check("MAC-shaped serial is not replaced by '0'",
      m["identity"]["usb_serial_number"]["value"], "d4:f9:8d:66:13:64")
check("probed MAC still wins as an identity",
      m["identity"]["mac"]["provenance"], "probed")

# ...but a genuinely better new value must still win.
m2 = merge_profile(
    {"identity": {"usb_product": {"value": "ESP32-S2", "provenance": "usb"}}},
    {"identity": {"usb_product": {"value": "QT Py ESP32-S2", "provenance": "usb"}}})
check("a MORE specific new product string is kept",
      m2["identity"]["usb_product"]["value"], "QT Py ESP32-S2")

m3 = merge_profile(
    {"identity": {"usb_serial_number": {"value": "0", "provenance": "usb"}}},
    {"identity": {"usb_serial_number": {"value": "d4:f9:8d:66:13:64",
                                        "provenance": "usb"}}})
check("a newly-seen MAC-shaped serial is kept",
      m3["identity"]["usb_serial_number"]["value"], "d4:f9:8d:66:13:64")


print("\nmerge: a failed probe must never weaken a measured fact")

# Real regression: a UF2-mode probe of the QT Py ESP32-S2 could not reach the
# chip, synthesised mac from the USB serial as `inferred`, and the merge kept
# THAT over the `probed` value an earlier successful probe had established.
GOOD = {"identity": {
    "mac": {"value": "d4:f9:8d:66:13:64", "provenance": "probed"},
    "chip_family": {"value": "ESP32-S2FNR2", "provenance": "probed"},
}}
FAILED = {"identity": {
    "mac": {"value": "d4:f9:8d:66:13:64", "provenance": "inferred"},
}}
m = merge_profile(GOOD, FAILED)
check("probed mac is NOT downgraded to inferred",
      m["identity"]["mac"]["provenance"], "probed")
check("probed field absent from the failed probe survives",
      m["identity"]["chip_family"]["provenance"], "probed")

# ...but a genuine UPGRADE must still be taken.
m2 = merge_profile(FAILED, GOOD)
check("inferred IS upgraded to probed by a good probe",
      m2["identity"]["mac"]["provenance"], "probed")

# verified outranks probed; a re-probe must not knock it back down.
m3 = merge_profile(
    {"identity": {"x": {"value": 1, "provenance": "verified"}}},
    {"identity": {"x": {"value": 1, "provenance": "probed"}}})
check("verified is not downgraded to probed",
      m3["identity"]["x"]["provenance"], "verified")

# equal rank -> the fresher observation wins
m4 = merge_profile(
    {"identity": {"y": {"value": "old", "provenance": "probed"}}},
    {"identity": {"y": {"value": "new", "provenance": "probed"}}})
check("equal provenance keeps the newer value",
      m4["identity"]["y"]["value"], "new")


print("\nprofile staleness: current / stale / incomplete")

from validate_profiles import profile_state, ALWAYS_EMITTED, CHIP_DEPENDENT  # noqa: E402

def prof(**ident):
    base = {"mac": {"value": "aa:bb:cc:dd:ee:ff", "provenance": "probed"},
            "chip_family": {"value": "ESP32-S3", "provenance": "probed"},
            "idf_target": {"value": "esp32s3", "provenance": "inferred"},
            "usb_product": {"value": "thing", "provenance": "usb"},
            "partitions": {"value": [{"label": "nvs"}], "provenance": "probed"}}
    base.update(ident)
    return {"identity": base}

check("a complete profile is current", profile_state(prof())[0], "current")

for f in ALWAYS_EMITTED:
    p2 = prof(); p2["identity"].pop(f)
    st, why = profile_state(p2)
    check(f"missing {f} -> stale", st, "stale")
    check(f"and says which field: {f}", f in why, True)

# The negative case that keeps the check from crying wolf. These absences are
# facts about the chip -- an ESP8266 reports none of them -- and flagging them
# would fire forever on hardware that simply does not have them.
p2 = prof()
for f in CHIP_DEPENDENT:
    p2["identity"].pop(f, None)
check("chip-dependent absences are NOT stale", profile_state(p2)[0], "current")

# A probe that never reached the chip is a different problem with a different
# fix (BOOT+RESET, then re-probe), so it gets its own state.
p2 = prof(mac={"value": "aa:bb", "provenance": "inferred"})
p2["identity"].pop("partitions")
st, why = profile_state(p2)
check("chip never reached -> incomplete", st, "incomplete")
check("and explains why", "not probed" in why, True)

# Precedence: re-probing an unreached board fixes both, so incomplete wins.
p2 = prof(mac={"value": "aa:bb", "provenance": "inferred"})
p2["identity"].pop("partitions"); p2["identity"].pop("usb_product")
check("incomplete outranks stale", profile_state(p2)[0], "incomplete")

check("empty identity is incomplete", profile_state({"identity": {}})[0], "incomplete")
check("a probed mac alone is enough to count as reached",
      profile_state(prof())[0], "current")

print()
if FAIL:
    print(f"{len(FAIL)} FAILURE(S): {', '.join(FAIL)}")
    sys.exit(1)
print("all validator tests passed")
