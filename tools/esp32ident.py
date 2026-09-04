#!/usr/bin/env python3
"""
esp32ident.py -- board-agnostic ESP32 identification engine.

Probes an unknown ESP32-family board over USB and emits a provenance-tagged
board profile. Reads ONLY; never writes to flash.

Pipeline stages (each degrades gracefully -- a failure at stage N still
emits everything learned in stages < N):

  0 enumerate   candidate serial ports, non-ESP devices filtered out
  1 usb         VID/PID -> USB bridge chip or native-USB SoC
  2 silicon     esptool chip probe: chip type, revision, features, MAC
  3 flash       SPI flash manufacturer/device ID and true size
  4 partitions  read + parse the partition table at 0x8000
  5 appdesc     parse esp_app_desc_t out of the running app partition
  6 emit        merged profile keyed by MAC, provenance on every field

Every fact carries a provenance level so a guessed pin map is never
mistaken for one read off the silicon:

  probed      read directly from the chip. Unarguable.
  usb         from the USB device descriptor.
  vendor_doc  from a vendor datasheet/schematic. Carries source_url.
  community   from a forum, wiki, or GitHub repo. Carries source_url.
  inferred    deduced from other probed facts. Explain in `note`.
  unverified  a guess. NEVER wire power to a pin marked this way.
  verified    was inferred/unverified, then confirmed on real hardware.
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone

import yaml  # guaranteed present: esptool requires PyYAML>=5.1

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARDS_DIR = os.path.join(REPO, "boards")

# --------------------------------------------------------------------------
# Provenance
# --------------------------------------------------------------------------

PROVENANCE_ORDER = [
    "unverified", "inferred", "community", "vendor_doc",
    "usb", "probed", "verified",
]


# Levels that are claims about an external document. A claim with no traceable
# origin is indistinguishable from an invention, so these must carry a source.
SOURCED_LEVELS = {"vendor_doc", "community"}


def fact(value, provenance, source=None, note=None):
    """Wrap a value with where it came from. This is the core discipline."""
    if provenance not in PROVENANCE_ORDER:
        raise ValueError(f"unknown provenance: {provenance}")
    if provenance in SOURCED_LEVELS and not source:
        raise ValueError(
            f"provenance '{provenance}' requires a source URL. "
            f"Without one, downgrade to 'unverified' -- do not assert it.")
    d = {"value": value, "provenance": provenance}
    if source:
        d["source"] = source
    if note:
        d["note"] = note
    return d


# --------------------------------------------------------------------------
# Stage 0/1 -- port enumeration and USB identification
# --------------------------------------------------------------------------

# Ports that are never an ESP32. macOS in particular exposes a pile of these.
PORT_DENYLIST = re.compile(
    r"(Bluetooth-Incoming-Port|debug-console|wlan-debug|/dev/cu\.BLTH)", re.I
)

# USB bridge / native-USB identification. Sources noted per entry; anything
# uncertain is emitted at 'inferred' rather than 'usb'.
USB_IDS = {
    # Espressif native USB (chip speaks USB itself -- no bridge chip)
    (0x303A, 0x1001): ("Espressif USB JTAG/serial debug unit", "native", "usb"),
    (0x303A, 0x0002): ("Espressif ESP32-S2 native USB CDC", "native", "usb"),
    (0x303A, 0x4001): ("Espressif native USB (composite)", "native", "inferred"),
    # QinHeng CH34x bridges -- the classic cheap-clone bridge
    (0x1A86, 0x7523): ("CH340/CH341 USB-UART bridge", "bridge", "usb"),
    (0x1A86, 0x7522): ("CH340K USB-UART bridge", "bridge", "usb"),
    (0x1A86, 0x55D4): ("CH9102F/CH343 USB-UART bridge", "bridge", "usb"),
    # Silicon Labs CP210x
    (0x10C4, 0xEA60): ("CP2102/CP2102N USB-UART bridge", "bridge", "usb"),
    (0x10C4, 0xEA70): ("CP2105 dual USB-UART bridge", "bridge", "usb"),
    (0x10C4, 0xEA71): ("CP2108 quad USB-UART bridge", "bridge", "usb"),
    # FTDI
    (0x0403, 0x6001): ("FTDI FT232R USB-UART bridge", "bridge", "usb"),
    (0x0403, 0x6010): ("FTDI FT2232 USB-UART bridge", "bridge", "usb"),
    (0x0403, 0x6015): ("FTDI FT231X USB-UART bridge", "bridge", "usb"),
}

# Vendor VIDs -- identifies who made the board, not the bridge chip.
VENDOR_VIDS = {
    0x303A: "Espressif",
    0x239A: "Adafruit",
    0x2886: "Seeed Studio",
    0x1B4F: "SparkFun",
    0x2E8A: "Raspberry Pi",
    0x1A86: "QinHeng (bridge vendor, not board vendor)",
    0x10C4: "Silicon Labs (bridge vendor, not board vendor)",
    0x0403: "FTDI (bridge vendor, not board vendor)",
}


def enumerate_ports(strict=True):
    """
    Return candidate serial ports.

    Discrimination rule: an ESP32 reached over USB ALWAYS presents a USB
    VID/PID. Bluetooth-bound serial ports (headphones, phones), the macOS
    debug console, and virtual ports never do. So we filter on "has a USB
    identity" -- a structural property -- rather than on a name blocklist,
    which can never be complete.

    strict=False disables that rule, for the rare board behind an adapter
    that hides its descriptors.
    """
    ports = []
    try:
        from serial.tools import list_ports  # ships with esptool
        for p in list_ports.comports():
            if PORT_DENYLIST.search(p.device or ""):
                continue
            if strict and (p.vid is None or p.pid is None):
                continue  # not a USB device -> cannot be an ESP32 over USB
            ports.append({
                "device": p.device,
                "description": p.description,
                "vid": p.vid,
                "pid": p.pid,
                "serial_number": p.serial_number,
                "manufacturer": p.manufacturer,
                "product": p.product,
                "_source": "pyserial",
            })
    except ImportError:
        # Fallback: glob. No VID/PID available, so stage 1 degrades.
        pats = ["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB*",
                "/dev/cu.wchusbserial*", "/dev/ttyUSB*", "/dev/ttyACM*"]
        for pat in pats:
            for dev in sorted(glob.glob(pat)):
                if PORT_DENYLIST.search(dev):
                    continue
                ports.append({"device": dev, "description": None, "vid": None,
                              "pid": None, "serial_number": None,
                              "manufacturer": None, "product": None,
                              "_source": "glob"})
    return ports


def identify_usb(port):
    """Stage 1: turn VID/PID into a statement about how USB reaches the chip."""
    out = {}
    vid, pid = port.get("vid"), port.get("pid")
    if vid is None or pid is None:
        out["usb_interface"] = fact(
            "unknown", "unverified",
            note="pyserial unavailable; VID/PID could not be read")
        return out

    out["usb_vid"] = fact(f"0x{vid:04x}", "usb")
    out["usb_pid"] = fact(f"0x{pid:04x}", "usb")
    if port.get("serial_number"):
        out["usb_serial_number"] = fact(port["serial_number"], "usb")

    entry = USB_IDS.get((vid, pid))
    if entry:
        name, kind, prov = entry
        out["usb_interface"] = fact(name, prov)
        out["usb_interface_kind"] = fact(kind, prov, note=(
            "native = the SoC implements USB directly (supports USB-JTAG debug, "
            "USB CDC console, and can re-enumerate after reset). "
            "bridge = a separate UART chip; the SoC has no USB peripheral."
        ))
    else:
        out["usb_interface"] = fact(
            port.get("description") or f"unknown device {vid:04x}:{pid:04x}",
            "inferred",
            note="VID/PID not in the known table; research required")
        out["usb_interface_kind"] = fact("unknown", "unverified")

    if vid in VENDOR_VIDS:
        out["usb_vendor"] = fact(VENDOR_VIDS[vid], "usb")
    return out


# --------------------------------------------------------------------------
# esptool invocation -- version-aware, because 5.x renamed every subcommand
# --------------------------------------------------------------------------

class Esptool:
    """
    Wraps esptool, adapting to the 4.x/5.x CLI split.

    esptool 5.x uses hyphenated subcommands (read-flash, flash-id, write-flash).
    esptool 4.x uses underscored ones (read_flash, flash_id, write_flash).
    Every ESP32 tutorial on the internet uses the 4.x spelling, so a tool that
    hardcodes either one is broken for half its users. We probe instead.
    """

    def __init__(self):
        self.exe = shutil.which("esptool") or shutil.which("esptool.py")
        self.version = None
        self.hyphenated = True
        if self.exe:
            try:
                out = subprocess.run([self.exe, "version"], capture_output=True,
                                     text=True, timeout=30)
                blob = (out.stdout or "") + (out.stderr or "")
            except Exception:
                blob = ""
            if not blob:
                try:
                    out = subprocess.run([self.exe, "--help"],
                                         capture_output=True, text=True,
                                         timeout=30)
                    blob = (out.stdout or "") + (out.stderr or "")
                except Exception:
                    blob = ""
            m = re.search(r"v?(\d+)\.(\d+)\.(\d+)", blob)
            if m:
                self.version = ".".join(m.groups())
                self.hyphenated = int(m.group(1)) >= 5
            # Definitive check: does the help text advertise the hyphen form?
            if "read-flash" in blob:
                self.hyphenated = True
            elif "read_flash" in blob:
                self.hyphenated = False

    def cmd(self, name):
        """Translate a canonical hyphenated subcommand to this esptool's dialect."""
        return name if self.hyphenated else name.replace("-", "_")

    def run(self, port, subcmd, *args, timeout=180, chip="auto", baud=None):
        """
        baud: esptool defaults to 115200 (~11.5 KB/s), which makes a full 8MB
        read take ~12 minutes. Measured on an ESP32-S3 over native USB. Pass a
        higher rate for bulk transfers; --baud is a GLOBAL option and must
        precede the subcommand.
        """
        if not self.exe:
            return 127, "", "esptool not found on PATH"
        argv = [self.exe, "--chip", chip, "--port", port]
        if baud:
            argv += ["--baud", str(baud)]
        argv += [self.cmd(subcmd), *[str(a) for a in args]]
        try:
            p = subprocess.run(argv, capture_output=True, text=True,
                               timeout=timeout)
            return p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            return 124, "", f"timeout after {timeout}s running: {' '.join(argv)}"
        except Exception as e:  # noqa: BLE001
            return 1, "", f"{type(e).__name__}: {e}"


