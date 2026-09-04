"""
ESP-IDF pre-flash backup gate.

Drop this file at the ROOT of an ESP-IDF project (next to CMakeLists.txt and
sdkconfig). idf.py loads <project_dir>/idf_ext.py automatically and calls
action_extensions(); the dict returned here registers a global action callback.

Verified against ESP-IDF source:
  tools/idf.py                     -- CLI.execute_tasks() runs every entry of
                                      ctx.command.global_action_callbacks BEFORE
                                      the task loop, and before dry_run is
                                      honoured. Aborting here aborts everything.
  tools/idf_py_actions/tools.py    -- merge_action_lists() forwards
                                      'global_action_callbacks' from a project
                                      extension, and (on v6.x) rejects a dict
                                      with no 'version' key.
  tools/idf_py_actions/serial_ext.py -- the action names that write to a chip.

This is the ESP-IDF counterpart of templates/pio-base/scripts/backup_gate.py.
Both adapters are pure I/O; the decision lives in tools/gate.py so it stays
testable.

Bypass, deliberately:   ESP32_NO_GATE=1 idf.py flash
"""

import os
import sys

# --------------------------------------------------------------------------
# Nothing at module scope may fail.
#
# idf.py v6.x wraps the action_extensions() call in `except Exception` and only
# logs a warning -- an extension that raises is silently skipped and the flash
# proceeds UNGUARDED. So action_extensions() below does nothing but return a
# literal, and every fallible operation happens inside the callback, where a
# failure can be turned into a refusal instead of a warning.
# --------------------------------------------------------------------------

BANNER_WIDTH = 74

# Action names that put bytes on the chip. Sources:
#   serial_ext.py : flash, app-flash, bootloader-flash, partition-table-flash,
#                   encrypted-flash, encrypted-app-flash, erase-flash,
#                   erase_flash (deprecated alias), erase-otadata,
#                   efuse-burn, efuse-burn-key, efuse-read-protect,
#                   efuse-write-protect
#   dfu_ext.py    : dfu-flash
WRITE_ACTIONS = frozenset({
    'flash', 'app-flash', 'bootloader-flash', 'partition-table-flash',
    'encrypted-flash', 'encrypted-app-flash',
    'erase-flash', 'erase_flash', 'erase-otadata',
    'dfu-flash',
})

# eFuse actions are deliberately NOT in WRITE_ACTIONS.
#
# This gate's entire premise is "a verified backup makes the write reversible."
# That premise DOES NOT HOLD for eFuse: burning a fuse is a one-way physical
# change, and no flash image can undo it. Gating these on "do you have a
# backup?" would return ALLOW and imply a protection that does not exist --
# false assurance, which is worse than no gate at all.
#
# They get their own branch: always refuse, with a separate opt-in, because
# there is no state to restore afterwards.
EFUSE_ACTIONS = frozenset({
    'efuse-burn', 'efuse-burn-key',
    'efuse-read-protect', 'efuse-write-protect',
})


def _is_write_action(name):
    """
    True if this task can modify the chip.

    The suffix rule is not redundant with the set above. Any command idf.py
    does not recognise is handed to the 'fallback' action, which builds it as
    a raw ninja target -- and get_command() constructs that Action with
    name=<whatever was typed>. So `idf.py some-custom-flash` arrives here as a
    task literally named "some-custom-flash". Matching the suffix catches
    build-system flash targets this file has never heard of. Over-matching
    costs one backup; under-matching costs a board.
    """
    return name in WRITE_ACTIONS or name == 'flash' or name.endswith('-flash')


