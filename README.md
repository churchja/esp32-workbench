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
| `tools/test_*.py` | 372 assertions across 8 files; no hardware needed |
| `smoke.sh` | One command: readiness + unit + schema + real ESP-IDF and PlatformIO builds |
| `boards/` | One profile per physical board, keyed by eFuse MAC. **The asset — commit these** |
| `templates/idf-base/` | **The default.** ESP-IDF starter that drives no peripheral at all |
| `templates/idf-usb-console/` | Sibling for **USB-OTG parts (S2)**, where a console needs TinyUSB |
| `templates/pio-base/` | PlatformIO/Arduino sibling, for the Arduino display-library ecosystem |
| `backups/` | Flash images (gitignored) + manifests (tracked) |
| `docs/` | Design rationale, recommendations, and the build log |
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

| Board | Chip | `set-target` | Flash | PSRAM | USB interface | Parts | Max baud | Profile |
|---|---|---|---|---|---|---|---|---|
| ESP32-S3 devkit | `ESP32-S3` | `esp32s3` | 8MB | 8MB (AP_3v3) | `USB-Serial/JTAG` | 4 | **230400** | `e072a1fb9c5c` |
| M5 Stamp S3 | `ESP32-S3` | `esp32s3` | 8MB | none | `USB-Serial/JTAG` | 6 | **230400** | `3cdc75edd7b0` |
| Cardputer ADV (Stamp **S3A**) | `ESP32-S3` | `esp32s3` | 8MB | none | `USB-Serial/JTAG` | 4 | **230400** | `aca704007f60` |
| LilyGo T-Display S3 AMOLED | `ESP32-S3` | `esp32s3` | **16MB** | 8MB (AP_3v3) | `USB-Serial/JTAG` | 6 | **230400** | `e4b0638aec2c` |
| Adafruit QT Py ESP32-S2 **A** | `ESP32-S2FNR2` | `esp32s2` | 4MB | 2MB | `USB-OTG` | 6 | **460800** | `d4f98d661364` |
| Adafruit QT Py ESP32-S2 **B** | `ESP32-S2FNR2` | `esp32s2` | 4MB | 2MB | `USB-OTG` | 6 | **460800** | `d4f98d66124a` |
| FutureProofHomes Satellite1 CORE rev4.1 | `ESP32-S3` | `esp32s3` | 16MB | 8MB | `USB-Serial/JTAG` | 5 | **230400** | `d83bda407850` |
| M5Stack Capsule v1.1 | `ESP32-S3` | `esp32s3` | 8MB | none | `USB-Serial/JTAG` | 6 | **230400** | `aca7040355d0` |
| Flipper Zero Wi-Fi Module v1 | `ESP32-S2` | `esp32s2` | 4MB | none | `USB-OTG` | 5 | **460800** | `3030f9837418` |
| CH340 board (ESP8266) | `ESP8266EX` | `**none**` | 4MB | none | `CH340 bridge` | 0 | **230400** | `bcddc2246e97` |

Backup and hash-verify: **all ten**. Restore: verified on the S3 devkit, both
QT Pys, and the ESP8266, and performed on the LilyGo after its variant scan; the
M5 Stamp, the Cardputer ADV and the Flipper module have not been written to.
Console over USB: free on every S3, needs `templates/idf-usb-console` on all
three S2s, and is UART by nature on the CH340 board.

**The two QT Py rows are identical in every column but the profile id.** Same
product string, VID/PID, chip, partition layout, factory images and build dates.
Only the eFuse MAC separates them — which is the collision the MAC-keyed design
exists to prevent, and it went untested until a second QT Py appeared, because
every earlier board was a different model. They landed on separate profiles, so
board A kept its backup manifest and verified read ceiling instead of being
silently overwritten.

**The four S3 rows carry two further lessons**, and they are different in kind.