# --------------------------------------------------------------------------
# Stage 2/3 -- silicon and flash
# --------------------------------------------------------------------------

# esptool changed its connect banner between 4.x and 5.x. Verified against the
# installed esptool 5.2.0 source (esptool/__init__.py, esptool/cmds.py):
#
#   4.x:  "Chip is ESP32-C6 (revision v0.1)"    "Crystal is 40MHz"
#   5.x:  "Chip type:          ESP32-C6 ..."    "Crystal frequency:  40MHz"
#
# Accept both. Matching only one yields a profile with no chip and no MAC,
# which in turn silently disables the backup gate.
CHIP_RE = re.compile(r"^\s*(?:Chip is|Chip type:)\s+(.+?)\s*$", re.I | re.M)
CONNECTED_RE = re.compile(r"^\s*Connected to\s+(\S+)\s+on\s", re.I | re.M)
FEAT_RE = re.compile(r"^\s*Features:\s*(.+?)\s*$", re.I | re.M)
XTAL_RE = re.compile(r"^\s*Crystal(?: is| frequency:)\s*(\S+)", re.I | re.M)
USB_MODE_RE = re.compile(r"^\s*USB mode:\s*(.+?)\s*$", re.I | re.M)
REV_RE = re.compile(r"[Rr]evision\s*v?([0-9.]+)")
FLASH_MFR_RE = re.compile(r"Manufacturer:\s*([0-9a-f]+)", re.I)
FLASH_DEV_RE = re.compile(r"Device:\s*([0-9a-f]+)", re.I)
FLASH_SIZE_RE = re.compile(r"Detected flash size:\s*(\S+)", re.I)
# Discovered by running against real hardware, not from docs: esptool also
# reports how the flash is wired and what voltage the eFuse selects. Both are
# build-affecting and neither is guessable from the part number.
FLASH_TYPE_RE = re.compile(r"Flash type set in eFuse:\s*(.+?)\s*$", re.I | re.M)
FLASH_VOLT_RE = re.compile(r"Flash voltage set by eFuse:\s*(\S+)", re.I)

