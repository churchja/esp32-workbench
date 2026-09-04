# Board profile schema

One file per physical board, at `boards/<mac-without-colons>.yaml`, keyed by
eFuse MAC. Two boards of the same model are different files.

Every leaf fact is `{value, provenance, source?, note?}`. A bare value with no
provenance is a schema violation — it makes a guess indistinguishable from a
measurement.

```yaml
schema_version: 1
profile_id: 30aea4xxxxxx          # MAC, colons stripped
identified_at: 2026-09-04T09:00:00Z
port_seen_on: /dev/cu.usbmodem101

identity:                          # written by esp32ident.py -- probed
  chip:            {value: "ESP32-C6 (QFN40) (revision v0.1)", provenance: probed}
  chip_family:     {value: "ESP32-C6", provenance: probed}
  chip_features:   {value: ["WiFi 6", "BT 5", "IEEE802.15.4"], provenance: probed}
  mac:             {value: "30:ae:a4:xx:xx:xx", provenance: probed}
  flash_size:      {value: "8MB", provenance: probed}
  usb_interface:   {value: "Espressif USB JTAG/serial debug unit", provenance: usb}
  usb_interface_kind: {value: native, provenance: usb}
  partitions:      {value: [...], provenance: probed}
  applications:    {value: [{project_name: ..., idf_version: ...}], provenance: probed}

board:                             # researched -- who made it, what it is
  vendor:      {value: Waveshare, provenance: vendor_doc, source: "https://..."}
  model:       {value: ESP32-C6-LCD-1.47, provenance: vendor_doc, source: "https://..."}
  pio_board:   {value: esp32-c6-devkitc-1, provenance: inferred,
                note: "No exact registry entry; generic C6 module + explicit pins"}

display:                           # researched -- probing cannot see this
  controller:  {value: ST7789, provenance: vendor_doc, source: "https://..."}
  resolution:  {value: [172, 320], provenance: vendor_doc, source: "https://..."}
  row_offset:  {value: 34, provenance: verified,
                note: "Confirmed on hardware: image was shifted 34px until set"}

pinmap:
  lcd_mosi:    {value: 6,  provenance: vendor_doc, source: "https://..."}
  lcd_bl:      {value: 22, provenance: unverified,
                note: "Single forum source, not confirmed. Do not drive yet."}

power:
  battery_adc_divider: {value: null, provenance: unverified,
                        note: "Unknown ratio. Any battery percentage computed
                               before this is measured is fiction."}

verification_log:                  # append-only; failures are as useful as passes
  - date: 2026-09-04
    tested: display.row_offset
    method: "Flashed test pattern, user photographed screen"
    result: verified
    note: "34px offset confirmed; image aligned after applying"

research_queue:                    # auto-generated: what is still unknown
  - field: pinmap
    status: unknown
    why: "GPIO assignment is a PCB routing decision, invisible to probing."
    how: "Vendor schematic, then board .json / pins_arduino.h, then community."
```

## Rules

- **Never** promote a field to `verified` without a `verification_log` entry
  describing the physical test.
- **Never** write `vendor_doc` or `community` without a `source` URL. Downgrade
  to `unverified` instead.
- Record disproved hypotheses. A pin that was tried and did not work saves the
  next session from repeating it.
- When reporting a spec to the user, report its provenance alongside. A
  schematic-sourced pin and a guess must not be delivered in the same voice.
