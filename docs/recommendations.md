# Recommendations: Claude Code + ESP32

Opinionated, and specific to this setup (macOS, PlatformIO 6.1.19 + its MCP
server, esptool 5.2.0, no ESP-IDF, no arduino-cli).

---

## 1. Let the board be unknown — but never let a *pin* be unknown

The instinct on plugging in an unlabelled board is to identify the model first.
Wrong order. Model names are marketing; several vendors ship visibly different
boards under one name, and clone sellers copy names outright.

Identify the **chip** from silicon, the **peripherals** from the factory
firmware, and treat the model name as a search hint rather than a fact. The
model is how you find the schematic; it is not itself evidence.

The corollary is the hard rule: a GPIO number is either sourced or it is
`unverified`, and `unverified` pins do not get driven. Everything else here is
convenience. This one prevents damage.

## 2. Open every session read-only

Structure the first prompt so it cannot destroy anything:

> A board just came up on USB. Identify it — read only, change nothing.

The video's Mike did this instinctively and it is why three unknown boards
survived. Two independent guards back it here: `esp32ident.py` has no write
path, and `esp32flash.py` refuses writes without a verified backup. Belt and
braces, because the failure is unrecoverable.

## 3. Photograph the board. Constantly.

The single highest-leverage habit, and the least obvious.

Claude cannot see the panel, the silkscreen, the LED, or which way you are
holding it — and an enormous share of embedded faults live exactly there. In
the source video, two separate bugs were diagnosed from phone photos: a 34-pixel
row offset, and text rendering vertically because the board was being held with
the cable to the right. Neither is visible in code.

Photograph: the silkscreen before you start (part numbers settle arguments no
amount of probing will), the screen whenever output looks wrong, and the board
in your hand when orientation matters.

## 4. Prefer the PlatformIO MCP server, but do not depend on it

You have 27 typed tools — `build_project`, `upload_firmware`, `start_monitor`,
`list_devices`, `get_board_info`, `query_logs`. Structured results beat parsing
build logs, and `query_logs` beats scraping a monitor.

But the tools here shell out to `pio` and `esptool` so the same skill works on
the Hermes NUC or a fresh laptop with nothing configured. Check which path is
available once at the start of a session, commit to it, and do not alternate —
a monitor opened by one path will hold the port against the other.

## 5. Ask for three ideas before building one

The video's best prompt asked for three project proposals *grounded in the
hardware actually discovered*, before any code. Two things come out of it: you
choose, and the proposals themselves reveal what Claude believes the board can
do — so a wrong belief surfaces before it becomes firmware.

The strongest version adds a constraint that forces engagement with real
limits: *"no Wi-Fi"* is what produced the USB-fed sea-temperature display, and
noticing that the C6 cannot carry Bluetooth audio is what produced the spectrum
analyser on a different board.

## 6. Buy boards that differ, not boards that are cheap

You already have Adafruit, Seeed/XIAO, S3 and C5. The gap worth filling is
**capability**, not count. A second S3 devkit teaches nothing a first one did
not. Genuinely different axes:

- **Native USB vs bridge** — changes debugging entirely (USB-JTAG, CDC console)
- **PSRAM vs none** — the hard ceiling on framebuffers and audio buffers
- **Display technology** — LCD (fast, needs power) vs e-paper (holds an image
  at zero power) are different design problems, not different screens
- **Radio** — BLE-only (C3, C6) vs Bluetooth Classic with audio (original
  ESP32). A2DP audio needs Classic; no amount of code fixes a C6 here
- **802.15.4** (C6, H2) — Thread and Matter, if Home Assistant matters to you

For the Adafruit boards specifically: their Learn guides are among the best
primary sources in the ecosystem, so those are the boards where research is
cheapest. Start unfamiliar techniques there, then port to the clones.

## 7. Ideas worth building, in rough order of value

**A physical status display for Claude Code itself.** A `Stop` hook writes
session state to a serial port; a small display shows which agent is running,
what it is waiting on, and whether the last run passed. This is the one that
justifies the desk space — you already run long agent sessions, and glancing at
a display beats alt-tabbing.

**A capability harness.** Firmware that exercises every peripheral the profile
claims and reports pass/fail over serial. It converts `unverified` fields into
`verified` ones systematically instead of one bug at a time.

**A profile-to-firmware generator.** Given a completed profile, emit a
`platformio.ini` plus a display-init block with the right controller, offsets,
and pins. The profile already holds everything needed; this closes the loop from
research to running code.

**A regression pinner.** After a board works, store the exact
platform/framework/library versions in its profile. Toolchain drift silently
breaks working boards, and without a record you cannot tell whether your code or
the platform changed.

## 8. Configuration worth doing once

**Do not run `--dangerously-skip-permissions` for flash work.** It was
reasonable in a demo. `erase-flash` is unrecoverable, and the whole value of a
permission prompt is that it fires on exactly that class of action. The gate in
`esp32flash.py` holds regardless — but do not rely on one layer.

**Add an allowlist instead**, so the safe operations stop prompting and the
dangerous ones still do: permit `pio run`, `pio device list`, `esptool flash-id`,
`esptool read-flash`, and the `tools/*.py` scripts; leave `write-flash` and
`erase-flash` prompting.

**Commit the `boards/` directory.** The profiles are the accumulated asset — the
firmware is regenerable, the research is not. Their diffs are also the audit
trail for when a spec turns out to be wrong.

**Keep backups out of git.** They are megabytes of binary per board. `.gitignore`
excludes the images but keeps `manifest.json`, so the record of what was backed
up survives even when the images live only on disk.

## 8a. ESP-IDF is now the primary framework

Superseding much of what follows: the workbench standardizes on **ESP-IDF
v6.0.3** (pinned in `.idf-version`), which supports every chip you own
natively. The PlatformIO/Arduino material below still applies to
`templates/pio-base/`, which is retained for Arduino-ecosystem work — but it is
no longer the default.

## 8b. Install the pioarduino platform before you touch C5 or C6

You own C5 and C6 hardware, so you will hit this immediately. Verified against
the live registry on 2026-09-04:

The official `platformio/espressif32` platform — your installed 6.13.0 and the
current 7.1.0 alike — pins **Arduino core 2.0.17**, which predates the C5, C6,
H2, and P4. Their board definitions declare `frameworks: ["espidf"]` only, so
`framework = arduino` fails in under half a second. Upgrading the official
platform does not fix it; 7.1.0 pins the same core.

The community **pioarduino** fork does support them — arduino-esp32 master with
ESP-IDF 5.5.5, 299 boards including `adafruit_feather_esp32c6` and
`esp32-c5-devkitc-1`, all declaring `["arduino", "espidf"]`. Pin it per-env:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
```

Pin the exact release rather than tracking `develop` — this fork moves fast, and
an unpinned platform means a board that worked last month may not build today.

Practical consequence: your ESP32/S3/C3 boards and your C5/C6 boards need
different platform lines in the same repo. `templates/pio-base/platformio.ini`
has both, separated and labelled.

## 9. Two failure modes specific to this pairing

**Confident wrong pin maps.** An LLM will produce a plausible GPIO number for
any board you name, because plausible GPIO numbers are easy. The provenance
system exists entirely for this. When a pin is reported to you, ask where it
came from; if the answer is not a URL or a probe, treat it as a guess.

**Version-blind commands.** esptool 5.x renamed every subcommand, and every
tutorial, forum post, and video on the internet still uses the 4.x spelling.
Anything copied from training data or a transcript will fail on your machine.
The tools here detect the dialect at runtime; hand-written commands will not.