# MAC handling is the subtle one. On chips with EUI64 (C6, C5, H2 ...) esptool
# prints THREE lines:
#     MAC:                60:55:f9:ff:fe:f7:2c:a2     <- 8-byte EUI64
#     BASE MAC:           60:55:f9:f7:2c:a2           <- the 6-byte identity
#     MAC_EXT:            ff:fe
# A naive /MAC:\s*([0-9a-f:]{17})/ matches the EUI64 line FIRST and clips it to
# six octets -- a plausible but WRONG identity, and identity is the backup key.
# Prefer BASE MAC, and anchor to end-of-line so a longer address cannot be
# truncated into looking like a short one.
_OCT6 = r"([0-9a-f]{2}(?::[0-9a-f]{2}){5})"
BASE_MAC_RE = re.compile(r"^\s*BASE MAC:\s*" + _OCT6 + r"\s*$", re.I | re.M)
ANY_MAC_RE = re.compile(r"^\s*MAC:\s*" + _OCT6 + r"\s*$", re.I | re.M)


def parse_chip_banner(blob):
    """
    Pure parser for esptool's connect banner.

    Split out from the subprocess call so the highest-risk parsing in the repo
    can be tested against recorded output without hardware attached.
    """
    out = {}

    m = CHIP_RE.search(blob) or CONNECTED_RE.search(blob)
    if m:
        chip_full = m.group(1).strip()
        out["chip"] = fact(chip_full, "probed")
        out["chip_family"] = fact(re.split(r"\s*\(", chip_full)[0].strip(), "probed")
        r = REV_RE.search(chip_full)
        if r:
            out["chip_revision"] = fact(r.group(1), "probed")

    m = FEAT_RE.search(blob)
    if m:
        feats = [f.strip() for f in m.group(1).split(",") if f.strip()]
        out["chip_features"] = fact(feats, "probed", note=(
            "Read from eFuse. Authoritative for which radios and embedded "
            "memory the silicon has, regardless of the vendor listing."))

    m = XTAL_RE.search(blob)
    if m:
        out["crystal"] = fact(m.group(1), "probed")

    m = USB_MODE_RE.search(blob)
    if m:
        out["usb_mode"] = fact(m.group(1), "probed", note=(
            "Reported by esptool 5.x from the chip itself; corroborates the "
            "USB VID/PID reading of native vs bridge."))

    m, src = BASE_MAC_RE.search(blob), "BASE MAC"
    if not m:
        m, src = ANY_MAC_RE.search(blob), "MAC"
    if m:
        out["mac"] = fact(m.group(1).lower(), "probed", note=(
            f"Base MAC from eFuse (read from the '{src}' line) -- globally "
            "unique per board. Used as the profile and backup key so two "
            "boards of the same model never collide."))

    m = FLASH_MFR_RE.search(blob)
    if m:
        out["flash_manufacturer_id"] = fact(f"0x{m.group(1)}", "probed")
    m = FLASH_DEV_RE.search(blob)
    if m:
        out["flash_device_id"] = fact(f"0x{m.group(1)}", "probed")
    m = FLASH_SIZE_RE.search(blob)
    if m:
        out["flash_size"] = fact(m.group(1), "probed", note=(
            "Detected from the SPI flash chip itself, not a build config. "
            "Vendor listings are frequently wrong about this."))

    m = FLASH_TYPE_RE.search(blob)
    if m:
        out["flash_mode"] = fact(m.group(1), "probed", note=(
            "quad vs octal changes the flash driver and the build config "
            "(CONFIG_ESPTOOLPY_OCT_FLASH / flash_mode). Getting it wrong "
            "produces a board that flashes but will not boot."))

    m = FLASH_VOLT_RE.search(blob)
    if m:
        out["flash_voltage"] = fact(m.group(1), "probed", note=(
            "Set by eFuse. A 1.8V part driven at 3.3V is a hardware fault, "
            "not a config choice -- never override this from a guess."))
    return out


