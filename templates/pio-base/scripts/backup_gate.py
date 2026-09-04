"""
PlatformIO pre-upload gate.

Binds the repo's one rule -- no write without a verified backup -- to the path
people actually use. Hooking PlatformIO itself covers `pio run -t upload`,
`pio run -t uploadfs`, and any caller driving PlatformIO underneath, including
the platformio MCP server.

All decision logic lives in tools/gate.py so it can be tested without SCons.

Bypass, deliberately:   ESP32_NO_GATE=1 pio run -t upload
"""

import os
import sys

Import("env")  # noqa: F821  -- injected by SCons


def _find_root(start):
    """
    Bootstrap: locate tools/gate.py before we can import anything from it.

    NOTE: SCons executes extra scripts without `__file__` defined, so the only
    reliable anchor is PlatformIO's own $PROJECT_DIR. Do not reintroduce a
    `__file__` fallback here -- it raises NameError at upload time, which the
    unit tests cannot catch because they exercise gate.py, not this adapter.
    """
    probe = os.path.abspath(start)
    for _ in range(8):
        if os.path.isfile(os.path.join(probe, "tools", "gate.py")):
            return probe
        parent = os.path.dirname(probe)
        if parent == probe:
            return None
        probe = parent
    return None


def check_backup(source, target, env):
    # Bypass is checked first so it needs nothing else to work -- an escape
    # hatch that depends on the machinery it escapes is not an escape hatch.
    if os.environ.get("ESP32_NO_GATE") == "1":
        bar = "!" * 74
        print("\n" + bar)
        print("BACKUP GATE BYPASSED  (ESP32_NO_GATE=1)")
        print("Whatever is on this board is about to be overwritten with no")
        print("recoverable copy. Factory firmware is usually not redistributed.")
        print(bar + "\n")
        return

    root = _find_root(env.subst("$PROJECT_DIR")) or _find_root(os.getcwd())

    if root is None:
        bar = "!" * 74
        print("\n" + bar)
        print("BACKUP GATE INACTIVE")
        print("This project sits outside the esp32-workbench repo, so the gate")
        print("cannot locate tools/gate.py. Proceeding unguarded.")
        print(bar + "\n")
        return

    sys.path.insert(0, os.path.join(root, "tools"))
    from gate import evaluate, render, BLOCK

    try:
        from esp32ident import Esptool, enumerate_ports
        from esp32flash import board_identity, verified_backups
    except Exception as e:  # noqa: BLE001
        print(render([f"BACKUP GATE FAILED TO LOAD: {type(e).__name__}: {e}",
                      "Failing CLOSED: refusing to upload rather than assuming",
                      "this board is safe to overwrite.",
                      "Bypass with ESP32_NO_GATE=1 if you accept the risk."], "!"))
        env.Exit(1)  # noqa: F821
        return

    esp = Esptool()
    if not esp.exe:
        verdict, lines, char = evaluate(root, None, None, None, [],
                                        esptool_found=False)
        print(render(lines, char))
        return

    port = env.subst("$UPLOAD_PORT") or None
    if not port:
        ports = enumerate_ports()
        if not ports:
            # No board attached. PlatformIO fails next with a clearer message,
            # and nothing is at risk either way.
            return
        port = ports[0]["device"]

    try:
        mac, _size, chip = board_identity(esp, port)
    except SystemExit:
        # Chip unreachable. PlatformIO's own upload will fail and explain why.
        return

    verdict, lines, char = evaluate(root, mac, chip, port,
                                    verified_backups(mac))
    print(render(lines, char))
    if verdict == BLOCK:
        env.Exit(1)  # noqa: F821


env.AddPreAction("upload", check_backup)      # noqa: F821  firmware
env.AddPreAction("uploadfs", check_backup)    # noqa: F821  filesystem image
