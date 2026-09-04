# Filling gaps probing cannot fill

Probing tells you what silicon is present. It cannot tell you how the board's
designer wired it. Pin maps, display controllers, backlight polarity, battery
divider ratios, and I2C addresses are PCB decisions that leave no trace in the
chip. Those must be researched — carefully, because a wrong pin number is not a
failed test, it is a short circuit.

## Source ladder

Work down this list. Stop at the first tier that answers the question, and
record which tier you used.

### Tier 1 — vendor primary (provenance: `vendor_doc`)

Verified reachable:

| Source | Use for |
|---|---|
| https://products.espressif.com/ | Official chip/module selector: flash, PSRAM, package, variants |
| https://www.espressif.com/en/support/documents/technical-documents | Datasheets, TRMs, hardware design guidelines |
| https://docs.espressif.com/projects/esp-idf/en/stable/esp32/index.html | ESP-IDF API and SoC capability docs (swap `esp32` for `esp32s3`, `esp32c6`, etc.) |
| https://docs.espressif.com/projects/esptool/en/latest/esp32/ | esptool behaviour, bootloader entry, flash modes |
| https://learn.adafruit.com/ | Adafruit board pinouts and schematics — excellent, and usually definitive |
| https://wiki.seeedstudio.com/ | Seeed / XIAO board wikis, pinout diagrams |
| https://www.waveshare.com/wiki/Main_Page | Waveshare board wikis, schematic PDFs, demo repos |

**The single most valuable artefact is the schematic PDF.** It settles pin
questions that every forum post will disagree about. Look for a "Resources",
"Documents", or "Schematic" link on the vendor's product page.

### Tier 2 — board definitions as machine-readable truth (provenance: `vendor_doc`)

Board support packages encode the pin map in code, which is far less
ambiguous than prose:

- **https://components.espressif.com** — the ESP Component Registry. Search the
  board name; many vendor boards have a BSP component whose `bsp/*.h` encodes
  panel type, offsets and every pin.
- **https://github.com/espressif/esp-bsp** — Espressif's own board support
  packages. Read `bsp/<board>/include/bsp/*.h` for the pin map directly.
- https://github.com/espressif/arduino-esp32 — `variants/<board>/pins_arduino.h`
  is a machine-readable pin map for hundreds of boards. **Still valid on an
  ESP-IDF bench**: a pin map is a property of the PCB, not of the framework you
  compile with. Read the GPIO numbers out of it; do not adopt it as a build path.
- https://registry.platformio.org/ — board `.json` files, same caveat: useful as
  a source of numbers, not as a toolchain.

Locally, PlatformIO's board database is already on disk:

```bash
pio boards esp32 --json-output | python3 -m json.tool | head -50
pio boards | grep -i <vendor>
grep -rl "<board-name>" ~/.platformio/platforms/*/boards/ 2>/dev/null
```

### Tier 3 — community (provenance: `community`)

For clone boards and unbranded hardware, this is often the *only* source. It is
also where wrong pin maps propagate by copy-paste. Require **two independent
sources that agree**, and record both URLs.

Good hunting grounds: the vendor's own GitHub demo repo (search the board name
plus `pins_arduino.h` or `User_Setup.h`), TFT_eSPI and LovyanGFX setup files,
and Home Assistant / ESPHome device pages.

These are Arduino-ecosystem files, and that does not disqualify them — they
record which SoC pin is wired to which peripheral, which is a fact about the
board. Transcribe the numbers into your IDF project and mark them
`community`-tier with the URL.

### Tier 4 — inference (provenance: `inferred`)

State the reasoning in the `note` field. Example: "USB interface is a CH340
bridge, therefore the SoC has no native USB peripheral, therefore USB-CDC
console and USB-JTAG debugging are unavailable on this board."

## Verifying a pin before trusting it

A pin map from any tier below `probed` is a hypothesis. Promote it to
`verified` only after hardware confirms it — and confirm in the safe direction:

1. **Read before you write.** Configure the candidate pin as an input with
   pull-up and observe it. Costs nothing, risks nothing.
2. **Test one pin at a time.** A blink sketch on a single candidate, then ask
   the user what physically happened. They can see the board; you cannot.
3. **Never drive an output on an unverified pin that might be a power rail or
   a strapping pin.** On most ESP32 variants, GPIO0, GPIO45, and GPIO46 change
   boot behaviour. Getting this wrong can make a board refuse to boot.
4. **Displays: confirm the controller before the pins.** Rendering with the
   wrong controller driver produces shifted, mirrored, or torn output that
   looks exactly like a pin problem and wastes hours.

Record the outcome either way. A pin map that was *disproved* is as valuable as
one confirmed — write it into the profile as `unverified` with a note saying
what was tried and what happened, so the next session does not repeat it.

## Writing research back

Every researched field lands in the profile as:

```yaml
backlight_pin:
  value: 22
  provenance: vendor_doc
  source: https://www.waveshare.com/wiki/<page>
  note: "From schematic rev 1.2, sheet 2. Active high. Not yet hardware-tested."
```

No source URL means no `vendor_doc` or `community` tag. Downgrade it to
`unverified` instead. This rule exists because a plausible-looking pin number
with no traceable origin is indistinguishable from an invention.