def _find_root(start):
    """
    Bootstrap: locate the workbench before anything can be imported from it.

    ESP32_WORKBENCH_ROOT wins if set, so a project living outside the repo can
    still be gated.
    """
    override = os.environ.get('ESP32_WORKBENCH_ROOT')
    if override:
        if os.path.isfile(os.path.join(override, 'tools', 'gate.py')):
            return os.path.abspath(override)
        # Falling through to the cwd search is the fail-SAFE direction -- a
        # typo must not disable the gate. But doing it silently means a
        # misconfigured override looks identical to a working one, so say so.
        _say(f'[backup-gate] ESP32_WORKBENCH_ROOT={override!r} does not contain '
             f'tools/gate.py; ignoring it and searching upward instead.')

    for base in (start, os.path.dirname(os.path.abspath(__file__)), os.getcwd()):
        if not base:
            continue
        probe = os.path.abspath(base)
        for _ in range(8):
            if os.path.isfile(os.path.join(probe, 'tools', 'gate.py')):
                return probe
            parent = os.path.dirname(probe)
            if parent == probe:
                break
            probe = parent
    return None


def _say(text):
    """Print to stderr and flush now -- idf.py's own output is buffered."""
    sys.stderr.write(text + '\n')
    sys.stderr.flush()


def _banner(lines, char):
    bar = char * BANNER_WIDTH
    return '\n' + bar + '\n' + '\n'.join(lines) + '\n' + bar


def _abort(lines):
    """
    Stop idf.py before any task runs.

    FatalError is caught at the bottom of tools/idf.py and turned into
    log.die(..., exit_code=2). It subclasses RuntimeError (via
    esp_pylib.errors.FatalError), not click.ClickException, so click's
    standalone_mode does not swallow it. The full banner is written to stderr
    first because log.die() runs the message through rich markup escaping and
    reflows it; only the one-line summary goes through that path.
    """
    _say(_banner(lines, '='))
    summary = 'Refusing to write to this board: no verified backup exists.'
    try:
        from idf_py_actions.errors import FatalError
    except Exception:                                          # noqa: BLE001
        raise SystemExit(2)
    raise FatalError(summary)


def _resolve_port(global_args):
    """
    Return the port idf.py itself would write to, or None if there is provably
    nothing to write to.

    Order matters. -p/--port has scope 'global' in serial_ext.PORT, and
    execute_tasks() promotes per-task global-scope options into global_args
    BEFORE the callbacks run, so an explicit -p is already visible here.
    Otherwise fall through to idf.py's OWN resolver, so the board this gate
    identifies is provably the board the flash will hit -- not a different one
    that happens to sort first in our own enumeration.
    """
    try:
        explicit = global_args.get('port')
    except Exception:                                          # noqa: BLE001
        explicit = None
    if explicit:
        return str(explicit)

    try:
        from idf_py_actions.tools import get_default_serial_port
    except Exception:                                          # noqa: BLE001
        return _fallback_port()

    try:
        return get_default_serial_port()
    except Exception as e:                                     # noqa: BLE001
        if type(e).__name__ == 'NoSerialPortFoundError':
            # idf.py's flash callback calls this same function and will raise
            # the same error before touching anything. Nothing to guard.
            return None
        # Detection blew up for some other reason. Do NOT treat that as
        # "no board attached" -- fall back to our own enumeration.
        return _fallback_port()


def _fallback_port():
    """Last resort when idf.py's own resolver is unusable."""
    try:
        from esp32ident import enumerate_ports
        ports = enumerate_ports()
    except Exception:                                          # noqa: BLE001
        return None
    if not ports:
        return None
    return ports[0]['device']


