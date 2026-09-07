Follow-up with the specific change that caused this, a correction to my own
earlier comments, and the reason the obvious one-line fix is not safe to merge.

### First, a correction

Both my earlier comments say `BatteryInterface::begin()`. **The method is
`RunSetup()`** — there is no `begin()` at either tag. My error; the reasoning
around it is unaffected.

### The change that broke it

**PR #1392, "Fix v7 battery and wardriving"** (merged 2026-07-20,
`f379cd59cb98`, shipped in v1.14.0). Its `configs.h` diff on the
V4/V6/V6_1/KIT branch is:

```diff
-      #define HAS_IP5306
+      #define HAS_MAX1704X
+      #undef HAS_IP5306
```

The PR was fixing v7, and flipped V4/V6/V6_1/KIT in the same edit.

### Why this survived four releases without being spotted

The regression is not visible in a diff of the two endpoint configs.
`HAS_IP5306` is undefined in **both** v1.12.1 and v1.15.1:

| version | macro | `#ifdef` guards in BatteryInterface | result |
|---|---|---|---|
| v1.12.1 | absent | **absent** — probes 0x75 and 0x36 unconditionally | works |
| v1.12.3 / v1.13.0 | `#define` | present | works |
| v1.14.0+ | `#undef` | present | **broken** |

It only exists in the interaction: guards were introduced in v1.12.3 while the
macro was defined (harmless), then v1.14.0 withdrew the macro and left the
guards. Each change was locally reasonable.

### The obvious fix is not safe — measured, not assumed

I preprocessed three patch variants for each target in the shared branch with
`gcc -E -dM -x c++ -D<TARGET>`:

| variant | V6_1 | KIT | V4 |
|---|---|---|---|
| stock v1.15.1 | MAX1704X | MAX1704X | MAX1704X |
| add `#define HAS_IP5306`, drop the `#undef` | IP5306 | IP5306 | IP5306 |
| delete both `#define HAS_MAX1704X` and `#undef HAS_IP5306` | IP5306 | **AXP192 + IP5306 + MAX1704X** | IP5306 |

The delete variant drops KIT through the ladder to the `#else // punt` fallback
and it ends up with three battery drivers compiled at once — worse than today.

The middle variant is what I am running locally and it works on my board, but it
flips **KIT and V4 to IP5306 as well**, which is wrong for any board in that
group that genuinely carries a MAX17048. That is the crux: the branch groups four
targets that evidently do not all use the same gauge, and the cleanup ladder then
forces exactly one driver for all of them.

Splitting the branch so each target resolves the IC its own board block already
declares looks like the minimal correct change, but you know the hardware mapping
and I do not.

### Also affected

**#1403 "battery indicator not showing up"** is the same bug — Clawzman (V6.1,
1.14.0) plus two commenters (V6 on 1.14.0, V6.1 on 1.14.1). Four reporters across
the two issues, all bisecting to v1.14.0. Worth linking them.

### One more thing, offered constructively

PR **#1326** (v1.13.0) was an earlier fix for this exact bug and added a guard,
`tools/check_battery_driver_macros.ps1`. Nothing in the tree references it — no
workflow, Makefile or hook (`grep -rn` for `.ps1|pwsh|powershell` returns zero
hits outside the file itself). Wiring it into CI would likely have caught #1392
before it merged.

### How I verified all of the above

Built v1.15.1 for `MARAUDER_V6_1` locally against the CI recipe (core 3.3.4,
pinned libraries, `User_Setup_og_marauder.h`, the `platform.txt` `-zmuldefs` and
`-fno-exceptions` steps). The unpatched control lands within 0.07% of the
published binary — 1,711,408 vs 1,710,224 bytes — with identical `app_desc`
fields, so the toolchain is faithful. String counts across the three binaries:

```
                    "Detected IP5306"   "Detected MAX17048"
official v1.15.1            0                   1
my stock control            0                   1
my patched build            1                   0
```

Happy to open a PR for whichever split you prefer.
