# When it does not work

## The board will not connect

Work through these in order; each is more intrusive than the last.

1. **Nothing on `--list`.** Unplugged, or a charge-only USB cable — a
   surprisingly common cause with cables that came with something else. Try a
   different cable before anything else.
2. **Port appears, esptool times out.** Something else holds it. Close any
   serial monitor (`pio device monitor` in another terminal is the usual
   culprit).
3. **Still failing.** Force the bootloader: hold **BOOT**, tap **RESET**,
   release BOOT. The board now waits for a flash operation instead of running
   firmware.
4. **Native-USB boards vanish mid-operation.** Chips with native USB
   re-enumerate on reset, so the port path can change. Re-run `--list`.
5. **Connects then drops under load.** Insufficient supply current — try a
   powered hub or a different port. Boards with displays and radios draw more
   than some hubs deliver.

## Vision debugging — use this, it is the highest-leverage tool here

When the board runs but looks wrong, **ask the user to photograph the screen
and paste the image in.** You have no eyes on the panel and cannot infer from
source what a display is physically doing. Visual faults have distinct
signatures that are obvious in a photo and near-impossible to deduce otherwise:

| What the photo shows | Almost always means |
|---|---|
| Image shifted by a fixed pixel count | Row/column offset wrong for this panel variant — same controller, different glass origin |
| Previous image visible underneath | Framebuffer not cleared, or e-paper needing a full refresh rather than partial |
| Colours inverted or swapped | RGB/BGR order, or an inversion flag the panel needs and the driver defaults off |
| Mirrored horizontally or vertically | Rotation/mirror bits in the MADCTL register |
| Right edge wrapping to the left | Stride/width mismatch — driver configured for a wider panel |
| Ghosting or streaking on e-paper | Insufficient refresh passes, or wrong waveform LUT |
| Text rotated 90° | Rotation set for a different physical orientation than the user is holding |

Ask for the photo early. It is faster than reading the driver, and it is
evidence rather than inference.

## Build failures

- **App too large** — change the partition scheme before cutting features (see
  `flashing.md`).
- **Library compiles but crashes at boot** — usually a pin conflict with a
  strapping pin or PSRAM line. Check the profile's pin map and its provenance;
  an `unverified` pin is the first suspect.
- **PSRAM not detected** — must be enabled explicitly
  (`board_build.arduino.memory_type`, or `-DBOARD_HAS_PSRAM`) *and* be present
  per the probed eFuse features. If probing did not report PSRAM, the board
  does not have it, whatever the listing said.

## Runtime failures

- **Boot loop with a backtrace** — read the monitor. The backtrace names the
  fault; do not guess without it.
- **Brownout detector triggered** — power, not code. Same causes as the
  connection drop above.
- **Board boots to the wrong firmware after flashing** — an OTA data partition
  is pointing at a different app slot. Check the partition table.

## When a fix does not work

Return to observation rather than trying a variant of the same fix. Re-read the
boot log, re-take the photo, re-probe. One hypothesis per attempt; a failed fix
means the diagnosis was wrong, not that the implementation was close.
