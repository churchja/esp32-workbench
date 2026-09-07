<!--
SUBMITTED as https://github.com/justcallmekoko/ESP32Marauder/pull/1534
"Restore IP5306 battery support for V4/V6/V6.1", 1 file, +9 -1, based on
upstream master 91724fd. Branch churchja:fix-ip5306-v4-v6-battery.
Record of what was sent; edit the PR, not this file.
-->

## Problem

Battery monitoring has been broken on Marauder V4, V6 and V6.1 hardware since
**v1.14.0**. Affected devices report `Battery Monitor: not supported` on Device
Info and lose the status-bar percentage.

Reported by four people across two open issues, all bisecting to v1.14.0:
#1477 (stevedee78) and #1403 (Clawzman, plus two commenters).

## Root cause

#1392 ("Fix v7 battery and wardriving") changed the shared
`V4 || V6 || V6_1 || KIT` branch in `configs.h` while fixing v7:

```diff
-      #define HAS_IP5306
+      #define HAS_MAX1704X
+      #undef HAS_AXP2101
+      #undef HAS_IP5306
```

Those four targets were swept along with V7 and V7_1. Since v1.12.3 the probes
in `BatteryInterface::RunSetup()` are wrapped in `#ifdef HAS_IP5306` /
`#ifdef HAS_MAX1704X`, so `#undef HAS_IP5306` removes the 0x75 probe from the
build entirely. On an IP5306 board the firmware now only probes `MAX17048_ADDR`
(0x36), gets no ACK, and `i2c_supported` never becomes true.

Worth noting for anyone bisecting: this is invisible in a diff of v1.12.1
against current, because `HAS_IP5306` is undefined in *both*. v1.12.1 had no
per-chip `#ifdef` at all and simply probed both addresses. The guards arrived in
v1.12.3 while the macro was defined (harmless); v1.14.0 removed the macro and
left the guards.

## Why the obvious fix is wrong

Simply deleting `#define HAS_MAX1704X` and `#undef HAS_IP5306` from that branch
drops **KIT** through the ladder to the `#else // punt` fallback, where it ends
up with `HAS_IP5306` **and** `HAS_MAX1704X` **and** `HAS_AXP192` defined at once
— worse than today. Measured, not assumed (see below).

## This change

Split the branch. KIT keeps exactly what it has today. V4, V6 and V6.1 already
declare `HAS_IP5306` in their own board blocks (`configs.h:257` and `:278`), so
this just stops overriding them and lets the existing dedup ladder resolve the
rest.

No target's macro set changes except the three that are broken.

## Verification

Resolved with the real preprocessor, `gcc -E -dM -x c++ -D<TARGET>`, before and
after:

| target | before (master `91724fd`) | after |
|---|---|---|
| `MARAUDER_V4` | `HAS_MAX1704X` | **`HAS_IP5306`** |
| `MARAUDER_V6` | `HAS_MAX1704X` | **`HAS_IP5306`** |
| `MARAUDER_V6_1` | `HAS_MAX1704X` | **`HAS_IP5306`** |
| `MARAUDER_KIT` | `HAS_MAX1704X` | `HAS_MAX1704X` |
| `MARAUDER_V7` | `HAS_MAX1704X` | `HAS_MAX1704X` |
| `MARAUDER_V7_1` | `HAS_MAX1704X` | `HAS_MAX1704X` |

**V7 and V7_1 are untouched, so #1392's fix is preserved.**

Compiles clean for `MARAUDER_V6_1` (`esp32:esp32:d32:PartitionScheme=min_spiffs`,
core 3.3.4, CI's pinned libraries and `platform.txt` steps).

## Confirmed on hardware

I ran this fix on a Marauder v6.1 (HoneyHoney Double Barrel). Battery monitoring
is restored: `Battery Monitor: supported`, `Battery Lvl: 50%`, status-bar
percentage back.

To be precise about what was flashed: the hardware test used the same macro
outcome (`HAS_IP5306` alone for `MARAUDER_V6_1`) built from the **v1.15.1 tag**,
since that is what the device was running. This PR is that same change against
**master**, expressed as a branch split so KIT is not affected. Both builds come
out at 1,709,360 bytes but they are not byte-identical, because master is ahead
of the tag — so I am claiming the *fix* is hardware-confirmed, not this exact
commit's binary.

The reading is worth a second look — **50% is a quarter value**. The IP5306
branch of `getBatteryLevel()` returns only 25/50/75/100 because it maps two
register bits; a MAX17048's float `cellPercent()` would essentially never land on
a round quarter. So the number is evidence that the IP5306 path specifically is
what answers, not merely that battery code runs again. The same reasoning fits
the original report, which showed 75% on v1.12.1.

## Suggestion, separate from this PR

#1326 added `tools/check_battery_driver_macros.ps1` as a regression guard for
this exact class of bug. Nothing in the tree references it — no workflow,
Makefile or hook. Wiring it into CI would likely have caught #1392 before merge.

Fixes #1477
Fixes #1403
