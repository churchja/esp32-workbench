# idf-usb-console — console over USB on USB-OTG parts

A **sibling** of `../idf-base`, not a replacement. Use this one only when you
need a console over USB on a part that has **USB-OTG** rather than a
USB-Serial/JTAG controller — in practice, the ESP32-S2.

## Why it is separate

`idf-base`'s defining property is that it drives **no peripheral at all**,
which is what makes it safe to flash onto a board whose pin map is unverified.
A USB device stack *is* a peripheral. Bolting TinyUSB onto the base template
would destroy that guarantee for every board, to gain a console on one family.

So: `idf-base` stays minimal and safe. This template drives the OTG peripheral
deliberately, and says so.

## What it solves

| Part | USB peripheral | Console over USB |
|---|---|---|
| S3, C3, C6, C5, H2, P4 | USB-Serial/JTAG | free — `idf-base` is readable, no changes |
| **S2** | **USB-OTG** | **needs this template** |
| classic ESP32 | none (UART bridge) | UART *is* the console; use `idf-base` |

Two earlier approaches failed on real hardware, both flashed and hash-verified,
both producing a board that ran correctly and presented **no USB device at all**:

1. `idf-base` unmodified. Parts with USB-Serial/JTAG get
   `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` for free; the S2 has no such
   controller, so it gets no secondary console.
2. `CONFIG_ESP_CONSOLE_USB_CDC=y`. IDF's own Kconfig help explains why: it
   *"uses the CDC driver in the chip ROM"* and is *"incompatible with TinyUSB
   stack."* The ROM CDC console works while the ROM bootloader already holds
   USB open; it does not bring USB up from a cold boot into an application.

TinyUSB drives the OTG peripheral itself, which is the missing piece.

## Gotchas that cost time

- **`CONFIG_TINYUSB_CDC_ENABLED=y` is mandatory.** `tinyusb_cdc_acm.h` has a
  hard `#error` guard. The symbol lives inside a *Communication Device Class
  (CDC)* submenu of the component's Kconfig, so a flat grep misses it.
- **`TINYUSB_DEFAULT_CONFIG()` needs `tinyusb_default_config.h`**, a separate
  header. It includes `tinyusb.h`, not the other way round.
- **Print on a loop, not once at boot.** The *device* brings USB up here, so
  the host enumerates and opens the port some indeterminate time later.
  Anything printed before that is lost, and a port that opens and sits silent
  looks exactly like the failure this template exists to fix.
- The board enumerates under TinyUSB's default descriptors (`303a:4001`,
  serial `123456`), **not** its vendor identity. A QT Py ESP32-S2 stops
  announcing itself as one.

## Cost: every reflash needs a manual BOOT+RESET

TinyUSB firmware **owns** the USB peripheral, so the port you see belongs to the
application, not the ROM bootloader. esptool's automatic DTR/RTS entry has
nothing to talk to, and `idf.py flash` will fail to connect.

Hold **BOOT**, tap **RESET**, release BOOT before every flash.

This is the same reason the factory tinyuf2 image resists esptool. It is not a
defect in this template — it is inherent to any firmware that drives USB itself,
and it is the price of a readable console on a USB-OTG part. `idf-base` on a
USB-Serial/JTAG board does not have this cost, because there the ROM keeps the
interface available.

## Verified

Flashed and read on an Adafruit QT Py ESP32-S2 (`ESP32-S2FNR2`, MAC
`d4:f9:8d:66:13:64`) with ESP-IDF v6.0.3 and `espressif/esp_tinyusb 2.2.1`.
Enumerated as `303a:4001` and printed its self-report over CDC.
