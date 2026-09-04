---
name: esp32-workbench
description: Connect to, identify, read, back up, program, and debug any ESP32-family board over USB. Use whenever the user plugs in an ESP32/ESP8266 board, asks what a connected board is, wants firmware built or flashed, wants existing firmware or filesystem contents pulled off a device, or is debugging a board that boots wrong, displays wrong, or will not connect. Covers ESP32, S2, S3, C2, C3, C5, C6, C61, H2, P4 and vendor boards from Espressif, Adafruit, Seeed/XIAO, Waveshare, LilyGO, M5Stack, and unbranded clones. Triggers on "what is this board", "flash this", "read the firmware", "dump the flash", "my display is wrong", "esptool", "platformio", "pio", "/dev/cu.usbmodem", "/dev/ttyUSB".
---

# ESP32 Workbench

Identify an unknown board from silicon up, persist what you learn, and program
it — without ever destroying what was already on it.

Repo root: the directory containing `tools/`, `boards/`, `templates/`.
All commands below are run from there.

## The one rule

**Never write to a board that has no verified backup.**

Two things enforce this, so it holds on the paths people actually use:

- `tools/esp32flash.py` gates its own `flash` and `erase`.
- `templates/idf-base/idf_ext.py` is an ESP-IDF `global_action_callback`.
  `idf.py` loads `idf_ext.py` from the project root automatically, and the
  callback runs before any task — so it covers `idf.py flash`, `app-flash`,
  `erase-flash`, `dfu-flash` and every `*-flash` target.
- `templates/pio-base/scripts/backup_gate.py` does the same for the optional
  PlatformIO/Arduino path, covering `pio run -t upload`, `-t uploadfs`, and the
  platformio MCP server's `upload_firmware`.

**eFuse is deliberately excluded from the backup gate.** Burning a fuse is a
one-way physical change; no flash image can undo it, so answering "you have a
backup" would imply a protection that does not exist. `idf.py efuse-burn` and
friends are refused outright and need their own opt-in,
`ESP32_EFUSE_I_UNDERSTAND=1`. `ESP32_NO_GATE=1` does **not** unlock them.

It fails closed: if the gate cannot load, the upload is refused rather than
waved through. Bypass deliberately with `ESP32_NO_GATE=1`, which prints a loud
warning and does not depend on any of the machinery it escapes.

Do not route around it by calling `esptool write-flash` directly. A $20 board is
cheap, but the vendor's factory firmware is often the only existing copy of a
working driver for that exact display, and it is not redistributed anywhere.

Caveat worth knowing: a project copied **outside** this repo cannot find the
gate and will print `BACKUP GATE INACTIVE` before proceeding unguarded. That is
deliberate -- it announces its own absence rather than silently implying safety.

## Order of operations

Work in this sequence. Each step's output is the next step's input.

### 1. Look before touching

```bash
python3 tools/esp32ident.py --list
```

Ports with no USB VID/PID are filtered out — Bluetooth devices and virtual
consoles are not ESP32s. If nothing appears, the board is unplugged, the cable
is charge-only, or a serial monitor is holding the port. Say which you suspect;
do not silently retry.

### 2. Identify

```bash
python3 tools/esp32ident.py --save
```

This is read-only. It probes USB descriptors, chip identity and eFuse features,
SPI flash ID and true size, the partition table at `0x8000`, and the
`esp_app_desc_t` of every app partition — then writes
`boards/<mac>.yaml`.

Read the profile back before doing anything else. If a profile for that MAC
already exists, **you have seen this board before** — use it rather than
re-deriving. That is the entire point of the store.

### 3. Fill the gaps — see `references/research.md`

Probing cannot reveal pin assignments, display controllers, battery divider
ratios, or peripheral wiring. Those are PCB decisions, absent from the silicon.
The profile's `research_queue` names exactly what is missing.

Resolve each entry against authoritative sources first, and **record where
every fact came from**. Never write a pin number into a profile without a
provenance tag and a source URL. A guessed GPIO is not a failed test — it is a
short circuit.

