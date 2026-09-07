# microSD copy — screen unit (MAC 48:9D:31:02:A9:48)

Taken 2026-09-07T06:27:14Z from the card the screen unit reports as
"SD Card: Connected / SD Card Size: 29820MB" on its own Device Info page.

    Volume name    MARAUDER
    Filesystem     MS-DOS FAT32
    Disk size      31,264,342,016 bytes (31.3 GB / 29 GiB)
    Used           1,998,848 bytes reported by diskutil
    Mounted        read-write (see the caveat below)

## What is on it

Nothing. The complete contents, excluding macOS-generated metadata:

    ./
    ./SCRIPTS      <- directory, empty

Zero bytes of files. `SCRIPTS` is preserved here by a .gitkeep because git
cannot track an empty directory; the .gitkeep is ours, it was not on the card.

The ~2 MB `diskutil` reports as used is `.Spotlight-V100` and `.fseventsd`,
which macOS wrote when the card was inserted. It is not Marauder data and is
deliberately excluded from this copy.

## Why this copy exists even though it is empty

"The card was empty on 2026-09-07" is a fact worth being able to prove later.
Without this, a future session finding an empty card cannot tell whether it was
always empty or was wiped, and that difference matters if this unit is ever
suspected of having lost data.

It also corrects a claim made twice in boards/489d31027e98.yaml before anyone
looked: that this card held the unit's settings, captures and portal assets and
was therefore the backup worth taking. It held none of them. The unit's settings
are evidently in on-chip NVS, which is behind the same missing USB path as
everything else on that board.

## Caveat on fidelity

macOS writes to removable media on mount, unprompted, and it had already done so
before this copy was taken. This is a faithful record of the FILES, not a
forensic image of the card. A byte-level image would have had to be captured
before macOS ever saw it. Here that costs nothing, because the card was blank.
