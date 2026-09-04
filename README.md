# esp32-workbench

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.3-informational)](.idf-version)
[![hardware validated](https://img.shields.io/badge/hardware%20validated-ESP32--S3%20%7C%20S2%20%7C%20ESP8266-success)](#validated-hardware)
[![tests](https://img.shields.io/badge/tests-no%20CI%2C%20run%20.%2Fsmoke.sh-lightgrey)](#is-it-working)

<sub>No "build passing" badge: there is no CI on this repo, so such a badge
would assert something nothing checks. Run <code>./smoke.sh</code> — it tells
you the truth about your machine, which a static image cannot.</sub>


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
| `tools/validate_profiles.py` | Enforces provenance rules; `--todo` lists open research, `--stale` which profiles a re-probe would improve |
| `tools/doctor.py` | Is it operational *here*? Versions, wiring, writability |
| `tools/usbwatch.py` | Watches USB devices arrive and leave, and says what each identity *means* |
| `tools/test_*.py` | 312 assertions across 8 files; no hardware needed |
| `smoke.sh` | One command: readiness + unit + schema + real ESP-IDF and PlatformIO builds |
| `boards/` | One profile per physical board, keyed by eFuse MAC. **The asset — commit these** |
| `templates/idf-base/` | **The default.** ESP-IDF starter that drives no peripheral at all |
| `templates/idf-usb-console/` | Sibling for **USB-OTG parts (S2)**, where a console needs TinyUSB |
| `templates/pio-base/` | PlatformIO/Arduino sibling, for the Arduino display-library ecosystem |
| `backups/` | Flash images (gitignored) + manifests (tracked) |
| `docs/` | Design rationale and recommendations |
| `.claude/skills/esp32-workbench/` | The skill and its references |

### Which template

| Your board | Template | Why |
|---|---|---|
| S3, C3, C6, C5, H2, P4 | `idf-base` | USB-Serial/JTAG gives a secondary console free; readable unchanged |
| **ESP32-S2** | **`idf-usb-console`** | USB-OTG only — no secondary console exists, so TinyUSB must drive it |
| classic ESP32 behind a bridge | `idf-base` | UART *is* the console |
| anything needing TFT_eSPI, LovyanGFX, GxEPD2 | `pio-base` | the Arduino ecosystem, which ESP-IDF does not replace |

`idf-base` drives **no peripheral at all** — no GPIO, no radios — which is what
makes it safe to flash onto a board whose pin map is unverified. `idf-usb-console`
deliberately breaks that to drive the USB-OTG peripheral, and costs a manual
BOOT+RESET before every reflash, because TinyUSB owns USB and esptool has
nothing to handshake with. They are siblings for that reason, not versions.

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

## Validated hardware

Every claim below was produced by running against the board, not by reading a
datasheet. Values are lifted from `boards/*.yaml`, so they cannot drift from
what the tools actually recorded.

| | ESP32-S3 devkit | QT Py ESP32-S2 **A** | QT Py ESP32-S2 **B** | CH340 board |
|---|---|---|---|---|
| Chip (probed) | `ESP32-S3 (QFN56) (revision v0.2)` | `ESP32-S2FNR2 (revision v0.0)` | `ESP32-S2FNR2 (revision v0.0)` | `ESP8266EX` |
| `idf.py set-target` | `esp32s3` | `esp32s2` | `esp32s2` | **none** |
| Flash | 8MB, quad | 4MB embedded | 4MB embedded | 4MB |
| PSRAM | 8MB (AP_3v3) | 2MB embedded | 2MB embedded | none |
| Radios | Wi-Fi, BT 5 (LE) | Wi-Fi only | Wi-Fi only | Wi-Fi only |
| Cores | Dual + LP, 240MHz | Single, 240MHz | Single, 240MHz | Single, 160MHz |
| USB peripheral | `USB-Serial/JTAG` | `USB-OTG` | `USB-OTG` | **none** — bridge |
| USB product string | not captured | `QT Py ESP32-S2` | `QT Py ESP32-S2` | `USB2.0-Serial` |
| Partitions read | 4 | 6 | 6 | **0** |
| **Max read baud** | **230400** | **460800** | **460800** | **230400** |
| Backup | ✅ | ✅ | ✅ | ✅ |
| Flash · restore | ✅ | ✅ | ✅ | restore ✅ |
| Console over USB | ✅ | ✅ via `idf-usb-console` | ✅ same | n/a — UART |
| Profile | `e072a1fb9c5c` | `d4f98d661364` | `d4f98d66124a` | `bcddc2246e97` |

**Columns A and B are the same model, and every row above is identical.** Same
product string, same VID/PID, same chip, same partition layout, same factory
images with the same build dates. The only thing distinguishing them is the
eFuse MAC — `d4:f9:8d:66:13:64` against `d4:f9:8d:66:12:4a`.

That is the collision the MAC-keyed design exists to prevent, and until a second
QT Py appeared it had never been tested: every earlier board was a different
model. They landed on separate profiles, so board A kept its backup manifest and
its `verified` read ceiling instead of being silently overwritten. Anything keyed
on what a person would call "the board" would have merged them.

Three mechanisms had to cooperate, each verified in passing: the USB-serial
fallback keyed B's first (failed) probe correctly and its derived MAC later
matched eFuse exactly; `adopt_orphans` did **not** fire, because it matches only
on exact normalized-serial equality — two boards differing in the last three
octets are precisely what a looser rule would have collapsed; and `mac` upgraded
`inferred` → `probed` in place on B's successful probe.

Note the baud row. Across **four** boards the ceiling tracks the **USB
interface**, not the individual board:

| Interface | Boards | Max read baud |
|---|---|---|
| USB-OTG | 2 × ESP32-S2 | **460800** |
| USB-Serial/JTAG | ESP32-S3 | 230400 |
| CH340 UART bridge | ESP8266 | 230400 |

Two *independent* S2s agree at 460800 exactly, while the S3 fails reproducibly
at that rate.

An earlier version of this file claimed the ceiling was per-board, arguing that
"the two native boards disagree with each other". That conflated USB-OTG with
USB-Serial/JTAG — different peripherals — and was written from three data
points. The fourth broke it. Split by interface, all four agree.

The ladder is still the right design, but for the plainer reason: **no constant
works.** 460800 makes S3 and ESP8266 backups *fail*; 115200 makes both S2s four
times slower than necessary. `esp32flash.py` negotiates rather than assumes.

`n` is small — three interfaces, and only USB-OTG has two boards. Treat the
table as a measured pattern, not a law.

Timing corroborates the mechanism. Against the 10-bits-per-byte serial framing
model, the S3 (USB-Serial/JTAG) measured 731s vs 728 predicted and the ESP8266
(real UART bridge) 202s vs 182 — both close. Only the S2's **USB-OTG** ran far
faster than predicted (33s vs 91), because OTG CDC does not throttle to the
nominal baud. The model stays a safe *upper* bound, which is all the derived
timeout needs.

**The S3 proved the happy path.** Identify, back up (8MB), flash different
firmware, run it, restore — and the restored image is **byte-identical** by
SHA-256 across all 8,388,608 bytes. It also established the repo's first
`verified` fact by physical test: 256KB reads succeeded at 115200 and 230400,
and failed reproducibly at 460800 and 921600.

**The S2 proved the failure paths**, and was worth more. It carries `tinyuf2`
alongside an Arduino app plus a FAT partition, so USB is claimed by application
firmware and esptool often cannot auto-enter the bootloader. Bugs only it could
find:

- a probe that fails still reads USB, so `profile_key()` hashed and orphaned a
  second profile for one board
- `--after hard-reset` ended every probe stage by resetting, consuming the
  single manual BOOT+RESET the pipeline depended on
- the same default in `backup` re-enumerated the board mid-workflow, so the
  `idf.py flash` that followed opened a port that no longer existed
- back-to-back esptool calls contended for the USB CDC endpoint
- a failed probe **downgraded** a `probed` MAC to `inferred`, because the merge
  only filled in absent fields — a guess overwriting a measurement
- its USB identity is **mode-dependent** — `239a:8111` "QT Py ESP32-S2" under
  tinyuf2, `303a:0002` "ESP32-S2" under the ROM bootloader *or* an app using
  native CDC, which are indistinguishable from the descriptor alone

Its restore was verified **by region**, which a whole-image hash could not do:
bootloader, partition table, `ota_0`, `ota_1`, `uf2` and `ffat` all
byte-identical; 50 bytes of 4,194,304 differ, all inside `nvs`, written by
tinyuf2 when the restored firmware booted. A whole-image SHA-256 answers "did
the write land" but cannot separate "restore failed" from "board booted and did
its job". Comparing against the probed partition table can — a concrete reason
the profile carries the layout and not just the sizes.

Both boards independently confirmed that the USB serial-number descriptor
equals the eFuse MAC (`E0:72:A1:FB:9C:5C`, `d4:f9:8d:66:13:64`). That is what
`profile_key()` falls back to when a probe cannot reach the chip — recorded as
`inferred`, never `probed`, because two-for-two is a strong hint and not a
measurement.

**The CH340 board closed the bridge path**, which had never met hardware —
both other boards are native USB. It probed clean on the first attempt: no
BOOT+RESET, no port churn, no vanishing. Every problem that dominated the S2
work came from the SoC *being* the USB device; here the CH340 is, and it stays
enumerated regardless of what the SoC does.

It also turned out to be an **ESP8266, not an ESP32**, which tested more than
intended. `ESP8266EX` normalises to `esp8266ex`, matches no ESP-IDF target, and
`idf_target` is emitted as `None` with provenance `unverified` and a note naming
what it normalised to. It did not invent `esp8266` — and did not fall back to
`esp32`, which a prefix matcher written slightly differently would have, since
both begin with `esp`. Its `usb_product` is `USB2.0-Serial`: the *bridge chip's*
generic string, naming neither the board nor the SoC, unlike the S2's
`QT Py ESP32-S2`.

> **Scope: ESP8266 support stops at identification, backup and restore.** There
> is no IDF target, no ESP32-style partition table, and no app descriptor, so
> those sections are empty rather than wrong — the pipeline degrades stage by
> stage. Building for one needs ESP8266_RTOS_SDK, not ESP-IDF, so there is no
> `idf.py flash` path here and none is claimed. Treat it as a probe, back-up
> and restore target, not a build target.

`restore` was exercised and verified **byte-exact**: 4,194,304 bytes read back
after the write hashed identically to the backup.

Across three restores the picture is consistent, and the S2's delta is a
*reproducible signature* rather than an explanation:

| Board | Result |
|---|---|
| ESP8266 | bit-identical — its firmware writes nothing to flash on boot |
| ESP32-S2, restore #1 | 50 bytes, all in `nvs` |
| ESP32-S2, restore #2 | **50 bytes, all in `nvs`** — same count, same region |

A **second** QT Py — a different board of the same model — then showed **49**,
which is the useful result:

| Board | Restore | `nvs` delta | Every other region |
|---|---|---|---|
| ESP8266 | #1 | 0 | identical |
| QT Py A | #1 | 50 | identical |
| QT Py A | #2 | 50 | identical |
| QT Py B | #1 | **49** | identical |

The same board reproduces exactly; a different board of the same model does
not. So the count is **device-specific** — MAC-derived values, a differing key
length — rather than a fixed firmware signature. An earlier version of this
file said tinyuf2 "initialises the same NVS keys on every boot", implying an
identical count. That was built on one board and the second narrowed it.

What holds across all four restores is the claim that matters: **`restore` is
exact.** Bootloader, partition table, both app slots, `uf2` and `ffat` are
byte-perfect every time, and every byte that moves is inside the one partition
designed to be written. A whole-image SHA-256 reports only "mismatch" and
leaves you guessing; the region breakdown separates *restore failed* from
*firmware booted and did its job* — which is why the profile carries the
partition layout and not just the sizes.

**Console over USB on the S2 took a third approach.** Parts with a
USB-Serial/JTAG controller (S3/C3/C6/C5/H2/P4) get a secondary console for free
and `templates/idf-base` is readable over USB unchanged. The S2 has only
USB-OTG, and two approaches failed on hardware — the stock template, and
`CONFIG_ESP_CONSOLE_USB_CDC=y` — each flashed and hash-verified, each producing
a board that ran correctly and presented **no USB device at all**.

`templates/idf-usb-console` solves it with `espressif/esp_tinyusb`, which drives
the OTG peripheral itself. Verified on the QT Py: enumerated as `303a:4001` and
printed its self-report over CDC.

It is a **sibling** of `idf-base`, deliberately not a change to it. `idf-base`
drives no peripheral at all, which is what makes it safe on a board whose pin
map is unverified; a USB device stack is a peripheral. Keeping them separate
preserves that guarantee for every other board.

### Not validated

- **The EUI64 MAC path.** On C6/C5/H2, esptool prints three MAC lines and the
  first is an 8-byte EUI64; an early parser truncated it into a wrong 6-byte
  identity, and identity is the backup key. Neither board has an EUI64. Closing
  this needs a C6, C5, or H2.
- **Boards behind a UART bridge** (CH340/CP210x/FTDI). Both boards here are
  native USB; the bridge path in `identify_usb()` is untested against hardware.

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
