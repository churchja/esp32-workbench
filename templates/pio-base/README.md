# pio-base — safe starting template

Copy, do not edit in place:

```bash
cp -r templates/pio-base projects/my-thing
cd projects/my-thing
```

Then set `board` in `platformio.ini` from the identification profile, pick the
matching `[env:...]`, and build:

```bash
pio run -e <env> -t upload
pio device monitor -b 115200
```

`src/main.cpp` drives no GPIO and starts no radios, so it is safe to flash onto
a board whose pin map is still `unverified`. Its output is runtime ground truth
to reconcile against the offline probe — where the two disagree, that gap is
usually a build config claiming hardware the silicon does not have.

## The upload gate

`platformio.ini` wires `scripts/backup_gate.py` as a pre-upload hook, so
`pio run -t upload` refuses to write to a board with no verified backup. It
fails closed. Bypass deliberately:

```bash
ESP32_NO_GATE=1 pio run -e <env> -t upload
```

A copy of this template placed outside the repo prints `BACKUP GATE INACTIVE`
and proceeds unguarded — it tells you it is not protecting you rather than
pretending it is.
