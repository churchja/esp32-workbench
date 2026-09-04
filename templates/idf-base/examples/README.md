# Known-good sdkconfig fragments

One file per board configuration that has been **built, flashed, and verified on
real hardware** — not assembled from documentation.

| File | Board | Verified |
|---|---|---|
| `s3-8mb.sdkconfig.defaults` | ESP32-S3, 8MB flash + 8MB PSRAM, native USB | 2026-09-04, MAC `e0:72:a1:fb:9c:5c` |

Two settings here are not guessable and are the usual reason a board misbehaves:

- **Flash size** must match `identity.flash_size` from the probed profile. IDF
  defaults to 2MB, which silently sizes the partition table for a quarter of an
  8MB part.
- **Console** must match the chip's USB peripheral, and on USB-OTG parts this
  is harder than it looks. See the note below.
- **PSRAM** must be enabled explicitly *and* be present per eFuse. If the probe
  firmware says "none usable" while the profile says PSRAM exists, the build is
  the problem, not the board.

Add a file here only after the configuration has actually run on hardware. A
fragment that has merely compiled belongs in a comment, not in this directory.

## Open problem: console on USB-OTG parts (ESP32-S2)

There is deliberately **no S2 example here**, because none has worked.

Parts with a USB-Serial/JTAG controller (S3, C3, C6, C5, H2, P4) get
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` automatically, so the stock
template is readable over USB with no changes. Verified on the S3.

The ESP32-S2 has no such controller — `soc_caps.h` declares only
`SOC_USB_OTG_SUPPORTED`. Two attempts, both flashed and hash-verified on real
hardware (MAC `d4f98d661364`), both produced a board that **runs correctly and
presents no USB device at all**:

1. Stock template. No secondary console exists for OTG, so output went to UART
   pins and USB was never initialised.
2. `CONFIG_ESP_CONSOLE_USB_CDC=y`. Still nothing, after a watchdog reset *and*
   after a clean power cycle.

The likely reason is in IDF's own Kconfig help for that option: *"uses the CDC
driver in the chip ROM... incompatible with TinyUSB stack."* The OTG peripheral
must be driven by a device stack to enumerate at all. The ROM CDC console works
while the ROM bootloader already holds USB open; it does not bring USB up from
a cold boot into an application. The stock firmware on this board enumerates
because tinyuf2 and CircuitPython both ship TinyUSB.

**If you need console on an S2, the next thing to try is the `esp_tinyusb`
component rather than the ROM CDC option** — untested here.

Recovery either way: hold BOOT, tap RESET to reach ROM download mode, then
restore the backup. The board is never bricked by this; it is running fine and
simply invisible.