*Silicon can differ under one `set-target`.* The devkit and the LilyGo have 8MB
PSRAM; neither Stamp nor the Capsule has any. The eFuse says so in words —
`Embedded PSRAM 8MB` on three boards, `Embedded Flash 8MB` on the three M5Stack
ones — and it **names the flash vendor**, agreeing with the JEDEC id read
separately: `(XMC)` against `0x20` on the Capsule, `(GD)` against `0xc8` on the
Stamp S3. Two independent readings of one fact from different parts of the chip. The LilyGo also carries **16MB** of flash, double
every other board here. `set-target` is `esp32s3` for all four, so a build
config valid on one can mismanage memory *or overrun flash* on another.
`chip_features` and `flash_size` are read from the silicon rather than inferred
from a part number, which is what catches it. Even the flash vendor differs —
GigaDevice on the Stamp S3, XMC on the Stamp S3A — both described as "8MB
embedded".

The LilyGo is the clearest case of PSRAM being a *consequence* of the board's
job rather than a spec-sheet upgrade: an AMOLED framebuffer does not fit in
internal SRAM, so the panel forces the memory. Reading `chip_features` predicts
what a board is built to do.

*Partition layout is a **firmware** decision recorded in hardware.* The Stamp S3
carries M5Stack's stock dual 3.19MB OTA slots plus 1.5MB SPIFFS. The Stamp S3A,
running Bruce, has a **single 4.875MB app** and **3MB SPIFFS** — it spent the OTA
partner slot to buy a larger image and filesystem. Coherent for a tool storing
captures and scripts, but it means no A/B rollback, so reflashing must go
through the ROM bootloader. Reading the layout tells you what a board is *for*,
not merely what it is.

The **M5Stack Capsule** turned that comparison into a controlled one. Three
M5Stack boards, and the layouts split against the hardware rather than with it:

| Board | Flash vendor | `partition_table_md5` |
|---|---|---|
| M5 Stamp S3 | GigaDevice `0xc8` | `467eb896e29d…` |
| **Capsule** | **XMC `0x20`** | **`467eb896e29d…`** — identical |
| Cardputer ADV | XMC `0x20` | `d179fad57629…` — different |

Different silicon, same table; same silicon, different table. The variable that
tracks is **which firmware shipped**, and nothing else. Two products sharing a
vendor's stock layout collide exactly; one that was reflashed with Bruce
diverges. This is the sharpest available demonstration that a partition table is
not a hardware fingerprint — a lesson that cost real effort to learn on the
LilyGo, where the table was read as identifying and was not.

Three mechanisms had to cooperate for the QT Py pair, each verified in passing:
the USB-serial fallback keyed B's first (failed) probe correctly and its derived
MAC later matched eFuse exactly; `adopt_orphans` did **not** fire, matching only
on exact normalized-serial equality — two boards differing in the last three
octets are precisely what a looser rule would have collapsed; and `mac` upgraded
`inferred` → `probed` in place on B's successful probe.

Note the baud row. Across **seven** boards the ceiling tracks the **USB
interface**, not the individual board:

| Interface | Boards | Max read baud |
|---|---|---|
| USB-OTG | **3** × ESP32-S2 | **460800** |
| USB-Serial/JTAG | **6** × ESP32-S3 | 230400 |
| CH340 UART bridge | ESP8266 | 230400 |

Every board within an interface agrees exactly, with **zero disagreement inside
any group**. USB-Serial/JTAG now has four boards from four vendors — an
Espressif devkit, an M5 Stamp S3, a Stamp S3A and a LilyGo T-Display S3 AMOLED —
different MACs, different flash vendors, different flash *sizes* (8MB vs 16MB),
different partition layouts, all capping at 230400 and all failing reproducibly
at 460800.

An earlier version of this file claimed the ceiling was per-board, arguing that
"the two native boards disagree with each other". That conflated USB-OTG with
USB-Serial/JTAG — different peripherals — and was written from three data
points. The fourth broke it; split by interface, all agreed. Boards **five,
six, seven and eight** each tested the corrected claim rather than producing it,
and it held every time.

The ladder is still the right design, but for the plainer reason: **no constant
works.** 460800 makes S3 and ESP8266 backups *fail*; 115200 makes all three S2s
four times slower than necessary. `esp32flash.py` negotiates rather than assumes.

`n` is still small — ten boards, three interfaces, one of which (the bridge)
still has a single example. Treat the table as a measured pattern, not a law.
The pattern's strength is that boards five through ten *tested* it rather than
producing it; its weakness is that every board in a group shares one SoC family,
so what is measured is a *peripheral*, not an interface in the abstract.

