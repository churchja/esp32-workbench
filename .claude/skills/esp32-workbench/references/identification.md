# What each probe stage actually reads

`tools/esp32ident.py` runs six stages. Each degrades gracefully — a failure at
stage N still emits what stages before it learned.

## 0 — Port enumeration

Lists serial ports, keeping only those with a USB VID/PID. An ESP32 reached
over USB always has one; Bluetooth serial ports, `debug-console`, and virtual
ports never do. This is a structural test, not a name blocklist, so it does not
need maintaining as new peripherals appear. `--any-port` disables it.

## 1 — USB identity

VID/PID answers a question that matters more than it first appears: **does the
SoC speak USB itself, or is a bridge chip translating?**

- `303a:1001` — Espressif native USB JTAG/serial. The chip has a USB
  peripheral: USB-CDC console and USB-JTAG debugging are available, and the
  port re-enumerates on reset.
- `1a86:7523` (CH340), `10c4:ea60` (CP210x), `0403:6001` (FTDI) — a bridge.
  The SoC has no USB peripheral. Classic ESP32-D0WD boards and most cheap
  clones are here.

Vendor VIDs (`239a` Adafruit, `2886` Seeed, `1b4f` SparkFun) identify who made
the board, which narrows the documentation search immediately.

## 2 — Silicon

`esptool flash-id` reports chip type, revision, eFuse feature list, crystal,
and base MAC in one connection.

The **feature list is authoritative** for radios and embedded memory — it is
read from eFuse, not from a build config or a product listing. If it says
`Embedded PSRAM 8MB`, the board has 8MB of PSRAM regardless of what the seller
claimed.

The **MAC is the board's identity.** It is globally unique per device, so
profiles and backups key off it. Ports are reassigned on every replug and board
models are shared by many physical objects; neither works as a key.

## 3 — Flash

Manufacturer ID, device ID, and detected size, read from the SPI flash chip
itself. Vendor listings get this wrong often enough that the probed value
should always win.

## 4 — Partition table

Read from `0x8000` (0xC00 bytes) and parsed directly. Each 32-byte entry is
magic `0x50AA`, type, subtype, offset, size, 16-byte label, flags; an `0xEBEB`
entry carries the table MD5.

This is ground truth for flash layout. It tells you how much space an app
actually has, whether OTA is set up, and whether a filesystem partition exists
worth extracting.

## 5 — Application descriptor

`esp_app_desc_t` sits at offset `0x20` in an app partition, behind magic
`0xABCD5432`. It yields project name, app version, build date and time, the
IDF version that built it, and the ELF SHA-256.

**Read this carefully — the descriptor often does not describe the sketch.**

Verified 2026-09-04 against a freshly compiled Arduino-framework binary: the
descriptor reported `project_name: arduino-lib-builder`, `idf_version:
v4.4.7-dirty`, `build_date: Mar 5 2024`. That is Espressif's build machine, not
the local one — the build was seconds old. The Arduino core ships as prebuilt
IDF libraries, so its descriptor metadata survives into every Arduino project.

So:

| Question | Where the answer actually is |
|---|---|
| Which SDK/core built this? | app descriptor — reliable |
| What is this firmware *for*? | **string mining**, not the descriptor |
| Who built it, on what machine? | string mining (`build_host_path`) |

In the same test, string mining recovered text unique to the sketch while the
descriptor showed none of it. For an **ESP-IDF** project the descriptor does
carry the real project name; for **Arduino** it usually does not. Check the
`idf_version` and build date against what you would expect — a build date years
in the past on freshly flashed firmware is the tell.

## 6 — Emit

Everything merges into `boards/<mac>.yaml` with a provenance tag per field, plus
a `research_queue` naming what probing structurally cannot answer. An existing
profile is preserved to a `.bak` before being overwritten.
