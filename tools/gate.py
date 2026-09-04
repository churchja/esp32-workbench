"""
The backup-gate decision, as a pure function.

Kept separate from the PlatformIO extra script deliberately: an extra script
runs under SCons and cannot be imported outside it, which would make the most
safety-critical logic in the repo untestable. Everything that decides lives
here; the extra script only does I/O and calls in.
"""

import os

ALLOW = "allow"
BLOCK = "block"

BANNER_WIDTH = 74


def find_repo_root(start, marker=os.path.join("tools", "esp32flash.py"), levels=8):
    """Walk up from `start` looking for the workbench. None if outside it."""
    d = os.path.abspath(start)
    for _ in range(levels):
        if os.path.isfile(os.path.join(d, marker)):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def evaluate(root, mac, chip, port, backups, bypass=False, esptool_found=True):
    """
    Decide whether an upload may proceed.

    backups -- list of verified backup manifest entries for THIS board's MAC.
               Empty list means no recoverable copy exists.

    Returns (verdict, lines, char) where verdict is ALLOW or BLOCK.
    """
    if bypass:
        return ALLOW, [
            "BACKUP GATE BYPASSED  (ESP32_NO_GATE=1)",
            "Whatever is on this board is about to be overwritten with no",
            "recoverable copy. Factory firmware is usually not redistributed.",
        ], "!"

    if root is None:
        return ALLOW, [
            "BACKUP GATE INACTIVE",
            "",
            "This project sits outside the esp32-workbench repo, so the gate",
            "cannot locate tools/esp32flash.py and cannot verify a backup.",
            "Proceeding unguarded.",
        ], "!"

    if not esptool_found:
        return ALLOW, [
            "BACKUP GATE INACTIVE: esptool not found on PATH.",
            "Install it (pip install esptool) so backups can be verified.",
        ], "!"

    if backups:
        newest = sorted(backups, key=lambda e: e["created"])[-1]
        return ALLOW, [
            f"[backup-gate] OK  {chip} @ {mac} -- "
            f"{len(backups)} verified backup(s), newest {newest['file']}"
        ], None

    rel = os.path.join(root, "tools", "esp32flash.py")
    try:
        rel = os.path.relpath(rel)
    except ValueError:
        pass
    return BLOCK, [
        "UPLOAD BLOCKED -- no verified backup for this board",
        "",
        f"  Board : {chip}",
        f"  MAC   : {mac}",
        f"  Port  : {port}",
        "",
        "  Take one first (~3 min for 8MB at 460800 baud):",
        f"    python3 {rel} backup --port {port}",
        "",
        "  Or bypass deliberately:",
        "    ESP32_NO_GATE=1 pio run -t upload",
    ], "="


def render(lines, char):
    if char is None:
        return "\n".join(lines)
    bar = char * BANNER_WIDTH
    return "\n" + bar + "\n" + "\n".join(lines) + "\n" + bar + "\n"