The Flipper module improved that in one specific way. The USB-OTG group was two
**same-model twins** — two Adafruit QT Pys, identical in every column but the
profile id — which is weak evidence for anything beyond that model. The Flipper
module is a different vendor, a different flash manufacturer (`0x5e`, new to
this repo), a different partition layout, no PSRAM, and an Arduino build a
year older, and it landed on the same rung at nearly the same speed. That is a
genuinely independent specimen where the group previously had one.

The USB-Serial/JTAG group has no such luck: all four are ESP32-S3, so the claim
there is still "the S3's USB-Serial/JTAG peripheral caps at 230400". A C3, C6 or
C5 would be its first real test.

Timing corroborates the mechanism. Against the 10-bits-per-byte serial framing
model, the S3 (USB-Serial/JTAG) measured 731s vs 728 predicted, the ESP8266
(real UART bridge) 202s vs 182, and the LilyGo 756.5s vs 728 — so the model
does not drift with image size. Only the S2's **USB-OTG** ran far faster than
predicted (33s vs 91), because OTG CDC does not throttle to the nominal baud.
The model stays a safe *upper* bound, which is all the derived timeout needs.

**Two of those four figures are upper bounds, not measurements** — and they are
the last ones recorded that way. Backups timed the *whole* baud ladder and
stored that as `read_seconds`, so a discarded 460800 attempt was charged to the
rung that actually produced the image:

| figure | rung | clean? |
|---|---|---|
| S3 731s vs 728 | pinned 115200, no ladder | **clean** |
| S2 33s vs 91 | 460800, first rung took it | **clean** |
| ESP8266 202s vs 182 | fell back from 460800 | inflated by an unrecorded amount |
| LilyGo 756.5s vs 728 | fell back from 460800 | inflated by an unrecorded amount |

So the ESP8266's "11% over model" and the LilyGo's "3.9%" are **ceilings on the
error, not the error**. Seven of the ten boards fall back — the six
USB-Serial/JTAG boards and the CH340 bridge; the three USB-OTG S2s never do —
so any laddered duration in a *pre-change* manifest carries the same inflation.

Two boards have now been backed up *after* the fix, and between them they show
both shapes. The Flipper module took the first rung, so its figures are equal:
`read_seconds` 33.2, `ladder_seconds` 33.2, one `attempts` entry marked `ok`.
The Satellite1 fell back, and its figures diverge:

| key | value |
|---|---|
| `read_seconds` | **750.5** — the 230400 rung alone |
| `ladder_seconds` | **755.2** — wall time the operator waited |
| `attempts` | `{460800, 4.7s, ok:false}`, `{230400, 750.5s, ok:true}` |

Exactly the discarded attempt apart, with the rungs summing to the ladder total
— the invariant `test_flash.py` asserts, now observed on hardware rather than
against a scripted clock.

It also produced a number that did not previously exist: **the cost of a failed
rung**. Two have now been measured, and the result is that it is not a constant:

| Board | Discarded 460800 attempt |
|---|---|
| Satellite1 CORE | 4.7s |
| M5Stack Capsule | 7.4s |
| LilyGo T-Display | **9.5s** |

Twice the spread between the extremes. The S3 devkit's profile has claimed *"reproducible in <6s"* since the
first day, hand-measured on that one board and stated as though it described the
failure mode; the Capsule exceeds it. That note now carries a caveat rather than
being deleted, because the bound was true where it was taken.

Predicting the Capsule's fallback at "about 4.7s" before the read was the same
error in miniature — one measurement promoted to a constant, committed within
the hour of writing that pattern down. The timer records the cost per attempt
precisely so it does not have to be assumed.

**The LilyGo settled the question of what the old field measured**, because it is
the only board read both before and after the fix:

| Run | `read_seconds` | `ladder_seconds` | discard |
|---|---|---|---|
| #1, pre-fix | **756.5** (whole-ladder) | not recorded | not recorded |
| #2, post-fix | 747.3 (clean) | **756.8** | 9.5s |

The old field and the new `ladder_seconds` agree to **0.3 seconds** on the same
board doing the same operation. That is direct evidence the old `read_seconds`
really was whole-ladder time and that `ladder_seconds` reproduces it exactly,
rather than an argument from reading the code. The clean read is 747.3s — 2.6%
over model, not the 3.9% the inflated figure implied.

