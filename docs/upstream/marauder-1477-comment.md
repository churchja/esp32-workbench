<!--
POSTED 2026-09-07T06:53:27Z by churchja as a comment on upstream issue #1477:
https://github.com/justcallmekoko/ESP32Marauder/issues/1477#issuecomment-5566281043
Kept here as the source of what was sent. Edit the upstream comment, not this
file, if it needs changing -- this is a record, not the live copy.
-->

Root cause, with a bisect. This is a config regression introduced in **v1.14.0**,
and it matches your report exactly ("works up to 1.14.0 beta, not from 1.14.0
onwards").

### The change

`esp32_marauder/configs.h`, in the `#ifdef HAS_BATTERY` section:

**v1.13.0 — working**
```c
#elif defined(MARAUDER_V4) || defined(MARAUDER_V6) || defined(MARAUDER_V6_1) || defined(MARAUDER_KIT)
  #define I2C_SDA 33
  #define I2C_SCL 22
  #define HAS_IP5306
```

**v1.14.0 through v1.15.1 and current master (line 2854) — broken**
```c
#elif defined(MARAUDER_V4) || defined(MARAUDER_V6) || defined(MARAUDER_V6_1) || defined(MARAUDER_KIT)
  #define I2C_SDA 33
  #define I2C_SCL 22
  #define HAS_MAX1704X
  #undef HAS_AXP2101
  #undef HAS_IP5306      // <-- this
```

`BatteryInterface::begin()` wraps each probe in `#ifdef HAS_IP5306` /
`#ifdef HAS_MAX1704X`, and those guards are present in both versions — so the
guards aren't the change, the config is. With `HAS_IP5306` undefined, the
IP5306 probe is not compiled in at all. The firmware only ever probes
`MAX17048_ADDR` (0x36), finds nothing on an IP5306 board, leaves
`i2c_supported = false`, and reports **"Battery Monitor: not supported"**.

Note that in v1.12.1 `begin()` probed both `0x75` and `0x36` unconditionally,
which is why boards with either chip worked.

### Bisect

| tag | `#define HAS_IP5306` | `#undef HAS_IP5306` + `HAS_MAX1704X` |
|---|---|---|
| v1.12.3 | yes | no |
| v1.13.0 | yes | no |
| **v1.14.0** | **no** | **yes** |
| v1.15.1 / master | no | yes |

### Data point

HoneyHoney Lab "Double Barrel", `Hardware: Marauder v6.1`:

- `v1.12.1` → `Battery Monitor: supported`, `Battery Lvl: 75%`
- `v1.15.1` → `Battery Monitor: not supported`, no level, no status-bar percentage

Same board, same SD update path, nothing else changed. The IP5306 is still on
the bus at `0x75`; the firmware just no longer looks for it.

### Suggested fix

Defining **both** for that group restores v1.12.1 behaviour without breaking
MAX17048 boards, since `begin()` probes each address and only sets
`i2c_supported` on an ACK:

```c
#elif defined(MARAUDER_V4) || defined(MARAUDER_V6) || defined(MARAUDER_V6_1) || defined(MARAUDER_KIT)
  #define I2C_SDA 33
  #define I2C_SCL 22
  #define HAS_IP5306
  #define HAS_MAX1704X
  #undef HAS_AXP2101
```

That group covers several vendors' boards and they don't all use the same fuel
gauge, so probing for both seems safer than picking one.

### Possibly relevant

#1326 ("Fix v6 IP5306 battery driver selection", merged 2026-06-22) describes
its intent as *"Keep `HAS_IP5306` as an explicit battery-driver cleanup case for
`MARAUDER_V6` and `MARAUDER_V6_1` builds. Disable unrelated AXP/MAX battery
drivers when IP5306 is selected."* The code currently in master does the
inverse for that group — keeps MAX and undefines IP5306. Worth a look in case
the branch landed inverted.
