# Recorded esptool output

Empty until a board is attached. Populate with:

```bash
ESP32_RECORD_FIXTURE=tests/fixtures/<chip>.txt python3 tools/esp32ident.py
```

`tools/test_banner.py` picks up every `*.txt` here automatically and asserts the
chip is identified and the MAC is exactly six octets.

Why this matters: fixtures written by hand only prove the parser is
self-consistent. Recorded output proves it matches what esptool on *this*
machine actually emits — and pins the format, so a future esptool release that
changes the banner breaks a test instead of silently breaking a backup key.