And 750.5s is the first *clean* 16MB timing here — 3.1% over the model, against
the LilyGo's whole-ladder 3.9% for the same size and rate. The two boards are
within about a second of each other once the discard is removed, which is what
the caveat on those older figures predicted.

`read_with_fallback` now times each rung separately and returns them:

| manifest key | meaning |
|---|---|
| `read_seconds` | the successful rung alone — what the transfer actually cost |
| `ladder_seconds` | wall time the operator waited, discarded attempts included |
| `attempts` | `{baud, seconds, ok}` per rung, in order |

`ladder_seconds` is exactly `sum(a.seconds for a in attempts)`. `test_flash.py`
asserts that on `read_with_fallback`'s return value **and** on the manifest
`cmd_backup` actually writes. The second is the one that matters: reinstating
the old whole-ladder value passed every assertion the first had, because
nothing exercised the manifest write — which is where the bug lived.

**The eight entries written before this change carry no `attempts` key**; for
those, read `read_seconds` as whole-ladder time. They were left as recorded
rather than retro-fitted, since the split cannot be recovered after the fact.
(Eight entries across seven files — the S3 devkit has two, and its first
predates the ladder entirely.)

The per-rung records also turn "the ladder fails fast" — until now a claim in a
profile note — into data the next backup collects on its own.

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

| Board | Restore | Image | `nvs` delta | Every other region |
|---|---|---|---|---|
| ESP8266 | #1 | 4MB | 0 | identical |
| QT Py A | #1 | 4MB | 50 | identical |
| QT Py A | #2 | 4MB | 50 | identical |
| QT Py B | #1 | 4MB | **49** | identical |
| **LilyGo** | #1 | **16MB** | **0** | identical |

The same board reproduces exactly; a different board of the same model does
not. So the count is **device-specific** — MAC-derived values, a differing key
length — rather than a fixed firmware signature. An earlier version of this
file said tinyuf2 "initialises the same NVS keys on every boot", implying an
identical count. That was built on one board and the second narrowed it.

**The LilyGo is the strongest row.** It is the first 16MB verification — double
the previous largest image — and the first verified after *different* firmware
ran on the board: `projects/i2c-variant-scan` was flashed onto it, ran, and was
then restored. **Zero differing bytes across all 16,777,216**, every region, with
the read-back sha256 equal to the backup's.

A caveat was written down *before* that measurement: the verification came hours
and several power cycles after the restore, so a non-zero `nvs` delta could not
have been attributed to the restore rather than to the firmware's own boots. The
zero result makes it moot — nothing wrote to flash at all, so there was nothing
for the extra boots to confound. It is recorded in the profile anyway, because it
was the right caveat for the result it was written against.

The comparison itself was validated before being trusted, against two controls:
the backup against itself (every region zero) and against an unrelated 16MB image
(differences everywhere, with the per-region totals **reconciling exactly** with
the whole-image count). That reconciliation is the load-bearing check — it proves
the regions leave no gap where a difference could hide and be silently
under-reported.

What holds across all five restores is the claim that matters: **`restore` is
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

### The first upgrade the gate was actually built for

Every flash before this one was a test: write something, prove it ran, restore.
The Satellite1 was the first time the promise was cashed in for real work —
a firmware upgrade the operator wanted to **keep**. It went from Satellite1
`v0.0.5-beta.1` (ESPHome 2024.11.2, IDF 4.4.8, Dec 2024) to `v0.2.0` (ESPHome
2026.4.5, IDF 5.5.4, May 2026), about thirteen releases.

The interesting part is what had to be checked first, because a vendor factory
image is not a restore — it carries its **own partition table**, and writing one
that disagrees with the board's silently strands whatever lived in the old
layout.

Three checks, in order, all before anything was written:

1. **Parse the image's partition table and compare it entry-by-entry** against
   the probed one. Identical offsets throughout; `nvs` differed only in size,
   `0x6d000` → `0x70000` at the same `0xf90000` start — growing to fill the
   flash, which does not invalidate existing entries.
2. **Confirm the image's span.** 3,334,320 bytes written at `0x0` reaches
   `0x32e0f0`. `nvs` at `0xf90000` is nowhere near it, so Wi-Fi credentials and
   entity states were never at risk.
