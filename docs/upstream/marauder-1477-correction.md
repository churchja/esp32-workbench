<!--
POSTED 2026-09-07T07:34:52Z by churchja on upstream issue #1477:
https://github.com/justcallmekoko/ESP32Marauder/issues/1477#issuecomment-5566933618
Retracts the suggested fix in issue-comment-5566281043 (the diagnosis there
stands). Record of what was sent; edit upstream, not this file.
-->

Correction to my previous comment — the fix I suggested does not work, and I
want to retract it before anyone acts on it.

I suggested defining both `HAS_IP5306` and `HAS_MAX1704X` for the
V4/V6/V6_1/KIT group, on the reasoning that `begin()` probes both addresses and
sets `i2c_supported` on whichever ACKs. That reasoning ignores the cleanup chain
further down `configs.h` (~line 2931):

```c
//  If we know what we have, we can delete what we're not using
#ifdef BATTERY_ADC_PIN
  ...
#elif defined(HAS_TP4057)
  ...
#elif defined(HAS_IP5306)
  #undef HAS_MAX1704X
  #undef HAS_AXP192
```

Defining `HAS_IP5306` **undefines** `HAS_MAX1704X`. So "define both" collapses to
IP5306 only — it would fix IP5306 boards by breaking MAX17048 ones. Same bug,
opposite direction.

I confirmed this empirically rather than by reading alone. Building v1.15.1 for
`MARAUDER_V6_1` with only that one line added:

```
                    size        "Detected IP5306"   "Detected MAX17048"
official v1.15.1    1,710,224           0                   1
my stock build      1,711,408           0                   1
my patched build    1,709,360           1                   0
```

The patched build is *smaller* despite adding a code path, and the MAX17048
string is gone — the Adafruit_MAX1704X library dropped out entirely.
(The stock build reproduces the official binary's battery strings and lands
within 0.07% on size, so the toolchain is faithful; the residual delta is
embedded build paths.)

**So the real problem is the grouping, not the macro.** V4, V6, V6.1 and KIT are
lumped into one branch, and the cleanup chain then forces exactly one driver for
all of them. If boards in that group genuinely ship different fuel gauges — mine
has an IP5306, and the change in v1.14.0 implies others have a MAX17048 — then
no single choice for that branch can be correct.

Two directions that would actually work, both for you to judge:

1. Split the group, so V6/V6_1 select `HAS_IP5306` and whichever targets need
   MAX17048 select that. Correct if driver choice is fixed per target.
2. Allow both probes for this group specifically, which is what v1.12.1 did — its
   `begin()` had no per-chip `#ifdef` and simply tried 0x75 then 0x36. That
   version worked on both kinds of board, which is evidence the group is mixed.

I have no visibility into which vendors' boards map to which target, so I can't
tell which is right — but option 2 is what the working version did.

Apologies for the noise on the first suggestion. The diagnosis in it still
stands; only the proposed fix was wrong.
