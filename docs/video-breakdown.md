# Source breakdown: "Claude Code Turned $20 Gadgets Into Tiny Computers"

**Creator Magic** (Mike) · published 2026-09-02 · 22:56 · 16 chapters
<https://www.youtube.com/watch?v=Zvb01TRY6UY>

Captured from the video's own auto-captions and chapter markers, fetched
2026-09-04. This is a technical summary written to extract design requirements
— not a transcript. Watch the original for the actual thing.

> **Provenance: `community` tier. Do not treat the technical specifics here as
> verified.**
>
> Everything below is derived from machine-generated captions, which
> demonstrably mangled proper nouns in this video — *Paphos* became "pathos",
> *Micro Center* became "MicroEnter", *Espressif* became "Expressive". I
> corrected those silently for readability, which means other errors may have
> survived the same way.
>
> So specific values — part numbers (`ESP32-D0WD-V3`, `ESP32-S3-PICO-1`),
> memory sizes, firmware sizes (376KB, 434KB, 813KB), frequencies (156Hz,
> 328Hz) — are **caption-derived and unverified against the hardware or vendor
> documentation**. Under this repo's own rules they are `community` tier at
> best. Do not copy them into a board profile as `vendor_doc`, and do not build
> firmware against them without probing the actual board.
>
> The *workflow* observations in Part 2 are robust to transcription error; the
> *numbers* in Part 1 are not.

---

## Part 1 — What happens, beat by beat

### Board one · Waveshare ESP32-C6-LCD-1.47 · ~$20, Micro Center
*Chapters 00:43–09:00*

| Beat | What occurs |
|---|---|
| 01:59 | Unboxed. Thumb-sized, 1.47" colour LCD, USB-C. Plugged straight into a Mac Studio |
| 02:50 | Factory demo runs on its own: RGB LED animation, on-board specs, a Wi-Fi scan |
| 03:18 | A directory is made and Claude Code started with permission checks skipped |
| 03:31 | **First prompt is deliberately read-only** — "can you see it? don't do anything with it yet" |
| 03:43 | Claude enumerates USB serial devices, names the *Espressif USB JTAG/serial debug unit*, notes the board sits behind a 4-port USB 2.0 hub, and that flashing and monitoring share the port |
| 04:17 | Second prompt: "what's flashed on it right now?" |
| 04:23 | Chip and flash ID via esptool → **ESP32-C6FH8** confirmed |
| 04:34 | Partition table dumped and parsed |
| 04:50 | App image pulled and mined for strings → identifies it as a **vendor factory test demo sketch**, Arduino-based, cross-checked against the boot log |
| 05:25 | Prompt for a project idea, with a hard constraint: **no Wi-Fi** |
| 05:49 | Claude finds live sea-surface temperature and wave data from Open-Meteo, then flags the contradiction: the data is online, the board is not |
| 06:05 | **Claude asks a clarifying question** — which board variant is this? Mike reads the silkscreen and confirms |
| 06:23 | Pin map assembled, firmware written |
| 06:38 | Compiles clean. **Factory image 1.87MB** (Wi-Fi + BlueDroid + NimBLE) vs **new build 376KB**, 11% of the app partition — no radios at all |
| 06:53 | **Claude backs up the factory firmware before overwriting**, unprompted |
| 07:03 | Flashed — and the display comes up **misaligned, with the old image bleeding through** |
| 07:20 | Claude states the limit plainly: *"I've got no eyes on the panel. Does it look right?"* |
| 07:31 | **Mike photographs the screen with his iPhone, AirDrops it, pastes it into the chat** |
| 08:09 | From the photo alone: **shifted by 34 pixels — the classic signature of a row offset**. Fixed and reflashed |
| 08:51 | Works. Unplugged, replugged — firmware persists and resumes |

### Board two · ESP32-2432S028R "Cheap Yellow Display" · ~$18
*Chapters 09:21–15:11*

| Beat | What occurs |
|---|---|
| 09:57 | Same USB-C cable, new board. Prompt asks what it is **and what it can do that the last one couldn't** |
| 10:23 | **The USB identity changed** — native Espressif JTAG is gone, a **CH340 UART bridge** is in its place. Claude reads the meaning: this board has no native USB, so a separate chip does the serial conversion |
| 10:43 | Identified as **ESP32-D0WD-V3**: DACs, touch pins, Ethernet MAC, resistive touchscreen, bigger display |
| 11:19 | Claude proposes a **Bluetooth audio spectrum analyser** — and the reasoning is the point: it picked a project the *previous* board physically could not run, because the C6's Bluetooth carries no audio |
| 11:52 | Compiles first time. Claude notes Bluetooth Classic is bulky and re-checks the backup |
| 12:02 | **Backup takes 3–4 minutes and is verified before any write** |
| 12:34 | Flashed. Appears on iPhone Bluetooth as `ESP32-Spectrum`, pairs |
| 12:49 | Displays the currently playing track metadata, then live spectrum from Spotify. Play/pause drives it in real time |
| 13:50 | Touchscreen works with stylus and finger |

