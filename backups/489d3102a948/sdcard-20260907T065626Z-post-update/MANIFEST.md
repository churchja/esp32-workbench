# microSD, second capture — after the v1.15.1 update

Taken 2026-09-07, from the same card as sdcard-20260907T062654Z (which was empty).

## What is new since the first capture

    ./pingscan_0.log    26 bytes
    ./sshscan_0.log    111 bytes
    ./sshscan_1.log    111 bytes

Written by the device, not by us. Timestamps read "Dec 31 1979" — the FAT epoch
default, which is what a device with no RTC writes. That is a useful side fact:
this unit has no real-time clock, so nothing on this card can be dated from its
filesystem metadata.

All three are empty stubs. Contents in full:

    Starting Ping Scan with...

    Starting Port Scan with...
    SSID:
    IP address: 0.0.0.0
    Gateway: 0.0.0.0
    Netmask: 0.0.0.0
    MAC: 00:00:00:00:00:00

Zeros throughout — the scans were started with no Wi-Fi association, so nothing
was captured. Checked BEFORE copying, because this repo is public and scan logs
are exactly the kind of artifact that carries SSIDs, local IP ranges and MACs.
They did not here. That check is not optional next time just because it came
back clean this time.

## What is excluded

`update.bin` is not in this copy. It is the v1.15.1 image already held byte-
identical at ../upstream-marauder-v1.15.1-v6_1.bin, so archiving it again would
store 1.7MB twice for no gain.

On the card it was renamed to `update.bin.applied-v1.15.1` rather than deleted.
That achieves the actual goal — `SDInterface::runUpdate` looks for exactly
`/update.bin` and will now report "Could not load update.bin from sd root", so a
stray SD Update cannot re-apply it — while destroying nothing.
