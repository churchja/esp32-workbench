#!/usr/bin/env python3
"""
validate_profiles.py -- make provenance a mechanism instead of an honour system.

The repo documents provenance rules in three places. Until this existed, nothing
checked them, which made the central discipline exactly the kind of habit the
backup gate was built to replace.

Rules enforced:

  1. Every leaf in a fact-bearing section is a fact: {value, provenance, ...}.
     A bare value is a violation -- it makes a guess indistinguishable from a
     measurement, which is the whole failure this schema exists to prevent.
  2. provenance is one of the known levels.
  3. vendor_doc / community carry a `source` that looks like a URL. A claim
     about an external document with no traceable origin is an invention.
  4. `verified` carries a matching verification_log entry naming a physical
     test. Nothing may claim hardware confirmation that never happened.

Reported but not fatal:
  - `unverified` entries under pinmap/power, surfaced so a live hazard is
    visible rather than buried.

Exit 0 clean, 1 on any violation.
"""

import argparse
import glob
import os
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from esp32ident import PROVENANCE_ORDER, SOURCED_LEVELS  # noqa: E402

FACT_SECTIONS = ("identity", "board", "display", "pinmap", "power", "peripherals")
HAZARD_SECTIONS = ("pinmap", "power")


def is_fact(node):
    return isinstance(node, dict) and "value" in node and "provenance" in node


def walk_facts(node, path=""):
    """Yield (path, fact_or_None) for every leaf under a fact-bearing section."""
    if is_fact(node):
        yield path, node
        return
    if isinstance(node, dict):
        if not node:
            return
        for k, v in node.items():
            yield from walk_facts(v, f"{path}.{k}" if path else str(k))
    elif isinstance(node, list):
        # A list of plain records (e.g. partitions) is data, not facts.
        return
    else:
        yield path, None  # bare leaf where a fact was required


def validate(path):
    errors, warnings = [], []
    try:
        with open(path) as fh:
            prof = yaml.safe_load(fh)
    except Exception as e:  # noqa: BLE001
        return [f"{os.path.basename(path)}: unparseable YAML: {e}"], []
    if not isinstance(prof, dict):
        return [f"{os.path.basename(path)}: top level is not a mapping"], []

    name = os.path.basename(path)
    logged = set()
    # `tested` may name one field or a list of them. One physical test often
    # verifies several facts at once -- lighting a panel proves its power pin,
    # reset, clock, data, chip-select and DC line in a single observation --
    # and forcing that into six near-identical entries buries the method in
    # duplication rather than recording it once.
    for entry in prof.get("verification_log") or []:
        if isinstance(entry, dict) and entry.get("tested"):
            t = entry["tested"]
            for item in (t if isinstance(t, list) else [t]):
                logged.add(str(item))

    for section in FACT_SECTIONS:
        node = prof.get(section)
        if node is None:
            continue
        for fpath, f in walk_facts(node, section):
            if f is None:
                errors.append(
                    f"{name}: {fpath} is a bare value with no provenance")
                continue
            prov = f.get("provenance")
            if prov not in PROVENANCE_ORDER:
                errors.append(f"{name}: {fpath} has unknown provenance {prov!r}")
                continue
            src = f.get("source")
            if prov in SOURCED_LEVELS:
                if not src:
                    errors.append(
                        f"{name}: {fpath} is '{prov}' with no source URL "
                        f"(downgrade to 'unverified' or cite it)")
                elif not str(src).startswith(("http://", "https://")):
                    errors.append(
                        f"{name}: {fpath} source is not a URL: {src!r}")
            if prov == "verified":
                key = fpath.split(".", 1)[-1] if "." in fpath else fpath
                if fpath not in logged and key not in logged:
                    errors.append(
                        f"{name}: {fpath} claims 'verified' but no "
                        f"verification_log entry tests it")
            if prov == "unverified" and fpath.split(".")[0] in HAZARD_SECTIONS:
                warnings.append(
                    f"{name}: {fpath} is unverified — do not drive this pin")
    return errors, warnings


# Fields esp32ident emits UNCONDITIONALLY on a successful probe. Absence means
# the profile predates the field, not that the chip lacks it.
#
#   idf_target   build_profile() derives it from chip_family for every chip,
#                emitting None with provenance 'unverified' when nothing matches
#                (an ESP8266 does this). The KEY is always present.
#   usb_product  identify_usb() records the descriptor string whenever the port
#                reports one, which in practice is always.
ALWAYS_EMITTED = ("idf_target", "usb_product")