def backup_gate(ctx, global_args, tasks):
    """
    global_action_callback -- idf.py calls this as
        action_callback(ctx, global_args, tasks)
    from CLI.execute_tasks(), before the task queue is built or executed.

    ctx         click Context
    global_args PropertyDict of resolved global arguments
    tasks       list of Task objects; task.name is the action name
    """
    names = [getattr(t, 'name', '') for t in tasks]
    writes = sorted({n for n in names if _is_write_action(n)})
    burns = sorted({n for n in names if n in EFUSE_ACTIONS})

    # ------------------------------------------------------------------
    # eFuse FIRST, and before the `if not writes` exit -- `idf.py efuse-burn`
    # on its own has no flash action, so checking it after that early return
    # would let the single most irreversible operation through ungated.
    #
    # A burned fuse cannot be restored from a flash image. The backup question
    # is therefore not merely unhelpful here, it is misleading: answering
    # "yes, you have a backup" implies a safety net that does not exist. So
    # this branch never consults backups at all.
    # ------------------------------------------------------------------
    if burns:
        if os.environ.get('ESP32_EFUSE_I_UNDERSTAND') != '1':
            _abort([
                'BLOCKED -- eFuse burn is PERMANENT and cannot be undone',
                '',
                f'  Action : idf.py {" ".join(burns)}',
                '',
                '  Burning an eFuse is a one-way physical change to the chip.',
                '  No flash backup can reverse it. This gate protects writes',
                '  by keeping a restorable copy, and there is nothing to',
                '  restore here -- so it will not pretend to cover you.',
                '',
                '  Read the eFuse docs, be certain of the exact bits, then:',
                f'    ESP32_EFUSE_I_UNDERSTAND=1 idf.py {" ".join(burns)}',
                '',
                '  ESP32_NO_GATE=1 deliberately does NOT unlock this.',
            ])
            return
        _say(_banner([
            'eFUSE BURN PROCEEDING -- PERMANENT, IRREVERSIBLE',
            f'  {" ".join(burns)}',
            'No backup covers this. There is no undo.',
        ], '!'))

    if not writes:
        return

    # Checked first, so the escape hatch depends on nothing that can break.
    if os.environ.get('ESP32_NO_GATE') == '1':
        _say(_banner([
            'BACKUP GATE BYPASSED  (ESP32_NO_GATE=1)',
            'Whatever is on this board is about to be overwritten with no',
            'recoverable copy. Factory firmware is usually not redistributed.',
        ], '!'))
        return

    # --dry-run writes nothing. global_action_callbacks run at idf.py:733,
    # BEFORE the dry-run short-circuit, so without this check the gate blocks
    # a command that cannot touch the chip. Blocking harmless commands is how
    # a gate teaches people to disable it.
    try:
        if global_args.get('dry_run'):
            return
    except Exception:                                          # noqa: BLE001
        pass

    project_dir = None
    try:
        project_dir = global_args.get('project_dir')
    except Exception:                                          # noqa: BLE001
        pass
    root = _find_root(project_dir or os.getcwd())

    if root is None:
        # Matches gate.py's own policy for a project outside the workbench:
        # allow, but loudly. Changing that belongs in gate.py, not here.
        _say(_banner([
            'BACKUP GATE INACTIVE',
            '',
            'This project sits outside the esp32-workbench repo, so the gate',
            'cannot locate tools/gate.py and cannot verify a backup.',
            'Set ESP32_WORKBENCH_ROOT=/path/to/esp32-workbench to enable it.',
            'Proceeding unguarded.',
        ], '!'))
        return

    tools_dir = os.path.join(root, 'tools')
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)

    try:
        from gate import evaluate, render, BLOCK
        from esp32ident import Esptool
        from esp32flash import board_identity, verified_backups
    except Exception as e:                                     # noqa: BLE001
        _abort([
            f'BACKUP GATE FAILED TO LOAD: {type(e).__name__}: {e}',
            '',
            'Failing CLOSED: refusing to write rather than assuming this',
            'board is safe to overwrite.',
            '',
            f'  Blocked: idf.py {" ".join(writes)}',
            '  Bypass with ESP32_NO_GATE=1 if you accept the risk.',
        ])
        return

    esp = Esptool()
    if not esp.exe:
        _verdict, lines, char = evaluate(root, None, None, None, [],
                                        esptool_found=False)
        _say(render(lines, char))
        return

    port = _resolve_port(global_args)
    if port is None:
        # idf.py's own resolver found nothing, so its flash action will raise
        # NoSerialPortFoundError before touching anything.
        return

    # ------------------------------------------------------------------
    # PIN THE PORT. Without this the gate is unsound.
    #
    # serial_ext.py:65-66 does `if args.port is None: args.port =
    # get_default_serial_port()` at EXECUTION time, and that resolver
    # (idf_py_actions/tools.py) re-runs esptool's scan every call, iterating
    # reversed(port_list) and breaking on the first port that answers. It is
    # connection-outcome dependent, not deterministic.
    #
    # So without pinning: the gate resolves port A, verifies board A's backup,
    # and then flash independently resolves port B -- overwriting a DIFFERENT
    # board whose backup was never checked. Our own probe below even resets
    # the chip, which can change which port answers first.
    #
    # HOW to pin it -- this is narrower than it looks.
    #
    # idf.py invokes an action as:
    #     self.callback(self.name, context, global_args, **action_args)
    # (v6.0.3 tools/idf.py:286). `port` has scope 'global' (serial_ext.py:38-45),
    # so idf.py deliberately KEEPS it in global_args and REMOVES it from
    # action_args (:759-763). serial_ext.flash then reads args.port, where
    # `args` IS global_args.
    #
    # Therefore setting global_args['port'] is both necessary and SUFFICIENT.
    # Do NOT also write it into task.action_args: that re-adds a global-scope
    # option idf.py removed on purpose, it arrives as an unexpected keyword,
    # and every flash dies with
    #     TypeError: flash() got an unexpected keyword argument 'port'
    # An earlier version did exactly that. Unit tests missed it because a fake
    # Task's action_args is a plain dict that accepts anything, while a real
    # callback has a signature.
    # ------------------------------------------------------------------
    try:
        global_args['port'] = port
    except Exception:                                          # noqa: BLE001
        pass

    _say(f'[backup-gate] identifying board on {port} '
         f'(this can take up to ~2 min if the chip needs BOOT held '
         f'during RESET) ...')

    try:
        mac, _size, chip = board_identity(esp, port)
    except SystemExit:
        # board_identity() calls die() -> sys.exit(1) when the chip does not
        # answer. Deliberately stricter than the PlatformIO adapter, which
        # returns here and lets the uploader fail on its own: our probe and
        # idf.py's flash do not use identical reset/baud settings, so "we
        # could not connect" is not proof that idf.py cannot.  No identity
        # means no backup key, and no backup key means no gate.
        _abort([
            'UPLOAD BLOCKED -- board identity could not be established',
            '',
            f'  Port : {port}',
            '',
            '  The chip did not answer, so this gate cannot tell which board',
            '  it is and cannot confirm a backup exists for it.',
            '',
            '  Usually: hold BOOT and tap RESET, close any serial monitor,',
            '  or replace a charge-only USB cable. Then re-run.',
            '',
            '  Or bypass deliberately:',
            f'    ESP32_NO_GATE=1 idf.py {" ".join(writes)}',
        ])
        return

    verdict, lines, char = evaluate(root, mac, chip, port, verified_backups(mac))
    if verdict == BLOCK:
        rel = os.path.join(root, 'tools', 'esp32flash.py')
        _abort([
            'UPLOAD BLOCKED -- no verified backup for this board',
            '',
            f'  Board  : {chip}',
            f'  MAC    : {mac}',
            f'  Port   : {port}',
            f'  Action : idf.py {" ".join(writes)}',
            '',
            '  Take one first (~3 min for 8MB at 460800 baud):',
            f'    python3 {rel} backup --port {port}',
            '',
            '  Or bypass deliberately:',
            f'    ESP32_NO_GATE=1 idf.py {" ".join(writes)}',
        ])
        return
    _say(render(lines, char))


def action_extensions(base_actions, project_path):
    """
    idf.py extension entry point.

    'version' is mandatory on ESP-IDF v6.x -- merge_action_lists() raises
    AttributeError without it. On v5.x merge_action_lists() reads only
    global_options / actions / global_action_callbacks, so the extra key is
    ignored. One file, both branches.

    Keep this function total. It must never raise.
    """
    return {
        'version': '1',
        'global_action_callbacks': [backup_gate],
    }
