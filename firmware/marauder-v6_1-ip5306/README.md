# Marauder v1.15.1, MARAUDER_V6_1, with IP5306 battery support restored

For board `48:9D:31:02:A9:48` — the screen unit of a HoneyHoney Lab "Double
Barrel". Fixes the battery meter that v1.14.0 broke on IP5306-equipped v6/v6.1
hardware. See boards/489d31027e98.yaml `sibling_board.regression_battery_monitor`
and upstream issue #1477.

    marauder-v1.15.1-v6_1-ip5306.bin          1,709,360   e365927a...a042   <- the build
    marauder-v1.15.1-v6_1-stock-control.bin   1,711,408   c9b84934...4d54   <- the control
    ip5306.patch                                          the entire change

## Why a control build is here

The patched binary on its own is unfalsifiable: of course it compiled, only a
`#define` changed. The control is an UNPATCHED build of the same tag with the
same toolchain, and it is what makes the patched one trustworthy:

    official esp32_marauder_v1_15_1_20260824_v6_1.bin   1,710,224 bytes
    stock control build                                 1,711,408 bytes  (+0.07%)

Both carry magic 0xE9, chip_id 0, 6 segments, a segment walk landing byte-exactly
on the file length, and identical app_desc fields (45c1b25 / arduino-lib-builder
/ v5.5.1-710). The residual 1,184 bytes is embedded build paths. A recipe that
produced 1.4MB instead would have been silently wrong in the patched build too.

## Build recipe (from .github/workflows/build_parallel.yml at v1.15.1)

    core      esp32:esp32@3.3.4 from the 3.3.4 package_esp32_dev_index.json
    fqbn      esp32:esp32:d32:PartitionScheme=min_spiffs
    define    compiler.cpp.extra_flags=-DMARAUDER_V6_1
    TFT       Bodmer/TFT_eSPI V2.5.34; repo User*.h copied into it;
              User_Setup_og_marauder.h include uncommented in User_Setup_Select.h
    libs      15 more repos at pinned refs, plus Adafruit_TCA8418 from the repo

TWO STEPS THAT ARE EASY TO MISS AND POISON THE BUILD SILENTLY, both required for
core 3.3.4:
  * platform.txt: compiler.c.elf.extra_flags gains `-Wl,-zmuldefs`
  * every cpp_flags under tools/esp32-arduino-libs: -fexceptions -> -fno-exceptions
Without them the link fails or the output differs materially from the shipped
firmware.

## What the patch actually does, which is NOT what was first assumed

It selects IP5306 INSTEAD OF MAX17048, not in addition. configs.h ~line 2931 has
a cleanup chain commented "If we know what we have, we can delete what we're not
using", and `#elif defined(HAS_IP5306)` there does `#undef HAS_MAX1704X`.

Correct for THIS board, which has an IP5306 and no MAX17048. It is a narrower
change than "probe for both", and the difference matters upstream -- the fix
suggested in the first #1477 comment was wrong for exactly this reason and was
retracted in a second comment.

HOW THAT WAS CAUGHT, because it nearly was not: the patched build came out 2,048
bytes SMALLER than the control, when adding a code path should make it larger.
That anomaly was the only reason to look. Verified by string count rather than
by re-reading the source:

                        "Detected IP5306"   "Detected MAX17048"
    official                    0                   1
    stock control               0                   1
    patched                     1                   0

The stock control matching the official binary on both counts is what makes that
table evidence rather than coincidence.

## Flashing

USB is not available on this board -- it has no data path (see
sibling_board.usb_path). The only route is the SD updater: copy the .bin to the
card ROOT as `update.bin`, then Device > Update firmware > SD Update > Yes.
SDInterface::runUpdate defaults to "/update.bin" and fails with "Could not load
update.bin from sd root" if it is anywhere else.

RISK, stated plainly: this is a custom build on a board with NO USB recovery. If
it does not boot, the Arduino core does not set
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so there is no automatic rollback and no
way back in. The OTA contract still helps -- the image is written to the inactive
slot and esp_ota_set_boot_partition validates before committing otadata -- so a
corrupt or incomplete WRITE is recoverable. A cleanly-written image that panics
at runtime is not.
