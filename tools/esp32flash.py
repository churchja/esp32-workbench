#!/usr/bin/env python3
"""
esp32flash.py -- guarded backup / restore / flash for ESP32 boards.

The governing rule: NO WRITE HAPPENS WITHOUT A VERIFIED BACKUP OF THAT
SPECIFIC BOARD.

"That specific board" means keyed by eFuse MAC, not by port and not by board
model. Ports are reassigned every replug; two boards of the same model are
physically different objects with different factory firmware. The MAC is the
only stable identity, so backups live under backups/<mac>/ and the gate looks
there and nowhere else.

Commands:
  backup    read the entire flash to a timestamped image + sha256 manifest
  verify    re-hash a stored backup and confirm it still matches its manifest
  list      show every backup held for every board
  restore   write a stored backup back to the chip
  flash     write a new image, but ONLY behind the gate
  erase     erase the chip, but ONLY behind the gate
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BACKUPS = os.path.join(REPO, "backups")
sys.path.insert(0, HERE)

from esp32ident import (Esptool, enumerate_ports, probe_silicon,  # noqa: E402
                        parse_app_desc, APP_DESC_OFFSET)

SIZE_RE = re.compile(r"^(\d+)\s*(B|KB|MB|GB)?$", re.I)
UNITS = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3}


# Read speed is a per-board property, not a constant.
#
# Measured on an ESP32-S3 over native USB (256KB reads):
#     115200 OK 23.5s | 230400 OK 12.4s | 460800 FAIL | 921600 FAIL
# Both failures were "Serial data stream stopped: Possible serial noise or
# corruption", reproducible in <6s. Writes tolerated 460800 fine, reads did
# not -- so a single default is wrong in one direction or the other. Try fast,
# step down on corruption, and record what worked.
BAUD_LADDER = (460800, 230400, 115200)

CORRUPTION_MARKERS = (
    "serial data stream stopped",
    "possible serial noise",
    "invalid head of packet",
    "timed out waiting for packet",
    # esptool 5.x emits this for a short read mid-transfer:
    #   "Corrupt data, expected 0x1000 bytes but received 0xff2 bytes"
    # It is a speed failure and was NOT recognised as one, so the ladder
    # classified it as "a real failure, not a speed one" and returned without
    # ever trying the next rung -- while the banner it had already printed
    # promised 460800/230400/115200 "in turn".
    #
    # Caught on the classic ESP32 (489d31027e98): 460800 failed with a
    # recognised marker, 230400 then failed with THIS one, and 115200 was never
    # attempted. Two consecutive runs failed at 0x07a000 and 0x326000 -- a
    # MOVING failure point, which is what distinguishes serial corruption from
    # a bad flash address, and the reason this belongs here.
    "corrupt data",
)


def looks_like_baud_failure(output):
    low = (output or "").lower()
    return any(m in low for m in CORRUPTION_MARKERS)


def read_with_fallback(esp, port, address, size, dest, timeout_for, baud=None,
                       after=None, clock=time.monotonic):
    """
    Read flash, stepping down the baud ladder on corruption-type failures.

    Returns (rc, output, baud_used, attempts). A non-corruption failure is
    returned immediately rather than retried -- a chip that will not answer at
    all is not going to answer slower.

    `attempts` is one dict per rung tried, in order:

        {"baud": 460800, "seconds": 5.3, "ok": False}
        {"baud": 230400, "seconds": 751.2, "ok": True}

    Each rung is timed SEPARATELY, which is the point. The caller used to wrap
    the whole ladder in one timer and record that as `read_seconds`, so a
    discarded 460800 attempt was silently added to the time attributed to the
    230400 read. That value was a valid upper bound but was presented as a
    measurement, and it inflated exactly the boards that fall back -- which in
    this repo is every board except the two USB-OTG S2s, the CH340 bridge
    included. Timing per rung also turns "the ladder fails fast" from a claim
    in a profile note into recorded data.

    `clock` is injectable so the timing is testable without sleeping. It
    defaults to time.monotonic, NOT time.time: these are durations, and
    time.time can step backwards when NTP corrects the system clock.

    `after` is passed to every attempt. It matters: esptool defaults to
    --after hard-reset, so a backup ends by rebooting the board. On a
    native-USB part that re-enumerates under a different identity on a
    different port path, and the NEXT command targets a port that no longer
    exists. That is exactly how an `idf.py flash` right after a backup failed
    with "could not open /dev/cu.usbmodem01".
    """
    ladder = [baud] if baud else list(BAUD_LADDER)
    attempts = []
    last = (1, "", None, attempts)
    for b in ladder:
        t0 = clock()
        rc, so, se = esp.run(port, "read-flash", str(address), str(size), dest,
                             timeout=timeout_for(b), baud=b, after=after)
        # Rounded at record time, not at report time, so that
        # ladder_seconds == sum(a["seconds"] for a in attempts) exactly.
        attempts.append({"baud": b, "seconds": round(clock() - t0, 1),
                         "ok": rc == 0})
        blob = so + se
        if rc == 0:
            return rc, blob, b, attempts
        last = (rc, blob, b, attempts)
        if not looks_like_baud_failure(blob):
            return last                      # a real failure, not a speed one
        if b != ladder[-1]:
            warn(f"{b} baud failed (serial corruption) after "
                 f"{attempts[-1]['seconds']:.0f}s; retrying at "
                 f"{ladder[ladder.index(b) + 1]}")
        if os.path.exists(dest):
            os.remove(dest)
    return last


def parse_size(text):
    m = SIZE_RE.match(str(text).strip())
    if not m:
        return None
    n = int(m.group(1))
    return n * UNITS.get((m.group(2) or "B").upper(), 1)


def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            b = fh.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def pick_port(explicit):
    if explicit:
        return explicit
    ports = enumerate_ports()
    if not ports:
        die("No USB serial device found. Plug the board in "
            "(and check the cable carries data, not just power).")
    if len(ports) > 1:
        warn(f"{len(ports)} boards attached; using {ports[0]['device']}. "
             f"Pass --port to choose.")
    return ports[0]["device"]


def die(msg, code=1):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def warn(msg):
    print(f"WARN: {msg}", file=sys.stderr)


def info(msg):
    print(msg, file=sys.stderr)


def board_identity(esp, port):
    """Return (mac, flash_bytes, chip). Everything downstream keys off this."""
    sil, _diag = probe_silicon(esp, port)
    if "_probe_error" in sil:
        die("Could not reach the chip:\n"
            + str(sil["_probe_error"]["value"])
            + "\n\nCommon causes: board is in run mode and needs BOOT held "
              "while you tap RESET; another program is holding the port "
              "(close any serial monitor); charge-only USB cable.")
    mac = sil.get("mac", {}).get("value")
    if not mac:
        die("Chip responded but no MAC was readable. Refusing to proceed: "
            "without a stable identity the backup gate cannot work.")
    size = parse_size(sil.get("flash_size", {}).get("value", ""))
    chip = sil.get("chip", {}).get("value", "unknown")
    return mac, size, chip


APP_PART_ALIGN = 0x10000   # ESP-IDF requires app partitions on 64KB boundaries


def app_sha_from_image(path):
    """Return the primary app's app_elf_sha256 from a full-flash image, or None.

    Read from the IMAGE, never the chip. The manifest describes an image, the
    descriptor is already inside it, and doing it offline means this also works
    on a backup taken years ago with the board long gone.

    Why it is worth recording at all: every OTHER field in esp_app_desc_t can be
    identical across completely different firmware. Reflashing a XIAO ESP32-S3
    on 2026-09-05 changed the app entirely while project_name, app_version,
    idf_version, build_date and build_time all stayed byte-for-byte the same --
    they describe the precompiled arduino-esp32 framework, not the application.
    The ELF hash was the only field that moved. A backup that cannot say which
    firmware it holds can only be verified as intact, not as correct.

    esp_app_desc_t sits APP_DESC_OFFSET into an app partition, and app
    partitions are 64KB-aligned, so scan those boundaries. Lowest offset wins:
    that is app0/ota_0, the partition that boots.
    """
    try:
        size = os.path.getsize(path)
    except OSError:
        return None
    want = APP_DESC_OFFSET + 256
    with open(path, "rb") as fh:
        for off in range(0, max(0, size - want) + 1, APP_PART_ALIGN):
            fh.seek(off)
            desc = parse_app_desc(fh.read(want))
            if desc:
                return desc["app_elf_sha256"]
    return None


def board_dir(mac):
    return os.path.join(BACKUPS, mac.replace(":", ""))


def manifest_path(mac):
    return os.path.join(board_dir(mac), "manifest.json")


def load_manifest(mac):
    p = manifest_path(mac)
    if not os.path.exists(p):
        return {"mac": mac, "backups": []}
    with open(p) as fh:
        return json.load(fh)


def save_manifest(mac, man):
    os.makedirs(board_dir(mac), exist_ok=True)
    with open(manifest_path(mac), "w") as fh:
        json.dump(man, fh, indent=2)


def verified_backups(mac):
    """Backups that exist on disk AND still hash to their recorded sha256."""
    man = load_manifest(mac)
    good = []
    for entry in man.get("backups", []):
        path = os.path.join(board_dir(mac), entry["file"])
        if not os.path.exists(path):
            continue
        if sha256_file(path) == entry.get("sha256"):
            good.append(entry)
    return good


# --------------------------------------------------------------------------
# The gate
# --------------------------------------------------------------------------

def require_backup(mac, chip, args, action, address=None, length=None):
    """
    Refuse a destructive operation unless a verified backup of THIS board
    exists. This is deliberately annoying. Bricking a board is more annoying.
    """
    good = verified_backups(mac)
    if good:
        newest = sorted(good, key=lambda e: e["created"])[-1]
        info(f"[gate] OK -- {len(good)} verified backup(s) for {mac}; "
             f"newest {newest['file']} ({newest['created']})")
    else:
        if not args.no_backup_i_accept_the_risk:
            die(
                f"BLOCKED: no verified backup exists for board {mac}.\n\n"
                f"  About to: {action}\n"
                f"  Board:    {chip} @ {mac}\n\n"
                f"Take one first:\n"
                f"  python3 tools/esp32flash.py backup --port {args.port or '<port>'}\n\n"
                f"A full backup of an 8MB board takes about 3 minutes at the "
                f"default 460800 baud (measured 12 min at 115200), and is "
                f"the only thing standing between you and an unrecoverable "
                f"factory image. If you genuinely do not want it, pass\n"
                f"  --no-backup-i-accept-the-risk\n"
                f"which exists to be typed deliberately, not reached for by habit.",
                code=4)
        warn(f"Proceeding WITHOUT a backup of {mac} because "
             f"--no-backup-i-accept-the-risk was passed. "
             f"The factory firmware will be unrecoverable.")

    # Second gate: confirmation naming the board and the exact range.
    if not args.yes:
        rng = ""
        if address is not None:
            rng = f"  Range:    {address}" + (f" .. +{length}" if length else "")
        die(
            f"CONFIRMATION REQUIRED\n"
            f"  Action:   {action}\n"
            f"  Board:    {chip} @ {mac}\n"
            f"  Port:     {args.port}\n"
            + (rng + "\n" if rng else "") +
            f"\nRe-run with --yes once you have read the above and it names "
            f"the board you actually meant.",
            code=5)


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def report_board_state(after, port):
    """
    State the board is in now, and what that means for the NEXT command.

    This is the information whose absence caused a real failure: a backup ended
    with esptool's default hard-reset, the board rebooted and re-enumerated
    under a different VID/PID on a different path, and the flash that followed
    opened a port that had ceased to exist.
    """
    if after == "no-reset":
        info(f"Board left in DOWNLOAD MODE on {port} -- the next flash or dump "
             f"can use it directly. Tap RESET to run its firmware again. Do not "
             f"leave it parked indefinitely; a resident stub can drop USB.")
    else:
        info(f"Board was RESET and is running its firmware again.")
        info(f"  If the SoC provides USB itself (S2/S3/C3/C6/C5/H2/P4), it "
             f"re-enumerates and {port} may no longer exist -- re-check with "
             f"tools/usbwatch.py --once, or use --after no-reset to keep it "
             f"parked in download mode.")
        info(f"  Behind a UART bridge (CH340/CP210x/FTDI) the port is stable: "
             f"the bridge stays enumerated regardless of what the SoC does.")


def cmd_backup(esp, args, clock=time.monotonic):
    port = args.port
    mac, size, chip = board_identity(esp, port)
    if not size:
        die("Flash size could not be detected; refusing to guess. "
            "Pass --size (e.g. --size 8MB).")
    if args.size:
        size = parse_size(args.size)

    os.makedirs(board_dir(mac), exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    name = f"full-{stamp}-{size}.bin"
    dest = os.path.join(board_dir(mac), name)

    # Derive the timeout instead of guessing it.
    #
    # Measured on an ESP32-S3 over native USB: 8MB at 115200 baud took >12 min,
    # against a fixed 900s timeout -- a ~20% margin. A slightly slower board or
    # a 16MB flash would have blown it, and esptool buffers the whole read in
    # memory, so a timeout destroys the work with no partial file to salvage.
    #
    # Serial framing is 10 bits per byte, so bytes/(baud/10) is the floor.
    # 3x that plus 60s of handshake covers stub upload, retries and overhead.
    def timeout_for(b):
        # Serial framing is 10 bits/byte; 3x plus handshake covers stub upload
        # and retries. Derived, not guessed -- a fixed 900s once came within
        # ~2 min of destroying a 731s read with no partial file to salvage.
        return max(args.timeout, int(size / (b / 10.0) * 3 + 60))

    if args.baud:
        info(f"Reading {size/1048576:.0f}MB from {chip} @ {mac} at "
             f"{args.baud} baud (pinned)")
    else:
        info(f"Reading {size/1048576:.0f}MB from {chip} @ {mac}; trying "
             f"{'/'.join(str(b) for b in BAUD_LADDER)} baud in turn")
    info("Do not unplug.")
    rc, blob, baud_used, attempts = read_with_fallback(
        esp, port, 0, size, dest, timeout_for, baud=args.baud,
        after=args.after, clock=clock)
    so, se = blob, ""
    if rc != 0:
        if os.path.exists(dest):
            os.remove(dest)
        hint = ""
        if rc == 124:
            hint = (f"\n\nThe read timed out. esptool buffers "
                    f"the whole image in memory, so there is no partial file to "
                    f"keep. Retry faster:\n"
                    f"  python3 tools/esp32flash.py backup --baud 921600\n"
                    f"or allow more time with --timeout.")
        die("read-flash failed:\n" + (so + se)[-1500:] + hint)

    # read_seconds is the SUCCESSFUL rung alone. ladder_seconds is the wall
    # time the operator actually waited, discarded attempts included. They
    # differ whenever the ladder steps down, and conflating them is what this
    # split exists to stop. Entries written before this change have no
    # "attempts" key -- for those, read_seconds is whole-ladder time.
    transfer = next(a["seconds"] for a in attempts if a["ok"])
    ladder_seconds = round(sum(a["seconds"] for a in attempts), 1)
    discarded = [a for a in attempts if not a["ok"]]

    digest = sha256_file(dest)
    actual = os.path.getsize(dest)
    if actual != size:
        warn(f"expected {size} bytes, got {actual}")

    man = load_manifest(mac)
    man["mac"] = mac
    man["chip"] = chip
    man.setdefault("backups", []).append({
        "file": name, "sha256": digest, "bytes": actual,
        # sha256 proves the image is INTACT; app_elf_sha256 says WHICH firmware
        # it holds. None when the image carries no app descriptor (a blank or
        # bootloader-only chip), which is itself worth recording.
        "app_elf_sha256": app_sha_from_image(dest),
        "created": datetime.now(timezone.utc).isoformat(),
        "chip": chip, "read_seconds": transfer,
        "ladder_seconds": ladder_seconds,
        "attempts": attempts,
        "baud": baud_used,
        "esptool_version": esp.version,
    })
    save_manifest(mac, man)

    info(f"Backup complete: {transfer:.0f}s at {baud_used} baud")
    if discarded:
        info("  ladder: " + ", ".join(
            f"{a['baud']} failed after {a['seconds']:.0f}s" for a in discarded)
            + f" -> {ladder_seconds:.0f}s wall total")
    report_board_state(args.after, port)
    print(dest)
    print(f"sha256 {digest}")
    return 0


def cmd_verify(esp, args):
    if args.mac:
        macs = [args.mac]
    elif os.path.isdir(BACKUPS):
        # Skip dotfiles -- macOS drops .DS_Store into every directory it opens,
        # and it is not a MAC address.
        macs = [d for d in os.listdir(BACKUPS)
                if not d.startswith(".")
                and os.path.isdir(os.path.join(BACKUPS, d))]
    else:
        macs = []
    if not macs:
        info("No backups stored yet.")
        return 0
    bad = 0
    for raw in macs:
        mac = raw if ":" in raw else ":".join(
            raw[i:i + 2] for i in range(0, len(raw), 2))
        man = load_manifest(mac)
        for e in man.get("backups", []):
            path = os.path.join(board_dir(mac), e["file"])
            if not os.path.exists(path):
                print(f"MISSING  {mac}  {e['file']}")
                bad += 1
                continue
            ok = sha256_file(path) == e.get("sha256")
            print(f"{'OK      ' if ok else 'CORRUPT '} {mac}  {e['file']}  "
                  f"{e['bytes']} bytes  {e['created']}")
            if not ok:
                bad += 1
    return 1 if bad else 0


def cmd_list(esp, args):
    if not os.path.isdir(BACKUPS) or not os.listdir(BACKUPS):
        print("No backups yet. Take one before flashing anything:")
        print("  python3 tools/esp32flash.py backup")
        return 0
    for d in sorted(os.listdir(BACKUPS)):
        p = os.path.join(BACKUPS, d, "manifest.json")
        if not os.path.exists(p):
            continue
        with open(p) as fh:
            man = json.load(fh)
        print(f"{man.get('mac', d)}  {man.get('chip', '?')}")
        for e in man.get("backups", []):
            print(f"    {e['created']}  {e['file']}  {e['bytes']} bytes")
    return 0


def cmd_restore(esp, args):
    port = args.port
    mac, _size, chip = board_identity(esp, port)
    good = verified_backups(mac)
    if not good:
        die(f"No verified backup exists for {mac}; nothing to restore.")
    entry = next((e for e in good if e["file"] == args.file), None) if args.file \
        else sorted(good, key=lambda e: e["created"])[-1]
    if not entry:
        die(f"Backup {args.file} not found or failed verification for {mac}.")
    path = os.path.join(board_dir(mac), entry["file"])

    if not args.yes:
        die(f"CONFIRMATION REQUIRED\n"
            f"  Action:   restore factory image, overwriting current firmware\n"
            f"  Board:    {chip} @ {mac}\n"
            f"  Image:    {entry['file']} ({entry['bytes']} bytes)\n"
            f"  Port:     {port}\n\nRe-run with --yes.", code=5)

    info(f"Restoring {entry['file']} to {chip} @ {mac}")
    # after= is not optional. Omitting it lets esptool apply its OWN default of
    # hard-reset, so --after no-reset is accepted on the command line and then
    # silently discarded -- the board reboots, its firmware runs, and it writes
    # to nvs. That is not hypothetical: it put 65 bytes into a Satellite1's nvs
    # during a restore verification that had explicitly asked for no-reset, and
    # the delta looked like firmware behaviour until the code was read. An
    # ignored option is worse than an absent one.
    rc, so, se = esp.run(port, "write-flash", "0x0", path,
                         timeout=args.timeout, baud=args.baud, after=args.after)
    if rc != 0:
        die("write-flash failed:\n" + (so + se)[-1500:])
    info("Restore complete.")
    report_board_state(args.after, port)
    return 0


def cmd_flash(esp, args):
    port = args.port
    mac, _size, chip = board_identity(esp, port)
    if not os.path.exists(args.image):
        die(f"Image not found: {args.image}")
    require_backup(mac, chip, args,
                   action=f"write {os.path.basename(args.image)} to flash",
                   address=args.address,
                   length=f"{os.path.getsize(args.image)} bytes")
    # Same contract as restore: forward both. Dropping baud here also meant
    # --baud was silently ignored on every flash.
    rc, so, se = esp.run(port, "write-flash", args.address, args.image,
                         timeout=args.timeout, baud=args.baud, after=args.after)
    if rc != 0:
        die("write-flash failed:\n" + (so + se)[-1500:])
    info("Flash complete.")
    report_board_state(args.after, port)
    return 0


def cmd_erase(esp, args):
    port = args.port
    mac, _size, chip = board_identity(esp, port)
    require_backup(mac, chip, args, action="ERASE THE ENTIRE FLASH")
    rc, so, se = esp.run(port, "erase-flash", timeout=args.timeout,
                         baud=args.baud, after=args.after)
    if rc != 0:
        die("erase-flash failed:\n" + (so + se)[-1500:])
    info("Erase complete. The board now has no firmware and will not boot "
         "until something is written.")
    # An erased USB-OTG part (S2) cannot bring USB up at all -- there is no
    # firmware left to do it -- so it vanishes entirely and needs BOOT+RESET.
    # Measured three times on a Flipper Zero Wi-Fi Module. Saying so here is
    # the difference between an expected outcome and an apparent brick.
    report_board_state(args.after, port)
    return 0


def main():
    # Shared flags are attached to BOTH the main parser and every subparser so
    # they work on either side of the subcommand. argparse would otherwise
    # reject `... flash x.bin --yes`, which is how everyone types it -- a bad
    # failure on a safety confirmation, since it trains retry-until-it-works.
    # SUPPRESS defaults keep an unspecified subparser copy from clobbering a
    # value given before the subcommand.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", default=argparse.SUPPRESS)
    common.add_argument("--timeout", type=int, default=argparse.SUPPRESS)
    common.add_argument("--after", default=argparse.SUPPRESS,
                        choices=("hard-reset", "no-reset"),
                        help="what to do when the operation finishes. "
                             "hard-reset (default) returns the board to running "
                             "its firmware but re-enumerates it, changing the "
                             "port; no-reset parks it in download mode so the "
                             "next flash can use the same port.")
    common.add_argument("--baud", type=int, default=argparse.SUPPRESS,
                        help="pin a serial rate; default is to try "
                             "460800/230400/115200 and use the first that works")
    common.add_argument("--yes", action="store_true", default=argparse.SUPPRESS,
                        help="confirm a destructive operation")
    common.add_argument("--no-backup-i-accept-the-risk", action="store_true",
                        default=argparse.SUPPRESS,
                        help="bypass the backup gate (deliberately verbose)")

    ap = argparse.ArgumentParser(
        description=__doc__, parents=[common],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.set_defaults(port=None, timeout=1800, baud=None, after='hard-reset', yes=False,
                    no_backup_i_accept_the_risk=False)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("backup", parents=[common]); b.add_argument("--size")
    sub.add_parser("list", parents=[common])
    v = sub.add_parser("verify", parents=[common]); v.add_argument("--mac")
    r = sub.add_parser("restore", parents=[common]); r.add_argument("--file")
    f = sub.add_parser("flash", parents=[common])
    f.add_argument("image"); f.add_argument("--address", default="0x10000")
    sub.add_parser("erase", parents=[common])

    args = ap.parse_args()
    esp = Esptool()
    if not esp.exe:
        die("esptool not found. Install: pip install esptool")

    if args.cmd in ("list", "verify"):
        return {"list": cmd_list, "verify": cmd_verify}[args.cmd](esp, args)

    args.port = pick_port(args.port)
    return {
        "backup": cmd_backup, "restore": cmd_restore,
        "flash": cmd_flash, "erase": cmd_erase,
    }[args.cmd](esp, args)


if __name__ == "__main__":
    sys.exit(main())