def probe_silicon(esp, port):
    """Stage 2+3: one connection; parsing is delegated to parse_chip_banner."""
    diag = {}
    rc, so, se = esp.run(port, "flash-id", timeout=120)
    blob = so + se
    diag["flash_id_rc"] = rc
    diag["flash_id_output"] = blob.strip()[-4000:]

    if rc != 0:
        return ({"_probe_error": fact(blob.strip()[-800:], "probed",
                                      note="esptool could not reach the chip")},
                diag)

    # Capture real banner text the first time a board is attached, so the
    # parser tests can move from self-authored fixtures to genuine conformance:
    #     ESP32_RECORD_FIXTURE=tests/fixtures/c6.txt python3 tools/esp32ident.py
    rec = os.environ.get("ESP32_RECORD_FIXTURE")
    if rec:
        os.makedirs(os.path.dirname(rec) or ".", exist_ok=True)
        with open(rec, "w") as fh:
            fh.write(blob)
        diag["fixture_recorded"] = rec

    return parse_chip_banner(blob), diag


# --------------------------------------------------------------------------
# Stage 4 -- partition table
# --------------------------------------------------------------------------

PART_MAGIC = 0x50AA
PART_MD5_MAGIC = 0xEBEB
PART_TABLE_OFFSET = 0x8000
PART_TABLE_SIZE = 0xC00

