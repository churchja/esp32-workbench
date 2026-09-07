# Marauder v6.1: what the v1.12.1 → v1.15.1 upgrade actually cost

Board `48:9D:31:02:A9:48` — screen unit of a HoneyHoney Lab Double Barrel,
target `MARAUDER_V6_1`, classic ESP32, 4MB, min_spiffs.

## Bottom line

**The battery monitor is the only thing that broke.** Nothing else regressed.

That is a real result, not a shrug, and it is worth more than a list of maybes.

## How that was established

Two independent methods, both reproduced locally rather than taken on trust:

1. **Preprocessor symbol diff.** Resolved `configs.h` with the real C preprocessor
   at both tags, `gcc -E -dM -x c++ -DMARAUDER_V6_1`, and diffed the final macro
   sets. This matters because configs.h declares macros in per-board blocks and
   then *revokes* them 2,500 lines later in "cleanup" ladders — reading the board
   block alone gives the wrong answer, which is exactly the trap this whole bug
   sits in.

   Result: **the only symbol that differs is `MARAUDER_VERSION`.** No `HAS_*`
   flag went from defined to undefined. Additions only.

2. **Reachability diff across all 42 common source files**, resolving every
   `#if/#ifdef/#elif` against this board's macro set, looking for code that was
   live at v1.12.1 and is still present but now compiled out.

   Result: **one file hit — `BatteryInterface.cpp`**, 14 lines. Everything else
   that looked missing was a rename or a refactor.

## The one regression, fully traced

**Culprit: PR #1392 "Fix v7 battery and wardriving"**, merged 2026-07-20,
`f379cd59cb98`, shipped in v1.14.0. Its configs.h diff on this branch is exactly:

    -      #define HAS_IP5306
    +      #define HAS_MAX1704X
    +      #undef HAS_IP5306

It flipped V4/V6/V6_1/KIT together with V7 and V7_1 while fixing v7. This board
was collateral damage in someone else's board fix.

### The three-version story, which is subtler than "a flag got removed"

    v1.12.1   no HAS_IP5306 macro anywhere, and NO #ifdef guards in
              BatteryInterface. RunSetup() probed 0x75 and 0x36 unconditionally.
              Whichever chip answered, won. WORKED.
    v1.12.3   guards introduced AND #define HAS_IP5306 added for this group.
    v1.13.0   same. WORKED.
    v1.14.0   #define replaced by #undef; guards remain. The IP5306 probe is no
              longer compiled at all. BROKEN.

So `HAS_IP5306` was undefined in v1.12.1 *and* v1.15.1 — the config diff between
those two tags shows nothing removed. The regression only exists in the
*interaction*: guards were added while the macro they depend on was taken away.
An audit comparing only the two endpoint configs would have found nothing.

### Two symptoms, one cause

`i2c_supported` never becomes true. It has exactly two consumers:
`WiFiScan.cpp` (the Device Info line) and `MenuFunctions.cpp` (the status-bar
percentage). Both die together. The status-bar symptom is derived from source,
not observed — but it is one boolean feeding an unconditional call site.

### Corroboration from your own screen

v1.12.1 reported **75%**. The IP5306 path returns only 25/50/75/100 — it reads
two register bits. A MAX17048's float `cellPercent()` would essentially never
land on a round quarter. The screenshot is itself evidence of which chip answered.

## Patch variants, measured not assumed

Preprocessed each variant for three targets in the shared branch:

    variant                          V6_1        KIT                            V4
    stock v1.15.1                    MAX1704X    MAX1704X                       MAX1704X
    ours (add define, drop undef)    IP5306      IP5306                         IP5306
    delete both lines                IP5306      AXP192+IP5306+MAX1704X  <-- !  IP5306

The delete-both variant drops KIT through the ladder to the `#else // punt`
fallback at :2951 and it ends up with three battery drivers at once — worse than
today. Ours does not do that, but it does flip KIT and V4 to IP5306, which is
wrong upstream if any board in that group genuinely carries a MAX17048.

**Correct for this board. Not correct as an upstream patch.** The real fix splits
the branch so each target selects the IC it actually has.

## Changed but irrelevant here

- `MARAUDER_VERSION` string.
- `HAS_DIRECT_UPLOAD`, `HAS_IDF_3`, `HAS_NIMBLE_2` — pure additions, absent from
  the v1.12.1 tree entirely.
- Status-bar X positions moved from literals to macros (`SB_MEM_X`, `SB_SD_X`,
  `SB_WIFI_X`…). Verified byte-identical output after expansion on this target.

## Checked and clear

GPS, SD, touch, NeoPixel, temperature sensor, the Flipper UART link, screen and
menus. No gate was added without its macro. SD, Wi-Fi, screen and touch are also
confirmed working by observation on the device at v1.15.1.

## What this audit cannot cover

Static source analysis only. No device was instrumented. Runtime behaviour,
timing, RF performance and memory pressure are all outside what was examined —
a feature that still compiles can still misbehave. The status-bar battery
symptom in particular is inferred, not observed.

## Upstream state

Four independent reporters, all bisecting to v1.14.0:

    #1477  stevedee78                 V6.1
    #1403  Clawzman + 2 commenters    V6 and V6.1, 1.14.0 and 1.14.1

Both issues open. Neither was diagnosed before this work.

Fix submitted as **https://github.com/justcallmekoko/ESP32Marauder/pull/1534** —
one file, +9 -1, splitting the shared branch so KIT keeps its current driver
while V4/V6/V6.1 keep the `HAS_IP5306` their own board blocks already declare.
The split needed no guesswork in the end: `MARAUDER_V4` declares `HAS_IP5306` at
`configs.h:257` and V6/V6.1 at `:278`, while KIT, V7 and V7_1 declare no IC at
all. The source answered the question the maintainer had not.

Also found: PR #1326 (v1.13.0) was an earlier attempt at this same bug and added
a regression guard, `tools/check_battery_driver_macros.ps1`. It is not wired into
anything — no workflow, Makefile or hook references it anywhere in the tree. A
guard nobody runs did not stop the same bug returning one release later.

## Two errors in what we published upstream

1. Both comments say `BatteryInterface::begin()`. The method is **`RunSetup()`**;
   there is no `begin()` at either tag. Asserted twice without checking, after an
   earlier grep for it returned nothing and that silence was not noticed.
2. The first comment's suggested fix ("define both") is wrong and was retracted;
   defining `HAS_IP5306` undefines `HAS_MAX1704X` via the cleanup ladder.

Both were caught by running things, not by re-reading. That is the pattern of the
whole session.
