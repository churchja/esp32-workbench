# esp32-workbench

Identify, read, back up, and program any ESP32-family board over USB — without
destroying what was already on it.

Built around one idea: **a board identified once should never need identifying
again.** Probing writes to a persistent, provenance-tagged profile, so knowledge
accumulates instead of evaporating when a session ends.

## Quick start

```bash
python3 tools/esp32ident.py --list      # what is attached?
python3 tools/esp32ident.py --save      # identify it, write boards/<mac>.yaml
python3 tools/esp32flash.py backup      # full flash image + sha256 manifest
python3 tools/esp32dump.py  --all       # partitions, filesystems, NVS, forensics

# ESP-IDF (primary). Version pinned in .idf-version.
IDF_TOOLS_PATH=~/.espressif-6.0.3 . ~/esp/esp-idf-v6.0.3/export.sh
cp -r templates/idf-base projects/my-thing && cd projects/my-thing
idf.py set-target esp32c6 && idf.py build && idf.py flash
```

## Installing ESP-IDF

```bash
git clone -b v6.0.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
cd ~/esp/esp-idf-v6.0.3
export IDF_TOOLS_PATH="$HOME/.espressif-6.0.3"      # isolated per version
./install.sh esp32,esp32s3,esp32c3,esp32c5,esp32c6  # limits the toolchain download
```

~4.0GB repo plus ~1.7GB toolchains. `IDF_TOOLS_PATH` is set per version so two
IDF lines can coexist without colliding. Add a shell helper:

```bash
idf603() { export IDF_TOOLS_PATH="$HOME/.espressif-6.0.3"; . "$HOME/esp/esp-idf-v6.0.3/export.sh"; }
```

**Why v6.0.3:** full ESP32-C5/C6/H2/P4 support, and in Espressif's Service
period until ~2027-03. The v5.5 line entered Maintenance in July 2026 — their
own policy table answers "recommended for new projects?" with *No*. v6.1 was
eight days old with zero bugfix releases at time of choosing. Choosing v6.x
means giving up the Arduino-as-component path, which is why `templates/pio-base`
is kept for Arduino work.

Inside Claude Code, the skill at `.claude/skills/esp32-workbench/` loads
automatically when you work in this repo.

## Layout

| Path | What it is |
|---|---|
| `tools/esp32ident.py` | Six-stage identification. **Read-only — no write path exists** |
| `tools/esp32flash.py` | Backup / restore / flash / erase, behind the write gate |
| `tools/esp32dump.py` | Partition dump, SPIFFS/LittleFS extraction, NVS parse, forensics |
| `tools/gate.py` | The backup-gate decision, as a pure testable function |

| `tools/validate_profiles.py` | Enforces provenance rules; `--todo` lists open research |
| `tools/test_*.py` | 98 assertions across 4 files; no hardware needed |
| `smoke.sh` | One command: readiness + unit + schema + real PlatformIO |
| `boards/` | One profile per physical board, keyed by eFuse MAC. **The asset — commit these** |
| `templates/pio-base/` | PlatformIO starter that drives no GPIO, safe on unverified hardware |
| `backups/` | Flash images (gitignored) + manifests (tracked) |
| `docs/` | Design rationale and recommendations |
| `.claude/skills/esp32-workbench/` | The skill and its references |

## The two rules

**1. No write without a verified backup of that specific board.** Keyed by eFuse
MAC — not by port (reassigned every replug) and not by model (shared by many
physical objects). Enforced in two places: `esp32flash.py` gates its own
`flash`/`erase`, and a PlatformIO pre-upload hook gates `pio run -t upload`,
`-t uploadfs`, and the MCP server's `upload_firmware`. Verified by running it,
not just asserted. Bypass with `ESP32_NO_GATE=1` (PlatformIO) or
`--no-backup-i-accept-the-risk` (esp32flash), both verbose on purpose.

**2. Every fact carries its provenance.** `probed` came off the silicon and is
unarguable. `unverified` is a guess and must never be wired to power. Five
levels sit between. A value with no provenance is a schema violation, because a
plausible pin number with no traceable origin is indistinguishable from an
invention.

## What this cannot do

**Recover source code.** The chip holds a stripped binary; the C++ ceased to
exist at compile time. Recoverable: the flash image, partition layout, unpacked
filesystem contents, NVS key/values, and build fingerprints — project name, IDF
version, toolchain, build host paths, embedded URLs. Not the source.

## Is it working?

```bash
./smoke.sh
```

Exit 0 all green · 1 degraded (warnings, core paths fine) · 2 something failed.

It runs four layers, which prove progressively more:

| Layer | Proves | Needs a board? |
|---|---|---|
| `tools/doctor.py` | esptool present and which dialect, platform installed, gate wired, `backups/` writable | no |
| `tools/test_*.py` | pure logic: binary parsers, banner parsing, gate decision, provenance | no |
| `validate_profiles.py` | every stored profile satisfies the schema | no |
| real PlatformIO | template compiles; the pre-upload hook actually fires | no |

`doctor.py` is the one to run when something feels off — it cross-checks its own
reading of the esptool dialect against what `Esptool()` concludes, so a silent
disagreement (which would make every flash command use the wrong spelling)
shows up as a failure rather than a mystery.

### First contact — when a board is finally attached

Everything above passes with no hardware. The serial layer does not, so run this
once, in order, the first time you plug something in:

```bash
python3 tools/doctor.py                       # 'board' should turn OK
python3 tools/esp32ident.py --list            # port visible?

ESP32_RECORD_FIXTURE=tests/fixtures/mychip.txt   python3 tools/esp32ident.py --save          # identify AND record real output

python3 tools/test_banner.py                  # fixtures now test conformance
python3 tools/validate_profiles.py            # profile satisfies the schema
python3 tools/esp32flash.py backup            # ~3 min for 8MB at 460800 baud
python3 tools/esp32flash.py verify            # hash matches
./smoke.sh                                    # should go all green
```

The `ESP32_RECORD_FIXTURE` step matters more than it looks. It captures esptool's
real banner text, which converts `test_banner.py` from fixtures *I* wrote into
conformance tests against *your* hardware — and permanently pins the format so a
future esptool upgrade breaks a test instead of breaking a backup.

That is not hypothetical. The banner parser was originally written against
esptool 4.x conventions (`Chip is`, `Crystal is`, one `MAC:` line). esptool 5.2.0
emits `Chip type:`, `Crystal frequency:`, and on C6/C5/H2 **three** MAC lines
where the first is an 8-byte EUI64. The old parser matched none of the first two
and silently truncated the EUI64 into a wrong 6-byte identity — and identity is
the backup key. Caught by reading esptool's source; it would otherwise have
surfaced as a mystery on first contact.

### What is still not covered

Anything that opens a serial port: the esptool subprocess calls themselves,
timeout and retry behaviour, `restore`, and bootloader-entry edge cases. Parsing
of their output is now tested; the I/O around it is not.

## Requirements

esptool (5.x or 4.x — the dialect is detected at runtime), PlatformIO, Python 3.
`pyserial` ships with esptool. Optional: `mklittlefs`/`mkspiffs` (PlatformIO
fetches them on first LittleFS build) or `littlefs-python` for filesystem
extraction.