### Board three · Waveshare ESP32-S3-ePaper-1.54 · ~$26
*Chapters 15:11–22:35*

| Beat | What occurs |
|---|---|
| 15:23 | Plugged in. E-paper shows temperature and humidity — **and keeps showing it after unplugging**, since ink needs power only to change |
| 16:10 | Identified as **ESP32-S3-PICO-1**: dual-core 240MHz, 8MB embedded flash, 8MB embedded PSRAM |
| 16:24 | Claude keeps digging past the chip and finds the *peripherals*: audio codec in and out, microphone, speaker, power amp, Wi-Fi, BLE, microSD, RTC with alarm, buttons — and a live temperature/humidity sensor it reads on the spot |
| 17:12 | Mike asks for **three ideas** before building anything |
| 17:31 | Proposals: *Sonic Polaroid* (button → 5s listen → develop a print), *The Room's Diary* (24h battery-powered portrait, deep sleep), *Paper Voice* (speech → transcription → typeset on ink). Each uses a different subset of the discovered hardware |
| 18:29 | **Claude searches GitHub, finds Waveshare's own demo repo**, and works backwards from it |
| 18:40 | Compiles at 434KB. Backs up first, as before |
| 19:04 | Works. Microphone captures real audio peaks; a bug is found and fixed |
| 20:01 | **Second photo pasted in.** Claude's assessment is notably not flattering: the print works but is *"mushier than it should be."* It reports the detected peaks (156Hz, 328Hz) and spots from the image that the caption is running vertically **because Mike is holding the board with the cable to the right** |
| 20:48 | Rebuilt at 813KB (24% of partition) with voice playback and corrected frequency mapping |
| 21:12 | It speaks back |

---

## Part 2 — What this framework took from it

### Kept

**Read-only reconnaissance first.** The opening prompt explicitly forbade
action. That ordering is why nothing was destroyed. → `esp32ident.py` cannot
write; the read and write paths are separate programs.

**Layered identification.** USB descriptor → chip ID → flash ID → partition
table → app image strings → boot log, each narrowing the next. → the six-stage
pipeline in `identification.md`.

**USB identity is a capability statement.** Noticing the CH340 replaced native
JTAG told Claude something real about the silicon before it read the chip ID at
all. → stage 1 records `usb_interface_kind` as native vs bridge, with the
consequences spelled out.

**Backup before overwrite, unprompted.** Done on all three boards. → promoted
from good habit to enforced gate, keyed by eFuse MAC.

**Size as a diagnostic.** 1.87MB → 376KB was not trivia; it proved the radios
were genuinely excluded. → `flashing.md` treats an unexpectedly large build as
a signal that an unused stack got linked in.

**Ask when the answer is on the silkscreen.** Claude stopped and asked which
board variant it was rather than guessing. → the research protocol requires
asking the human for what only the human can see.

**Photograph the screen.** The highest-leverage move in the whole video, twice.
A 34-pixel row offset and a rotation-vs-grip mismatch were both diagnosed from
photographs, from faults that would be near-invisible in source. → a first-class
workflow with a signature table in `troubleshooting.md`.

**Propose options before building.** Three ideas, each exercising a different
part of the discovered hardware, chosen before a line was written.

### Changed

**Knowledge was thrown away.** Every board was re-derived from scratch, and
none of it survived the session. Plug the same board in next week and you pay
the discovery cost again — possibly getting a different answer, with nothing
recording which pin map actually worked. → **persistent profiles keyed by MAC**,
which is the single biggest change here.

**Provenance was flattened.** A pin read from eFuse and a pin inferred from a
vendor repo were reported in the same confident voice. On hardware those carry
very different risk. → **seven provenance levels**, and pins stay `unverified`
until a physical test promotes them.

**The backup was a habit, not a guarantee.** It happened because the model
chose to. → an enforced gate that refuses writes and has to be overridden by a
deliberately verbose flag.

**`--dangerously-skip-permissions` was on.** Reasonable for a demo. Not
reasonable as a default for a tool that erases flash. → the gate lives inside
the tool, so it holds regardless of permission mode.

**Commands were version-blind.** esptool 5.x renamed every subcommand;
tutorials and transcripts all use the 4.x spelling. → runtime dialect detection.
