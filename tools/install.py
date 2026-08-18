#!/usr/bin/env python3
"""Install or update the companion over USB, the way M5Apps would.

The M5Apps Installer, given a bare application image, allocates one app
partition and flashes the image into it. This script reproduces that end state
exactly, so the launcher, FDISK and the OTA machinery keep treating the app as a
normally installed one:

  * offset  = end of the last partition, rounded up to 64 KB
  * size    = image size, rounded up to 64 KB
  * subtype = the first unused ota_N
  * label   = the image filename without its extension (max 15 chars)

By default it only writes into a partition that already exists. Creating or
resizing one edits the partition table, so it takes an explicit
--create-partition. The factory partition, where M5Apps itself lives, is never a
target, and existing entries are never reordered or resized.
"""

import argparse
import glob
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
ENTRY_MAGIC = 0x50AA
ENTRY_SIZE = 32
TYPE_APP = 0x00
TYPE_DATA = 0x01
SUBTYPE_FACTORY = 0x00
OTA_MIN, OTA_MAX = 0x10, 0x20
APP_ALIGN = 0x10000
FLASH_SIZE = 0x800000

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DEFAULT_IMAGE = os.path.join(PROJECT, "dist", "Codex.bin")
BUILD_IMAGE = os.path.join(PROJECT, "build", "codex_microputer_adv.bin")

SUBTYPE_NAMES = {
    (TYPE_DATA, 0x01): "phy", (TYPE_DATA, 0x02): "nvs", (TYPE_DATA, 0x00): "ota",
    (TYPE_DATA, 0x81): "fat", (TYPE_DATA, 0x82): "spiffs",
}


def esptool(port, *args, quiet=False):
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, *args]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"esptool failed: {' '.join(args)}")
    if not quiet:
        for line in result.stdout.splitlines():
            if any(k in line for k in ("Wrote", "Hash of data", "Erase")):
                print("  " + line.strip())
    return result.stdout


def select_ota_partition(port, label, baud):
    """Select the installed app exactly as M5Apps does before launching it."""
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise SystemExit("IDF_PATH is not set; source tools/env.sh first")
    otatool = os.path.join(idf_path, "components", "app_update", "otatool.py")
    command = [sys.executable, otatool, "--port", port, "--baud", baud,
               "switch_ota_partition", "--name", label]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"failed to select {label!r} as the M5Apps launch target")


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_partitions(port):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as scratch:
        path = scratch.name
    try:
        esptool(port, "--before", "default_reset", "--after", "no_reset",
                "read_flash", hex(PARTITION_TABLE_OFFSET), hex(PARTITION_TABLE_SIZE), path,
                quiet=True)
        blob = open(path, "rb").read()
    finally:
        os.unlink(path)

    partitions = []
    for offset in range(0, len(blob), ENTRY_SIZE):
        entry = blob[offset:offset + ENTRY_SIZE]
        if len(entry) < ENTRY_SIZE:
            break
        magic, ptype, subtype, address, size = struct.unpack_from("<HBBII", entry, 0)
        if magic != ENTRY_MAGIC:
            break
        label = entry[12:28].split(b"\0")[0].decode(errors="replace")
        flags = struct.unpack_from("<I", entry, 28)[0]
        partitions.append({"type": ptype, "subtype": subtype, "offset": address,
                           "size": size, "label": label, "flags": flags})
    return partitions


def next_offset(partitions):
    end = max((p["offset"] + p["size"] for p in partitions), default=0)
    return (end + APP_ALIGN - 1) & ~(APP_ALIGN - 1)


def next_ota_subtype(partitions):
    used = {p["subtype"] for p in partitions
            if p["type"] == TYPE_APP and OTA_MIN <= p["subtype"] < OTA_MAX}
    for subtype in range(OTA_MIN, OTA_MAX):
        if subtype not in used:
            return subtype
    raise SystemExit("no free OTA slot in the partition table")


def to_csv(partitions):
    lines = ["# Name, Type, SubType, Offset, Size, Flags"]
    for p in partitions:
        if p["type"] == TYPE_APP:
            type_name = "app"
            sub = "factory" if p["subtype"] == SUBTYPE_FACTORY else f"ota_{p['subtype'] - OTA_MIN}"
        else:
            type_name = "data"
            sub = SUBTYPE_NAMES.get((p["type"], p["subtype"]), hex(p["subtype"]))
        lines.append(f"{p['label']},{type_name},{sub},{hex(p['offset'])},{hex(p['size'])},")
    return "\n".join(lines) + "\n"


