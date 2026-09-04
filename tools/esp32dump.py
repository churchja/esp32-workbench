#!/usr/bin/env python3
"""
esp32dump.py -- read what is already on a board, without changing it.

You cannot download source code from a flashed ESP32. The chip holds a
stripped binary; the C++ ceased to exist at compile time. What IS recoverable,
and what this tool recovers:

  partitions  every partition dumped to its own file, named by label
  fs          SPIFFS / LittleFS / FAT images unpacked into real directories
              (this is where vendor web UIs, fonts, and configs actually live)
  nvs         the key/value store: saved Wi-Fi credentials, calibration,
              provisioning state, device serials
  forensics   what built this firmware -- project name, IDF/Arduino version,
              build host paths, embedded URLs and SSIDs, toolchain fingerprints

All operations are read-only. Nothing here can modify the board.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from esp32ident import (Esptool, enumerate_ports, probe_silicon,  # noqa: E402
                        probe_partitions, parse_app_desc)


def die(m, code=1):
    print(f"ERROR: {m}", file=sys.stderr)
    sys.exit(code)


def info(m):
    print(m, file=sys.stderr)


# --------------------------------------------------------------------------
# strings -- implemented here rather than shelling out, because macOS,
# Linux, and BSD all ship subtly different `strings` binaries (or none).
# --------------------------------------------------------------------------

def extract_strings(blob, minlen=6):
    out, cur = [], bytearray()
    for byte in blob:
        if 0x20 <= byte < 0x7F:
            cur.append(byte)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode("ascii", "replace"))
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode("ascii", "replace"))
    return out


# Patterns that reliably say something about provenance. Each is a claim
# about the firmware's origin, not about the board's capabilities.
FORENSIC_PATTERNS = {
    "idf_version":   re.compile(r"\bv?\d+\.\d+(\.\d+)?(-dirty)?(-[0-9a-g]+)?\b.*(esp-idf|ESP-IDF)"),
    "idf_tag":       re.compile(r"(esp-idf[ _-]?v?\d+\.\d+(\.\d+)?)", re.I),
    "arduino_core":  re.compile(r"(arduino[- ]?esp32|ESP32 Arduino|framework-arduinoespressif32)[^\s]*", re.I),
    "build_host_path": re.compile(r"([A-Za-z]:\\Users\\[^\\\s\"]+|/Users/[^/\s\"]+|/home/[^/\s\"]+)"),
    "url":           re.compile(r"https?://[^\s\"'<>\\]{6,120}"),
    "wifi_ssid_hint": re.compile(r"\b(ssid|SSID)[=: ]+([A-Za-z0-9_\-]{2,32})"),
    "mac_literal":   re.compile(r"\b([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b"),
    "compiler":      re.compile(r"(GCC:?\s*\([^)]*\)\s*[\d.]+|xtensa-esp32[a-z0-9-]*-elf|riscv32-esp-elf)"),
    "source_file":   re.compile(r"[A-Za-z0-9_\-/]+\.(cpp|c|ino|h|hpp|py):\d+"),
    "lib_marker":    re.compile(r"\b(TFT_eSPI|LovyanGFX|Adafruit_[A-Za-z0-9_]+|lvgl|LVGL|NimBLE|BlueDroid|GxEPD2?|FastLED|ArduinoJson)\b"),
}


def forensics_report(blob, name):
    strs = extract_strings(blob)
    hits = defaultdict(set)
    for s in strs:
        for label, pat in FORENSIC_PATTERNS.items():
            for m in pat.finditer(s):
                hits[label].add(m.group(0)[:200])
    desc = parse_app_desc(blob)
    return {
        "partition": name,
        "bytes": len(blob),
        "printable_strings": len(strs),
        "app_descriptor": desc,
        "findings": {k: sorted(v)[:40] for k, v in sorted(hits.items())},
    }


# --------------------------------------------------------------------------
# NVS -- Espressif's key/value store. Format: 4096-byte pages, a 32-byte
# header, a 32-byte entry-state bitmap (2 bits/entry), then 126 32-byte slots.
# --------------------------------------------------------------------------

NVS_PAGE = 4096
NVS_ENTRY = 32
NVS_ENTRIES_PER_PAGE = 126
NVS_TYPES = {
    0x01: "u8", 0x11: "i8", 0x02: "u16", 0x12: "i16",
    0x04: "u32", 0x14: "i32", 0x08: "u64", 0x18: "i64",
    0x21: "str", 0x41: "blob", 0x42: "blob_data", 0x48: "blob_idx",
}
STATE_WRITTEN = 0b10


def parse_nvs(blob):
    namespaces, entries = {}, []
    for pstart in range(0, len(blob) - NVS_PAGE + 1, NVS_PAGE):
        page = blob[pstart:pstart + NVS_PAGE]
        state = struct.unpack("<I", page[0:4])[0]
        if state == 0xFFFFFFFF:
            continue  # empty page
        bitmap = page[32:64]
        body = page[64:]
        i = 0
        while i < NVS_ENTRIES_PER_PAGE:
            two = (bitmap[i // 4] >> ((i % 4) * 2)) & 0b11
            if two != STATE_WRITTEN:
                i += 1
                continue
            rec = body[i * NVS_ENTRY:(i + 1) * NVS_ENTRY]
            if len(rec) < NVS_ENTRY:
                break
            ns_idx, dtype, span = rec[0], rec[1], rec[2]
            key = rec[8:24].split(b"\x00")[0].decode("utf-8", "replace")
            tname = NVS_TYPES.get(dtype)
            if not tname or not key:
                i += max(1, span)
                continue
            value = None
            if tname in ("u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64"):
                raw = rec[24:32]
                fmt = {"u8": "<B", "i8": "<b", "u16": "<H", "i16": "<h",
                       "u32": "<I", "i32": "<i", "u64": "<Q", "i64": "<q"}[tname]
                value = struct.unpack(fmt, raw[:struct.calcsize(fmt)])[0]
            elif tname in ("str", "blob", "blob_data"):
                dlen = struct.unpack("<H", rec[24:26])[0]
                payload = body[(i + 1) * NVS_ENTRY:(i + 1) * NVS_ENTRY + dlen]
                if tname == "str":
                    value = payload.split(b"\x00")[0].decode("utf-8", "replace")
                else:
                    value = f"<{dlen} bytes> " + payload[:24].hex()
            if ns_idx == 0 and tname == "u8":
                namespaces[value] = key  # ns entries map index -> name
            else:
                entries.append({"ns_index": ns_idx, "key": key,
                                "type": tname, "value": value})
            i += max(1, span)
    for e in entries:
        e["namespace"] = namespaces.get(e["ns_index"], f"#{e['ns_index']}")
    return {"namespaces": namespaces, "entries": entries}


# --------------------------------------------------------------------------
# Filesystem images
# --------------------------------------------------------------------------

def detect_fs(blob):
    """Identify a filesystem image by its on-disk signature."""
    if len(blob) > 16 and blob[8:16] == b"littlefs":
        return "littlefs"
    # LittleFS v2 superblock can also sit at block 0 offset 0x08 after a rev tag
    if b"littlefs" in blob[:1024]:
        return "littlefs"
    if len(blob) > 512 and blob[510:512] == b"\x55\xaa":
        return "fat"
    # SPIFFS has no magic; infer from its page/object structure being present
    if len(blob) >= 256 and blob[:4] not in (b"\xff\xff\xff\xff",) \
            and b"\xff" * 64 in blob:
        return "spiffs?"
    return "unknown"


def find_pio_tool(name):
    """PlatformIO ships mklittlefs/mkspiffs inside its package cache."""
    p = shutil.which(name)
    if p:
        return p
    root = os.path.expanduser("~/.platformio/packages")
    if os.path.isdir(root):
        for d in os.listdir(root):
            cand = os.path.join(root, d, name)
            if os.path.isfile(cand) and os.access(cand, os.X_OK):
                return cand
    return None


def unpack_fs(image, outdir, kind):
    os.makedirs(outdir, exist_ok=True)
    if kind == "littlefs":
        tool = find_pio_tool("mklittlefs")
        if tool:
            r = subprocess.run([tool, "-u", outdir, "-b", "4096", "-p", "256",
                                image], capture_output=True, text=True)
            if r.returncode == 0:
                return "mklittlefs", None
            return None, r.stderr[-500:]
        try:
            from littlefs import LittleFS  # pip install littlefs-python
        except ImportError:
            return None, ("no unpacker available. Install one:\n"
                          "  pip install littlefs-python\n"
                          "  (or let PlatformIO fetch mklittlefs by building "
                          "any LittleFS project once)")
        with open(image, "rb") as fh:
            data = fh.read()
        fs = LittleFS(block_size=4096, block_count=len(data) // 4096, mount=False)
        fs.context.buffer = bytearray(data)
        fs.mount()
        count = 0
        for root, _dirs, files in fs.walk("/"):
            for f in files:
                src = os.path.join(root, f).replace("\\", "/")
                dst = os.path.join(outdir, src.lstrip("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with fs.open(src, "rb") as sh, open(dst, "wb") as dh:
                    dh.write(sh.read())
                count += 1
        return f"littlefs-python ({count} files)", None
    if kind and kind.startswith("spiffs"):
        tool = find_pio_tool("mkspiffs")
        if tool:
            r = subprocess.run([tool, "-u", outdir, "-b", "4096", "-p", "256",
                                image], capture_output=True, text=True)
            if r.returncode == 0:
                return "mkspiffs", None
            return None, r.stderr[-500:]
        return None, "mkspiffs not found; build any SPIFFS project in PlatformIO once to fetch it"
    return None, f"no unpacker for filesystem type '{kind}'"


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--from-image", help="analyse a local .bin instead of a board")
    ap.add_argument("--all", action="store_true", help="partitions + fs + nvs + forensics")
    ap.add_argument("--partitions", action="store_true")
    ap.add_argument("--fs", action="store_true")
    ap.add_argument("--nvs", action="store_true")
    ap.add_argument("--forensics", action="store_true")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    if not any([args.all, args.partitions, args.fs, args.nvs, args.forensics]):
        args.all = True
    want = lambda f: args.all or getattr(args, f)  # noqa: E731

    if args.from_image:
        with open(args.from_image, "rb") as fh:
            blob = fh.read()
        report = forensics_report(blob, os.path.basename(args.from_image))
        print(json.dumps(report, indent=2, default=str))
        return 0

    esp = Esptool()
    if not esp.exe:
        die("esptool not found. Install: pip install esptool")
    port = args.port
    if not port:
        ports = enumerate_ports()
        if not ports:
            die("No USB serial device found. Plug the board in.")
        port = ports[0]["device"]

    sil, _ = probe_silicon(esp, port)
    if "_probe_error" in sil:
        die("Could not reach the chip:\n" + str(sil["_probe_error"]["value"]))
    mac = sil.get("mac", {}).get("value", "unknown")
    chip = sil.get("chip", {}).get("value", "unknown")
    outdir = args.outdir or os.path.join(REPO, "backups",
                                         mac.replace(":", ""), "dump")
    os.makedirs(outdir, exist_ok=True)
    info(f"{chip} @ {mac} -> {outdir}")

    pt, _ = probe_partitions(esp, port, outdir)
    parts = pt.get("partitions", {}).get("value", [])
    if not parts:
        die("Could not read a partition table. Without it there is nothing "
            "to dump selectively; take a full backup instead:\n"
            "  python3 tools/esp32flash.py backup")

    summary = {"chip": chip, "mac": mac, "partitions": parts,
               "extracted": [], "forensics": [], "nvs": None}

    def needed(part):
        """
        Decide whether this partition must actually come down the wire.

        Previously every partition was read regardless of flags, so `--nvs` on
        an 8MB board pulled all 8MB over serial -- minutes -- to parse a 20KB
        partition. The flags filtered only the analysis, not the transfer.
        """
        if args.all or args.partitions:
            return True
        if args.nvs and part["subtype"] == "nvs":
            return True
        if args.fs and part["subtype"] in ("spiffs", "littlefs", "fat"):
            return True
        if args.forensics and part["type"] == "app":
            return True
        return False

    selected = [p for p in parts if needed(p)]
    skipped = len(parts) - len(selected)
    if skipped:
        info(f"  ({skipped} partition(s) skipped -- not needed for the "
             f"requested analysis)")

    for p in selected:
        dest = os.path.join(outdir, f"{p['label']}.bin")
        info(f"  reading {p['label']:<10} {p['offset']:>10} {p['size_human']:>8}")
        rc, so, se = esp.run(port, "read-flash", p["offset"], p["size"], dest,
                             timeout=args.timeout)
        if rc != 0:
            info(f"    failed: {(so+se).strip()[-200:]}")
            continue
        with open(dest, "rb") as fh:
            blob = fh.read()
        summary["extracted"].append({"label": p["label"], "file": dest,
                                     "bytes": len(blob)})

        if want("forensics") and p["type"] == "app":
            summary["forensics"].append(forensics_report(blob, p["label"]))

        if want("nvs") and p["subtype"] == "nvs":
            try:
                summary["nvs"] = parse_nvs(blob)
            except Exception as e:  # noqa: BLE001
                summary["nvs"] = {"error": f"{type(e).__name__}: {e}"}

        if want("fs") and p["subtype"] in ("spiffs", "littlefs", "fat"):
            kind = detect_fs(blob)
            target = os.path.join(outdir, f"{p['label']}.extracted")
            method, err = unpack_fs(dest, target, kind)
            summary.setdefault("filesystems", []).append({
                "label": p["label"], "detected": kind,
                "unpacked_with": method, "output": target if method else None,
                "error": err,
            })

    path = os.path.join(outdir, "report.json")
    with open(path, "w") as fh:
        json.dump(summary, fh, indent=2, default=str)
    print(json.dumps(summary, indent=2, default=str))
    info(f"\nreport: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
