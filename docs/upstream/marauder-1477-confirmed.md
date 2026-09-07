<!--
POSTED to upstream issue #1477:
https://github.com/justcallmekoko/ESP32Marauder/issues/1477#issuecomment-5567270198
Record of what was sent; edit upstream, not this file.
-->

**Confirmed fixed on real v6.1 hardware.**

Built v1.15.1 for `MARAUDER_V6_1` with `HAS_IP5306` restored on the
V4/V6/V6_1/KIT branch, flashed it via SD update, and the meter is back:

```
before:  Battery Monitor: not supported      (no level line, no status-bar %)
after:   Battery Monitor: supported
         Battery Lvl: 50%                    (status-bar % also restored)
```

Two details that make this more than "it works now":

**The reading is a quarter value.** `getBatteryLevel()`'s IP5306 branch returns
only 25/50/75/100 — it maps two register bits. A MAX17048's float
`cellPercent()` would essentially never land on a round quarter. So 50% is
positive evidence that the **IP5306 probe specifically** is the one answering,
not merely that some battery code runs again. The same reasoning read backwards
fits the original report: this board showed **75%** on v1.12.1.

**The status-bar percentage came back too, and that was predicted rather than
observed.** `i2c_supported` has exactly two consumers — `WiFiScan.cpp` for the
Device Info line and `MenuFunctions.cpp` for the status-bar draw at `SB_BAT_X`.
Only the Device Info symptom was ever reported; the status-bar one was inferred
from the shared boolean and then confirmed on the device.

Build details for anyone reproducing: core `esp32:esp32@3.3.4`, fqbn
`esp32:esp32:d32:PartitionScheme=min_spiffs`, `-DMARAUDER_V6_1`, CI's pinned
libraries and `User_Setup_og_marauder.h`, plus the `platform.txt` `-zmuldefs`
and `-fno-exceptions` steps. My **unpatched control build** lands within 0.07%
of the published binary (1,711,408 vs 1,710,224 bytes) with identical
`app_desc` fields, so the toolchain is faithful and the only meaningful delta in
the patched build is the battery probe.

To be clear about scope, since I raised it earlier: the change I am running is
correct for V6/V6.1 but is **not** a safe upstream patch as-is, because it flips
V4 and KIT to IP5306 as well. The branch groups four targets that evidently do
not all carry the same gauge. Splitting it so each target resolves the IC its own
board block already declares is the fix I would propose — happy to open a PR if
you tell me which way you want the KIT and V4 mapping to go, since you know the
hardware and I only have my own board in front of me.