def write_partition_table(port, partitions):
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise SystemExit("IDF_PATH is not set; source tools/env.sh first")
    generator = os.path.join(idf_path, "components", "partition_table", "gen_esp32part.py")

    with tempfile.TemporaryDirectory() as workdir:
        csv_path = os.path.join(workdir, "table.csv")
        bin_path = os.path.join(workdir, "table.bin")
        open(csv_path, "w").write(to_csv(partitions))
        result = subprocess.run([sys.executable, generator, csv_path, bin_path],
                                capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout + result.stderr)
            raise SystemExit("failed to generate the partition table")
        esptool(port, "--before", "default_reset", "--after", "no_reset",
                "write_flash", "--flash_mode", "dio", "--flash_size", "8MB",
                "--flash_freq", "80m", hex(PARTITION_TABLE_OFFSET), bin_path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None))
    parser.add_argument("--image", default=DEFAULT_IMAGE)
    parser.add_argument("--label", default=None,
                        help="partition label; defaults to the image filename")
    parser.add_argument("--create-partition", action="store_true",
                        help="allocate or resize the app partition (edits the partition table)")
    parser.add_argument("--baud", default="921600")
    args = parser.parse_args()

    if not args.port:
        raise SystemExit("no /dev/cu.usbmodem* port found")
    if not os.path.exists(args.image):
        raise SystemExit(f"image not found: {args.image} (run tools/build.sh first)")
    if os.path.abspath(args.image) == os.path.abspath(DEFAULT_IMAGE) and os.path.exists(BUILD_IMAGE):
        staged_hash = file_sha256(args.image)
        build_hash = file_sha256(BUILD_IMAGE)
        if staged_hash != build_hash:
            raise SystemExit(
                "dist/Codex.bin is stale and does not match build/codex_microputer_adv.bin.\n"
                "Run ./tools/build.sh before installing; refusing to flash an old UI."
            )

    label = args.label or os.path.splitext(os.path.basename(args.image))[0]
    if len(label) > 15:
        label = label[:14] + ">"
    image_size = os.path.getsize(args.image)
    aligned_size = (image_size + APP_ALIGN - 1) & ~(APP_ALIGN - 1)

    partitions = read_partitions(args.port)
    if not partitions:
        raise SystemExit("could not read a partition table from the device")
    print(f"device has {len(partitions)} partitions; "
          f"app slots: {', '.join(p['label'] for p in partitions if p['type'] == TYPE_APP)}")

    target = next((p for p in partitions if p["type"] == TYPE_APP and p["label"] == label), None)

    if target and target["subtype"] == SUBTYPE_FACTORY:
        raise SystemExit(f"{label!r} is the factory partition (M5Apps). Refusing.")

    if target and image_size > target["size"]:
        if not args.create_partition:
            raise SystemExit(
                f"image is {image_size} bytes, partition {label!r} holds {target['size']}.\n"
                "Re-run with --create-partition to resize it."
            )
        if target["offset"] + target["size"] != next_offset(partitions):
            raise SystemExit(
                f"{label!r} is not the last partition, so resizing it would move others. "
                "Delete it with FDISK on the device and re-run."
            )
        print(f"resizing {label!r} from {target['size']} to {aligned_size}")
        target["size"] = aligned_size
        write_partition_table(args.port, partitions)

    elif target is None:
        if not args.create_partition:
            raise SystemExit(
                f"no app partition labelled {label!r} on the device.\n"
                "Re-run with --create-partition to allocate one, exactly as the "
                "M5Apps Installer would."
            )
        offset = next_offset(partitions)
        if offset + aligned_size > FLASH_SIZE:
            raise SystemExit(f"not enough free flash: need {aligned_size} at 0x{offset:X}")
        subtype = next_ota_subtype(partitions)
        target = {"type": TYPE_APP, "subtype": subtype, "offset": offset,
                  "size": aligned_size, "label": label, "flags": 0}
        partitions.append(target)
        print(f"allocating {label!r} as ota_{subtype - OTA_MIN} "
              f"at 0x{offset:X}, size 0x{aligned_size:X}")
        write_partition_table(args.port, partitions)

    print(f"writing {image_size} bytes into {label!r} at 0x{target['offset']:X}")
    esptool(args.port, "-b", args.baud, "--before", "default_reset", "--after", "no_reset",
            "write_flash", "--flash_mode", "dio", "--flash_size", "8MB", "--flash_freq", "80m",
            hex(target["offset"]), args.image)
    print(f"selecting {label!r} as the M5Apps launch target")
    select_ota_partition(args.port, label, args.baud)
    print(f"\ninstalled and launched {label!r}.")


if __name__ == "__main__":
    main()