3. **Confirm a verified backup existed** — which is the gate's job, and it said
   so before allowing the write.

Afterwards the board was re-probed rather than trusted: `app0` reads project
`satellite1`, version `2026.4.5`, IDF `5.5.4`, built `May 28 2026 21:52:27`, and
the flashed table matches the image byte-for-byte.

**A check that looked like a test and was not.** The first attempt at step 1
hashed the image's table with a hand-rolled md5 over the `0xff`-stripped region
and compared it against the profile's `partition_table_md5`. Those are different
computations over different byte ranges — the profile records the md5 record
**ESP-IDF embeds in the table itself** (magic `EBEB`, covering only the bytes of
real entries). The two numbers were never comparable, and confirming it after
the fact showed the hand-rolled method returns `cc9a39b7…` where the tool
reports `67654859…` for the very same bytes. The conclusion survived only
because step 1 was then done properly by parsing. It belongs to the pattern
catalogued in [the build log](docs/session-2026-09-04.md#1-checks-that-report-success-for-the-wrong-reason)
— a check that cannot distinguish *"I compared them"* from *"I compared the
wrong things."*

**The validator caught the other half.** `board.firmware_running` was written
with provenance `verified` and rejected:

```
ERROR d83bda407850.yaml: board.firmware_running claims 'verified'
  but no verification_log entry tests it
```

Top provenance requires its method on the record. The rule fired on the person
writing the profile, which is the only place it could usefully fire.

**The one thing a backup here does not cover.** The Satellite1's XMOS XU316 has
its **own SPI flash**, written by the ESP32 over SPI — `esptool` cannot see it,
so the 16MB image does not contain it. It is not unrecoverable: the ESPHome
build embeds an XMOS image and reflashes it on boot, so the co-processor follows
whatever ESP32 firmware is running. But it means the guarantee is narrower than
on the other eight boards. Restoring the pre-update image would put back
`v0.0.5-beta.1`, which would then reflash the XMOS to *its* embedded version —
recovery by a different route than "restore the image", and worth knowing before
relying on one.

### What identification cannot reach

Six boards were identified down to flash vendor and partition layout. The
seventh stops sooner, and the reason is worth recording because it is a limit
of the method, not a gap in the effort.

**"LilyGo T-Display S3 AMOLED" is a family, not a board.** At least six ship
under names that close to each other:

| Variant | Panel | Controller |
|---|---|---|
| T-Display-AMOLED-Lite 1.47" | 194×368 | SH8501 |
| T-Display-S3 AMOLED 1.91" | 240×536 | RM67162 (QSPI) |
| T-Display-S3 AMOLED **Plus** 1.91" | 240×536 | RM67162 (single SPI) |
| T4-S3 2.41" | 450×600 | RM690B0 |
| T-Display-S3-AMOLED-1.64 | 280×456 | — |
| T-Display-S3-AMOLED 1.43/1.75" | 466×466 round | CO5300 or SH8601 |

Every one is ESP32-S3R8 with 16MB flash and 8MB OPI PSRAM. **The eFuse reading
that separated the other six boards separates nothing here.**

> **Resolved on hardware.** An I2C scan pinned it: **T-Display-S3 AMOLED Plus
> 1.91"**, RM67162 over *single SPI*. Read the rest of this section anyway — the
> interesting part is what the answer did to the guesses that preceded it.

Nor does anything else the probe collects, and this generalises:

| Field | Why it does not identify a board |
|---|---|
| `partition_table_md5` | byte-for-byte espressif's `arduino-esp32` `default_16MB.csv` |
| USB `303a:1001` | the generic Espressif native USB-Serial/JTAG ID |
| app `arduino-lib-builder / v4.4.7` | the standard arduino-esp32 core build stamp |

**Those three identify a *build*, not a board.** On the other six they happened
to correlate with one, which is a different thing and easy to mistake for the
same thing.

The **Capsule proved it without being asked to.** Its app descriptor is
byte-identical to the LilyGo's — same project, same IDF version, same build date
**and the same build second**:

```
arduino-lib-builder | esp-idf: v4.4.7 38eeba213a | Mar  5 2024 12:12:53
```

Two unrelated vendors, an M5Stack device and a LilyGO one, shipping the same
`arduino-lib-builder` artifact. Their ELF hashes differ (`a2d0f822…` vs
`61d714e4…`), so the *applications* are not the same — only the stamp collides.
Which is the whole point: a field that two different products can share to the
second is not an identifier, however specific it looks.

Five source lanes were searched — LilyGO's repos, the ESP Component Registry,
`esp-bsp`, panel drivers, and the schematics. Results, recorded in the profile
with source URLs:

- **No BSP exists** for any variant, so the research queue's own "check for a
  BSP first" shortcut is closed here. A panel driver must be written or a
  third-party one adopted.
- The circumstantial case for the 1.91" non-touch board is real but weak: its
  board JSON matches flash size, flash mode, memory type, USB hwids and
  partition file. The same file would match several siblings, so it is recorded
  as `community` provenance and labelled circumstantial, not as an answer.

**`pinmap` is deliberately still `{}`.** Two LilyGO-owned sources assign
different *functions* to the same GPIOs on boards both label 1.91":

| GPIO | One build | Another build |
|---|---|---|
| **38** | `PIN_LED` — green LED, confirmed in the schematic netlist | **`OL_EN` — AMOLED panel power enable** |
| **21** | button with a 10K pull-up | touch controller INT |
| **4** | battery sense ADC (1.91") | **SD-card MISO (T4-S3)** |

Writing either set down would be a coin flip on a live rail. The QSPI pins two
repos *do* agree on are kept as a note-level lead, not a pinmap, because a third
vendor file disagrees.

A second pass corroborated each finding against a *different* source than the
one it came from — 24 checks, 21 agreeing at low risk. It sharpened the picture
in one direction only: **more variant-dependent, not less.**

The one contradiction is instructive. The first pass reported the battery
divider ratio as "not established"; the second extracted the vendor schematic's
Altium netlist and found it exactly — `BAT → R15 10K → BAT_VOLT → R16 10K → GND`,
ratio **2.0** on GPIO4, independently confirmed by the driver's
`analogReadMilliVolts(adcPins) * 2`. A value, from a vendor source, corroborated
twice. It is still not in the profile as a value, because of what came with it:

| Variant | Battery sense |
|---|---|
| 1.91" QSPI | `adcPins = 4`, no PMU — **ratio 2.0 applies** |
| 1.91" SPI / Plus | `adcPins = 4` but a PMU shadows it; reads BQ25896 over I2C. The ADC path is dead code |
| 1.47" Lite | `adcPins = -1`, AXP2101 — **no divider exists** |
| 2.41" T4-S3 | `adcPins = -1`, SY6970 — no divider, and **GPIO4 is SD-card MISO** |

A hardcoded `analogRead(4) * 2` is correct on one of four boards, reads a card
data line on another, and on the remaining two returns a plausible-looking wrong
number instead of failing. That is worse than having no value at all — which is
the argument for `status: blocked` over a best guess, stated as evidence rather
than as caution.

Worth separating, because it changes what is actually risky: the display **bus**
parameters — clock, command/address bits, SPI mode — cannot damage anything if
wrong. Worst case is a blank or garbled panel. The hazard is entirely in the
*pins* that travel alongside them.

One source turned out to disagree with the hardware outright: the non-touch
schematic shows a **W25Q32 (4MB)** where this unit probed **W25Q128 (16MB)**.
A published vendor schematic is one source, not ground truth — recorded as a
`schematic_trust` queue entry so the next reader does not take it on faith.

Four of the five queue entries are `blocked` on resolving the variant — and the
research turned up a **programmatic** way to do that, which is better than the
one I first wrote down. LilyGO's own library autodetects the board by I2C probe
in `LilyGo_AMOLED::begin()`:

| Probe | Result |
|---|---|
| `Wire.begin(1,2)` → AXP2101 present | 1.47" Lite |
| `Wire.begin(3,2)` → CST816 present | a 1.91" **touch** board… |
| …then `0x51` (PCF85063 RTC) present / absent | …**Plus** (SPI) / **Touch** (QSPI) |
| `Wire.begin(6,7)` → SY6970 present | 2.41" T4-S3 |
| none of the above | 1.91" **non-touch** (QSPI) |

Non-destructive, vendor-sanctioned — their library runs it on every boot across
every family member — and it costs one flash, which the gate permitted because
this board has a verified backup.

`projects/i2c-variant-scan` reproduces that sequence and reports only. It ran:

```
pair A  SDA=1  SCL=2   -> no devices
pair B  SDA=3  SCL=2   -> 0x15 0x51 0x6b

VARIANT : T-Display-S3 AMOLED Plus 1.91in (RM67162, single SPI)
BASIS   : CST816 (0x15) AND PCF85063 RTC (0x51) on SDA=3/SCL=2
```

`0x15` CST816 says a 1.91" touch board; `0x51` PCF85063 RTC says **Plus**, not
Touch; `0x6b` BQ25896 is the `BOARD_AMOLED_191_SPI` PMU. Three independent
confirmations on one bus. Pair C was never driven — the scan exits at the first
hit, as LilyGO's own autodetect does, so the display-bus pins were never touched.

### What the answer did to the guesses

This is the part worth keeping. **Every one of the three GPIO conflicts resolved
to the side that would have been wrong**, and the circumstantial evidence pointed
at the wrong board:

| Recorded before the scan | After |
|---|---|
| "consistent with the 1.91" **non-touch** board" — its board JSON matched flash size, mode, memory type, hwids *and* partition file | **Wrong.** It matched a *sibling* |
| GPIO38 — `PIN_LED` or `OL_EN`? | **`OL_EN`** — panel power enable |
| GPIO21 — button or touch INT? | **touch INT** |
| GPIO4 — battery ADC, divider 2.0? | **dead code**, shadowed by the BQ25896; battery is read over I2C |
| QSPI pins two repos agreed on | **inapplicable** — the Plus is single SPI, `d2 = d3 = -1` |

Four board-JSON fields matched and the conclusion was still wrong, because those
fields are shared across a product line. **Four matching fields is not
identification.** The corroboration pass had predicted exactly this — *"the same
board file would match several siblings"* — which is why it was filed as
circumstantial and never written into a pinmap.

Had the "obvious" values from the older non-touch repo been recorded, driving
GPIO38 as an indicator LED would have gated the display rail, and
`analogRead(4) * 2` would have returned a plausible wrong battery voltage rather
than failing. The entry is kept in the profile as `value: WRONG` — a record of a
failed inference, since the inference was reasonable and still wrong.

### Not validated

- **The EUI64 MAC path.** On C6/C5/H2, esptool prints three MAC lines and the
  first is an 8-byte EUI64; an early parser truncated it into a wrong 6-byte
  identity, and identity is the backup key. None of the ten boards here is a
  C6, C5 or H2, so none prints one. Closing this needs one of those parts — it
  is the only path in `esp32ident.py` that has never met the hardware it exists
  for at all.
- **Bridge chips other than CH340.** The bridge path itself is validated — the
  CH340 board closed it, and is described above. But `esp32ident.py` also
  claims **CP210x** (`10c4:ea60/ea70/ea71`) and **FTDI**
  (`0403:6001/6010/6015`), and neither has met hardware. They share the code
  path CH340 exercised, so this is a narrower gap than it looks: what is
  untested is the VID/PID table entries, not the logic around them.

  An earlier version of this file listed the whole bridge path here as untested
  while the CH340 section three screens up said it was closed. Two prose claims
  about the same fact, drifting apart because nothing checks prose.

## Is it working?

```bash
./smoke.sh
```

Exit 0 all green · 1 degraded (warnings, core paths fine) · 2 something failed.

It runs five layers, which prove progressively more:

| # | Layer | Proves | Needs a board? |
|---|---|---|---|
| 1 | `tools/doctor.py` | esptool present and which dialect, platform installed, gate wired, `backups/` writable | no |
| 2 | `tools/test_*.py` | pure logic: binary parsers, banner parsing, gate decision, provenance, per-rung timing | no |
| 3 | `validate_profiles.py` | every stored profile satisfies the schema | no |
| 4 | **real ESP-IDF** | `idf-base` builds for `esp32c6`; the gate fires on `idf.py flash` and correctly ignores `--dry-run` | no |
| 5 | real PlatformIO | template compiles; the pre-upload hook actually fires | no |

Layer 4 is the one that matters most and was missing from this table until an
audit caught it: it is the only layer that exercises the **primary** framework
and the gate mechanism most flashing actually goes through.

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
