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
- **PSRAM** must be enabled explicitly *and* be present per eFuse. If the probe
  firmware says "none usable" while the profile says PSRAM exists, the build is
  the problem, not the board.

Add a file here only after the configuration has actually run on hardware. A
fragment that has merely compiled belongs in a comment, not in this directory.