PART_TYPES = {0x00: "app", 0x01: "data"}
APP_SUBTYPES = {
    0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x12: "ota_2",
    0x13: "ota_3", 0x20: "test",
}
DATA_SUBTYPES = {
    0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump",
    0x04: "nvs_keys", 0x05: "efuse_em", 0x06: "undefined",
    0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs",
}


def parse_partition_table(blob):
    """Parse a raw partition-table image into entries. 32 bytes per entry."""
    entries, md5 = [], None
    for off in range(0, len(blob) - 31, 32):
        rec = blob[off:off + 32]
        magic = struct.unpack("<H", rec[0:2])[0]
        if magic == PART_MD5_MAGIC:
            md5 = rec[16:32].hex()
            continue
        if magic != PART_MAGIC:
            break  # 0xFF padding -> end of table
        ptype, psub = rec[2], rec[3]
        poff, psize = struct.unpack("<II", rec[4:12])
        label = rec[12:28].split(b"\x00")[0].decode("utf-8", "replace")
        flags = struct.unpack("<I", rec[28:32])[0]
        tname = PART_TYPES.get(ptype, f"0x{ptype:02x}")
        sub_map = APP_SUBTYPES if ptype == 0x00 else DATA_SUBTYPES
        sname = sub_map.get(psub, f"0x{psub:02x}")
        entries.append({
            "label": label, "type": tname, "subtype": sname,
            "offset": f"0x{poff:x}", "offset_int": poff,
            "size": f"0x{psize:x}", "size_int": psize,
            "size_human": human_bytes(psize),
            "encrypted": bool(flags & 1),
        })
    return entries, md5


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n}{unit}" if unit == "B" else f"{n:g}{unit}"
        n /= 1024.0


def probe_partitions(esp, port, workdir):
    out, diag = {}, {}
    dest = os.path.join(workdir, "partition-table.bin")
    rc, so, se = esp.run(port, "read-flash", hex(PART_TABLE_OFFSET),
                         hex(PART_TABLE_SIZE), dest, timeout=180)
    diag["partition_rc"] = rc
    if rc != 0 or not os.path.exists(dest):
        diag["partition_error"] = (so + se).strip()[-800:]
        return out, diag
    with open(dest, "rb") as fh:
        blob = fh.read()
    entries, md5 = parse_partition_table(blob)
    if not entries:
        diag["partition_error"] = "no valid partition entries (magic 0x50AA absent)"
        return out, diag
    out["partitions"] = fact(entries, "probed", note=(
        "Parsed from the on-chip table at 0x8000. This is the ground truth "
        "for flash layout -- prefer it over any assumed default."))
    if md5:
        out["partition_table_md5"] = fact(md5, "probed")
    return out, diag