### 4. Read what is already there

```bash
python3 tools/esp32dump.py --all          # partitions, filesystems, NVS, forensics
python3 tools/esp32dump.py --from-image backups/<mac>/full-*.bin   # offline
```

Be honest with the user about what this can and cannot do: **source code is not
recoverable.** The chip holds a stripped binary. What comes back is the flash
image, partition layout, unpacked SPIFFS/LittleFS contents, NVS key/values, and
build fingerprints (project name, IDF version, toolchain, build host paths,
embedded URLs).

### 5. Back up — always, before any write

```bash
python3 tools/esp32flash.py backup
```

About 3 minutes for 8MB at the default 460800 baud (measured 12 min at esptool's 115200 default). Keyed by eFuse MAC, hashed, and recorded in a
manifest. Two boards of the same model are different objects and get different
backups.

### 6. Build and upload — see `references/flashing.md`

**ESP-IDF is the primary framework.** The version is pinned in `.idf-version`
and must match the exported shell.

```bash
IDF_TOOLS_PATH=~/.espressif-6.0.3 . ~/esp/esp-idf-v6.0.3/export.sh

cp -r templates/idf-base projects/<name> && cd projects/<name>
idf.py set-target esp32c6     # from the probed profile's chip_family
idf.py build
idf.py flash                  # goes through the backup gate
idf.py monitor
```

`set-target` is per-project and rewrites sdkconfig, so one project serves one
chip. Copy the template again for a different board rather than re-targeting.

**PlatformIO/Arduino remains available** at `templates/pio-base/` for work that
needs the Arduino display ecosystem (TFT_eSPI, LovyanGFX, GxEPD2, LVGL
wrappers). It is a sibling, not a legacy path — but note the official
`espressif32` platform cannot build Arduino for C5/C6/H2/P4; that template pins
the pioarduino fork for those.

### 7. Debug — see `references/troubleshooting.md`

When the board runs but behaves wrong, **ask the user for a photograph of it.**
A shifted display, wrong colours, a mirrored image, or ghosting each have
distinct visual signatures that identify the fault far faster than reasoning
from source. You cannot see the panel; they can. Ask.

## Provenance discipline

Every fact in a board profile carries one of these. Respect the distinction —
it is what keeps this from being confident guessing:

| Level | Meaning |
|---|---|
| `probed` | Read off the silicon. Unarguable. |
| `usb` | From the USB device descriptor. |
| `vendor_doc` | From a vendor datasheet/schematic. Carries `source`. |
| `community` | Forum, wiki, or GitHub repo. Carries `source`. |
| `inferred` | Deduced from probed facts. Explain in `note`. |
| `unverified` | A guess. **Never wire power to a pin marked this way.** |
| `verified` | Was inferred/unverified, then confirmed on hardware. |

When you state a spec to the user, state its provenance too. "GPIO 21, per the
Waveshare schematic" and "GPIO 21, my best guess" are different claims and must
not be delivered in the same voice.

## Toolchain facts worth not re-learning

- **esptool 5.x uses hyphenated subcommands** (`read-flash`, `write-flash`,
  `flash-id`). 4.x used underscores. Every tutorial online uses the 4.x form.
  `tools/esp32ident.py` detects the dialect at runtime — go through it rather
  than hardcoding either.
- A board in run mode may need **BOOT held while tapping RESET** to enter the
  bootloader. Native-USB chips re-enumerate on reset, so the port path can
  change mid-operation.
- `pio device monitor` and flashing contend for the same port. Stop the monitor
  before uploading.
- Flash size reported by a vendor listing is frequently wrong. Trust
  `flash-id`.

## References

- `references/identification.md` — what each probe stage reads and why
- `references/research.md` — source allowlist, provenance rules, pin verification
- `references/flashing.md` — PlatformIO, frameworks, partition schemes
- `references/troubleshooting.md` — connection failures, display faults, vision debugging
- `references/board-profile.md` — the profile schema
