# Building and uploading

## ESP-IDF is the primary path

Version pinned in `.idf-version` (currently v6.0.3). Export before anything:

```bash
IDF_TOOLS_PATH=~/.espressif-6.0.3 . ~/esp/esp-idf-v6.0.3/export.sh
```

```bash
cp -r templates/idf-base projects/<name> && cd projects/<name>
idf.py set-target esp32c6      # chip_family from the probed profile
idf.py build
idf.py flash                   # passes through the backup gate
idf.py monitor                 # Ctrl-] to exit
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Notes that matter:

- **`set-target` is per project.** It rewrites `sdkconfig` and wipes the build
  dir. One project serves one chip; copy the template again for another board.
- **`idf.py --dry-run flash` writes nothing** and the gate deliberately ignores
  it. Use it to inspect what would run.
- **eFuse is not covered by the backup gate.** A burned fuse cannot be restored
  from any image, so `efuse-burn` is refused outright and needs
  `ESP32_EFUSE_I_UNDERSTAND=1`. `ESP32_NO_GATE=1` will not unlock it.
- **The gate pins the port.** `idf.py` re-resolves the serial port at execution
  time, and that resolver is connection-dependent, so the gate writes its
  resolved port onto the task to guarantee the board it checked is the board
  that gets written.
- IDF v6.0.3 bundles **esptool 5.4.0** — same major and dialect as a typical
  host install, so the workbench tools behave identically inside or outside an
  IDF shell. Do not assume that for other IDF lines; v5.5 pins esptool 4.x,
  which uses underscored subcommands.

## PlatformIO / Arduino — the optional second path

Kept for work that needs the Arduino display ecosystem (TFT_eSPI, LovyanGFX,
GxEPD2). It is a sibling framework, not a legacy version.

## Two paths, chosen once per session

**PlatformIO MCP tools**, when the session has them: `list_devices`,
`get_board_info`, `init_project`, `install_library`, `build_project`,
`upload_firmware`, `upload_filesystem`, `start_monitor`, `query_logs`,
`run_tests`. Prefer these — they return structured results rather than text to
parse, and `query_logs` beats scraping a build log.

**CLI**, otherwise, or on a machine without the MCP server:

```bash
pio run                        # build
pio run -t upload              # build + flash
pio run -t uploadfs            # flash the filesystem image
pio run -t clean
pio device list
pio device monitor -b 115200
pio boards esp32 --json-output  # local board database
```

Decide which path is available, then stay on it. Alternating produces confusing
half-states — a monitor opened by one path holding the port against the other.

## Choosing a board ID

`platformio.ini` needs a `board`. Three cases:

1. **Board is in the PlatformIO registry** — use its ID. Find it with
   `pio boards | grep -i <vendor>` or the registry search.
2. **Board is not, but the module is** — target the generic module
   (`esp32-s3-devkitc-1`, `esp32-c6-devkitc-1`, `esp32dev`) and supply the
   board-specific pins yourself via `build_flags`. This is the common case for
   clones and small vendors, and it works fine.
3. **Nothing matches** — write a custom board JSON in `boards/` of the project.
   Rare; try case 2 first.

Set flash size and PSRAM from the **probed** profile, not from the board
definition's defaults — the definition describes a reference design, and your
board may be a variant.

## Which platform — this bites on newer chips

`platform = espressif32` is not one thing. Verified 2026-09-04:

| Chips | Platform to use |
|---|---|
| ESP32, S2, S3, C3 | Official `espressif32` (6.x or 7.x). Works. |
| **C5, C6, H2, P4** | **Official platform does NOT support `framework = arduino`.** Use the pioarduino fork. |

The official platform — installed 6.13.0 *and* current 7.1.0 — pins Arduino
core **2.0.17**, which predates these chips. Its `esp32-c6-devkitc-1.json`
declares `frameworks: ["espidf"]` only, so an Arduino build fails in under a
second with `Error: This board doesn't support arduino framework!`. Upgrading
the official platform does not help; the same core is pinned.

```ini
[env:my-c6]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board = esp32-c6-devkitc-1
framework = arduino
```

pioarduino tracks arduino-esp32 master with ESP-IDF 5.5.5 and ships 299 boards
(5 C5, 10 C6, 1 H2, 6 P4), all declaring `["arduino", "espidf"]`. First build
pulls ~1GB of toolchain, once.

If a build fails instantly rather than after compiling, suspect this before
suspecting the code — an instant failure is a board/framework mismatch, not a
syntax error.

## Frameworks

- `framework = arduino` — default here. Widest library support, and the whole
  display/graphics ecosystem (TFT_eSPI, LovyanGFX, LVGL, GxEPD2, FastLED)
  assumes it.
- `framework = espidf` — when you need FreeRTOS control, low-power/deep-sleep
  precision, or an IDF-only peripheral driver.
- Both together is supported but complicates builds; do not reach for it
  without a reason.

## Partition schemes

The default app partition is often smaller than a display-plus-BLE build needs.
When a build fails on size, the fix is usually a partition scheme, not code
cutting:

```ini
board_build.partitions = huge_app.csv      ; single large app, no OTA
; or min_spiffs.csv (OTA + small FS), default.csv, or a custom CSV
```

Changing the partition scheme rewrites the flash layout — take a backup first
if the existing layout holds anything you want.

## Size as a signal

Compare a build against the factory image size from the profile. A vendor demo
carrying full Wi-Fi and Bluetooth stacks is often 1–2MB; a focused build with
radios disabled can be a few hundred KB. A build that is *unexpectedly large*
usually means a stack got linked in that the project does not use — worth
finding before it crowds the partition.

## After flashing

Confirm the board actually booted rather than assuming success from a clean
upload. Open the monitor and read the boot log. A panic-reboot loop prints a
backtrace every few seconds and is unmistakable — and is invisible if you never
look.