# Absence here is a FACT ABOUT THE CHIP and must never read as staleness.
# Flagging these would fire forever on hardware that simply does not report
# them, which trains you to ignore the check.
#
#   flash_mode / flash_voltage  ESP32-family eFuse lines. The S3 emits both,
#                               the S2 only mode, the ESP8266 neither.
#   usb_mode                    esptool prints it only for parts with a USB
#                               peripheral; absent behind a UART bridge.
CHIP_DEPENDENT = ("flash_mode", "flash_voltage", "usb_mode", "crystal",
                  "chip_revision")


def profile_state(prof):
    """
    Return (state, detail).

      current     nothing a re-probe would add
      incomplete  the chip was never reached -- mac is inferred, no partitions.
                  A successful re-probe would upgrade provenance and fill in
                  partitions and applications.
      stale       predates a field the tool now always emits

    `incomplete` outranks `stale`: re-probing an unreached board fixes both.
    """
    ident = prof.get("identity") or {}
    if not ident:
        return "incomplete", "no identity recorded"

    mac = ident.get("mac") or {}
    reached = mac.get("provenance") == "probed" or "partitions" in ident
    if not reached:
        why = []
        if mac.get("provenance") and mac["provenance"] != "probed":
            why.append(f"mac is '{mac['provenance']}', not probed")
        if "partitions" not in ident:
            why.append("no partition table")
        return "incomplete", "; ".join(why) or "chip never reached"

    missing = [k for k in ALWAYS_EMITTED if k not in ident]
    if missing:
        return "stale", "missing " + ", ".join(missing)
    return "current", ""


def open_research(paths):
    """Collect unresolved research_queue entries so something consumes them."""
    out = {}
    for p in paths:
        try:
            with open(p) as fh:
                prof = yaml.safe_load(fh) or {}
        except Exception:  # noqa: BLE001
            continue
        items = [i for i in (prof.get("research_queue") or [])
                 if isinstance(i, dict) and i.get("status") != "resolved"]
        if items:
            out[os.path.basename(p)] = items
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*",
                    help="profile files (default: every boards/*.yaml)")
    ap.add_argument("--quiet", action="store_true", help="errors only")
    ap.add_argument("--todo", action="store_true",
                    help="list unresolved research items across all profiles")
    ap.add_argument("--stale", action="store_true",
                    help="report which profiles a re-probe would improve")
    args = ap.parse_args()

    paths = args.paths or sorted(glob.glob(os.path.join(REPO, "boards", "*.yaml")))
    if not paths:
        print("No profiles to validate. Identify a board first:")
        print("  python3 tools/esp32ident.py --save")
        return 0

    all_err, all_warn = [], []
    for p in paths:
        e, w = validate(p)
        all_err += e
        all_warn += w

    # research_queue is written by esp32ident.py. Until this existed nothing
    # read it, which made it a reminder-to-do-X rather than a work list --
    # a representation of the action substituted for the action.
    if args.stale:
        import yaml as _y
        rows, worth = [], 0
        for path in paths:
            name = os.path.basename(path)
            if name.startswith("_"):
                continue
            try:
                prof = _y.safe_load(open(path)) or {}
            except Exception as e:  # noqa: BLE001
                rows.append(("unreadable", name, str(e)[:50])); continue
            state, detail = profile_state(prof)
            rows.append((state, name, detail))
            worth += state in ("stale", "incomplete")
        width = max((len(n) for _s, n, _d in rows), default=10)
        for state, name, detail in sorted(rows):
            print(f"  {state:<11} {name:<{width}}  {detail}")
        print()
        if worth:
            print(f"{worth} profile(s) a re-probe would improve.")
            print("  stale      -> re-probe adds fields the tool now always emits")
            print("  incomplete -> chip was never reached; BOOT+RESET, then re-probe")
        else:
            print("All profiles are current. A re-probe would add nothing.")
            print("Chip-dependent absences (%s) are\n"
                  "facts about the hardware, not gaps."
                  % ", ".join(CHIP_DEPENDENT))
        return 1 if worth else 0

    todos = open_research(paths)
    if args.todo:
        if not todos:
            print("No open research items.")
        for name, items in todos.items():
            print(f"\n{name}")
            for it in items:
                print(f"  [{it.get('status', '?'):<8}] {it.get('field')}")
                if it.get("why"):
                    print(f"             why: {it['why']}")
                if it.get("how"):
                    print(f"             how: {it['how']}")
        return 1 if all_err else 0

    if all_warn and not args.quiet:
        print(f"{len(all_warn)} warning(s):")
        for w in all_warn:
            print(f"  WARN  {w}")
    if all_err:
        print(f"\n{len(all_err)} violation(s):")
        for e in all_err:
            print(f"  ERROR {e}")
        return 1
    if not args.quiet:
        n = sum(len(v) for v in todos.values())
        print(f"{len(paths)} profile(s) valid."
              + (f" {n} open research item(s) — see --todo." if n else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