# --------------------------------------------------------------------------
# Stage 5 -- esp_app_desc_t out of the running app
# --------------------------------------------------------------------------

APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 0x20  # esp_app_desc_t sits right after the image header


def parse_app_desc(blob):
    if len(blob) < APP_DESC_OFFSET + 256:
        return None
    d = blob[APP_DESC_OFFSET:APP_DESC_OFFSET + 256]
    magic = struct.unpack("<I", d[0:4])[0]
    if magic != APP_DESC_MAGIC:
        return None

    def s(b):
        return b.split(b"\x00")[0].decode("utf-8", "replace").strip()

    return {
        "secure_version": struct.unpack("<I", d[4:8])[0],
        "app_version": s(d[16:48]),
        "project_name": s(d[48:80]),
        "build_time": s(d[80:96]),
        "build_date": s(d[96:112]),
        "idf_version": s(d[112:144]),
        "app_elf_sha256": d[144:176].hex(),
    }


def probe_app(esp, port, workdir, partitions):
    """Read the head of each app partition and decode its descriptor."""
    out, diag, apps = {}, {}, []
    app_parts = [p for p in partitions if p["type"] == "app"]
    for p in app_parts[:4]:
        dest = os.path.join(workdir, f"app-head-{p['label']}.bin")
        rc, so, se = esp.run(port, "read-flash", p["offset"], "0x1000",
                             dest, timeout=180)
        if rc != 0 or not os.path.exists(dest):
            diag[f"app_{p['label']}_error"] = (so + se).strip()[-400:]
            continue
        with open(dest, "rb") as fh:
            head = fh.read()
        desc = parse_app_desc(head)
        if desc:
            desc["partition"] = p["label"]
            apps.append(desc)
    if apps:
        out["applications"] = fact(apps, "probed", note=(
            "Decoded from esp_app_desc_t. project_name and idf_version tell "
            "you what firmware is on the board and which SDK built it."))
    return out, diag


# --------------------------------------------------------------------------
# Profile assembly
# --------------------------------------------------------------------------

def profile_key(merged):
    mac = merged.get("mac", {}).get("value")
    if mac:
        return mac.replace(":", "").lower()
    return "unknown-" + hashlib.sha1(
        json.dumps(merged, sort_keys=True, default=str).encode()
    ).hexdigest()[:10]


def build_profile(port, sections, diag):
    merged = {}
    for s in sections:
        merged.update(s)
    key = profile_key(merged)
    unknowns = suggest_research(merged)
    return {
        "schema_version": 1,
        "profile_id": key,
        "identified_at": datetime.now(timezone.utc).isoformat(),
        "port_seen_on": port,
        "identity": merged,
        "pinmap": {},
        "peripherals": {},
        "research_queue": unknowns,
        "_diagnostics": diag,
    }


def suggest_research(merged):
    """
    Name what probing CANNOT answer. These are the fields that require
    fetching a datasheet -- they are not encoded in the silicon anywhere.
    """
    q = []
    chip = merged.get("chip_family", {}).get("value", "unknown chip")
    if "display" not in merged:
        q.append({
            "field": "display", "status": "unknown",
            "why": "Display controller, resolution, and bus pins are a board-level "
                   "design choice; nothing on the SoC records them.",
            "how": f"Find the vendor schematic or board definition for the {chip} "
                   "board carrying this display; confirm controller part number "
                   "(ST7789 / ILI9341 / SSD1680 / GDEH...) before writing a driver.",
        })
    q.append({
        "field": "pinmap", "status": "unknown",
        "why": "GPIO assignment is a PCB routing decision, invisible to probing.",
        "how": "Vendor schematic PDF first, then the vendor's Arduino/PlatformIO "
               "board .json or pins_arduino.h, then community repos. Mark every "
               "pin 'unverified' until a physical test confirms it.",
        "danger": "Driving a wrong pin can short a rail. Never mark verified "
                  "without a hardware test.",
    })
    q.append({
        "field": "power", "status": "unknown",
        "why": "Battery connector, charge IC, and ADC divider ratio are board-level.",
        "how": "Schematic. The divider ratio in particular is a magic number that "
               "cannot be guessed and silently corrupts every battery reading.",
    })
    return q



# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Identify an unknown ESP32 board. Read-only; never writes flash.")
    ap.add_argument("--port", help="serial port; auto-detected if omitted")
    ap.add_argument("--list", action="store_true", help="list candidate ports and exit")
    ap.add_argument("--any-port", action="store_true",
                    help="include serial ports with no USB identity "
                         "(Bluetooth/virtual). Off by default.")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of YAML")
    ap.add_argument("--save", action="store_true",
                    help=f"write the profile into {BOARDS_DIR}/")
    ap.add_argument("--workdir", default=None, help="scratch dir for dumped blobs")
    args = ap.parse_args()

    ports = enumerate_ports(strict=not args.any_port)
    if not ports and not args.any_port:
        loose = enumerate_ports(strict=False)
        if loose:
            print(f"# No USB serial devices found, but {len(loose)} non-USB "
                  f"serial port(s) exist (Bluetooth/virtual). These are almost "
                  f"certainly not ESP32 boards. Re-run with --any-port to "
                  f"include them anyway.", file=sys.stderr)
    if args.list or not args.port:
        if not ports:
            print("No candidate serial ports found.", file=sys.stderr)
            print("Plug a board in over USB. If it is already plugged in, the "
                  "cable may be charge-only -- a surprising number are.",
                  file=sys.stderr)
            return 2
        if args.list:
            for p in ports:
                vid = f"{p['vid']:04x}" if p['vid'] is not None else "????"
                pid = f"{p['pid']:04x}" if p['pid'] is not None else "????"
                print(f"{p['device']}\t{vid}:{pid}\t{p.get('description') or ''}")
            return 0

    port = args.port or ports[0]["device"]
    portmeta = next((p for p in ports if p["device"] == port),
                    {"device": port, "vid": None, "pid": None})
    if not args.port and len(ports) > 1:
        print(f"# {len(ports)} candidate ports; using {port}. "
              f"Use --list to see all.", file=sys.stderr)

    workdir = args.workdir or os.path.join(
        REPO, ".scratch", f"ident-{int(time.time())}")
    os.makedirs(workdir, exist_ok=True)

    esp = Esptool()
    diag = {
        "esptool_path": esp.exe,
        "esptool_version": esp.version,
        "esptool_dialect": "hyphenated (5.x)" if esp.hyphenated
                           else "underscored (4.x)",
        "workdir": workdir,
    }
    if not esp.exe:
        print("esptool not found on PATH. Install with: pip install esptool",
              file=sys.stderr)
        return 3

    sections = [identify_usb(portmeta)]
    sil, d = probe_silicon(esp, port)
    sections.append(sil)
    diag.update(d)

    parts = []
    if "_probe_error" not in sil:
        pt, d = probe_partitions(esp, port, workdir)
        sections.append(pt)
        diag.update(d)
        parts = pt.get("partitions", {}).get("value", [])
        if parts:
            app, d = probe_app(esp, port, workdir, parts)
            sections.append(app)
            diag.update(d)

    profile = build_profile(port, sections, diag)

    if args.json:
        text = json.dumps(profile, indent=2, default=str)
    else:
        text = yaml.safe_dump(profile, sort_keys=False,
                              default_flow_style=False, width=100)
    print(text)

    if args.save:
        os.makedirs(BOARDS_DIR, exist_ok=True)
        path = os.path.join(BOARDS_DIR, f"{profile['profile_id']}.yaml")
        existing = ""
        if os.path.exists(path):
            with open(path) as fh:
                existing = fh.read()
            backup = path + f".{int(time.time())}.bak"
            with open(backup, "w") as fh:
                fh.write(existing)
            print(f"# previous profile preserved at {backup}", file=sys.stderr)
        with open(path, "w") as fh:
            yaml.safe_dump(profile, fh, sort_keys=False,
                           default_flow_style=False, width=100)
        print(f"# profile saved: {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
