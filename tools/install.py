#!/usr/bin/env python3
from __future__ import annotations
import argparse, glob, hashlib, os, struct, subprocess, sys, tempfile
from datetime import datetime
from pathlib import Path

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
ENTRY_MAGIC = 0x50AA
ENTRY_SIZE = 32
TYPE_APP = 0
TYPE_DATA = 1
SUBTYPE_FACTORY = 0
OTA_MIN, OTA_MAX = 0x10, 0x20
APP_ALIGN = 0x10000
FLASH_SIZE = 0x800000
FLAG_ENCRYPTED = 1
HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
DEFAULT_IMAGE = PROJECT / "dist" / "Codex.bin"
BUILD_IMAGE = PROJECT / "build" / "codex_microputer_adv.bin"
SUBTYPE_NAMES = {
    (TYPE_DATA, 1): "phy",
    (TYPE_DATA, 2): "nvs",
    (TYPE_DATA, 0): "ota",
    (TYPE_DATA, 0x81): "fat",
    (TYPE_DATA, 0x82): "spiffs",
}


def esptool(port, *args, quiet=False):
    c = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, *args]
    r = subprocess.run(c, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"esptool failed: {' '.join(args)}")
    if not quiet:
        for line in r.stdout.splitlines():
            if any(k in line for k in ("Wrote", "Hash of data", "Erase")):
                print("  " + line.strip())
    return r.stdout


def file_sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_flash(port, offset, size, dst, attempts=3, chunk_size=0x10000):
    dst = Path(dst)
    with dst.open("wb") as output:
        for chunk_offset in range(0, size, chunk_size):
            length = min(chunk_size, size - chunk_offset)
            with tempfile.NamedTemporaryFile(delete=False) as scratch:
                chunk = Path(scratch.name)
            try:
                for attempt in range(1, attempts + 1):
                    try:
                        esptool(
                            port,
                            "--no-stub",
                            "--before",
                            "default_reset",
                            "--after",
                            "no_reset",
                            "read_flash",
                            hex(offset + chunk_offset),
                            hex(length),
                            str(chunk),
                            quiet=True,
                        )
                        output.write(chunk.read_bytes())
                        break
                    except SystemExit:
                        if attempt == attempts:
                            raise
                        print(
                            f"read-back interrupted at {hex(offset+chunk_offset)}; retrying ({attempt}/{attempts})"
                        )
            finally:
                chunk.unlink(missing_ok=True)


def parse_partition_table(blob):
    out = []
    for off in range(0, len(blob), ENTRY_SIZE):
        e = blob[off : off + ENTRY_SIZE]
        if len(e) < ENTRY_SIZE:
            break
        magic, ptype, sub, address, size = struct.unpack_from("<HBBII", e, 0)
        if magic != ENTRY_MAGIC:
            break
        label = e[12:28].split(b"\0", 1)[0].decode("ascii")
        flags = struct.unpack_from("<I", e, 28)[0]
        out.append(
            {
                "type": ptype,
                "subtype": sub,
                "offset": address,
                "size": size,
                "label": label,
                "flags": flags,
            }
        )
    return out


def read_partition_table(port):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        p = Path(f.name)
    try:
        read_flash(port, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, p)
        blob = p.read_bytes()
    finally:
        p.unlink(missing_ok=True)
    return blob, parse_partition_table(blob)


def max_partition_end(parts):
    return max((p["offset"] + p["size"] for p in parts), default=0)


def next_offset(parts):
    return (max_partition_end(parts) + APP_ALIGN - 1) & ~(APP_ALIGN - 1)


def next_ota_subtype(parts):
    used = {
        p["subtype"]
        for p in parts
        if p["type"] == TYPE_APP and OTA_MIN <= p["subtype"] < OTA_MAX
    }
    for s in range(OTA_MIN, OTA_MAX):
        if s not in used:
            return s
    raise SystemExit("no free OTA slot")


def validate_label(label):
    if not label or any(c in label for c in ",\r\n\0"):
        raise SystemExit("invalid partition label")
    try:
        label.encode("ascii")
    except UnicodeEncodeError as e:
        raise SystemExit("partition label must be ASCII") from e
    return label if len(label) <= 15 else label[:14] + ">"


def validate_partitions(parts):
    seen = set()
    end = PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE
    for p in sorted(parts, key=lambda x: x["offset"]):
        validate_label(p["label"])
        if p["label"] in seen:
            raise SystemExit("duplicate partition label")
        seen.add(p["label"])
        if p["offset"] < end:
            raise SystemExit(f"partition {p['label']!r} overlaps previous")
        if p["size"] <= 0 or p["offset"] + p["size"] > FLASH_SIZE:
            raise SystemExit(f"partition {p['label']!r} outside flash")
        if p["flags"] & ~FLAG_ENCRYPTED:
            raise SystemExit(f"partition {p['label']!r} has unsupported flags")
        end = p["offset"] + p["size"]


def to_csv(parts):
    validate_partitions(parts)
    lines = ["# Name, Type, SubType, Offset, Size, Flags"]
    for p in parts:
        typ = (
            "app"
            if p["type"] == TYPE_APP
            else "data" if p["type"] == TYPE_DATA else hex(p["type"])
        )
        if p["type"] == TYPE_APP:
            sub = (
                "factory"
                if p["subtype"] == SUBTYPE_FACTORY
                else (
                    f"ota_{p['subtype']-OTA_MIN}"
                    if OTA_MIN <= p["subtype"] < OTA_MAX
                    else hex(p["subtype"])
                )
            )
        else:
            sub = SUBTYPE_NAMES.get((p["type"], p["subtype"]), hex(p["subtype"]))
        flags = "encrypted" if p["flags"] & FLAG_ENCRYPTED else ""
        lines.append(
            f"{p['label']},{typ},{sub},{hex(p['offset'])},{hex(p['size'])},{flags}"
        )
    return "\n".join(lines) + "\n"


def write_partition_table(port, parts, original, backup):
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise SystemExit("IDF_PATH is not set")
    backup.parent.mkdir(parents=True, exist_ok=True)
    backup.write_bytes(original)
    print(f"partition table backup: {backup}")
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        csv = d / "table.csv"
        raw = d / "table.bin"
        csv.write_text(to_csv(parts))
        r = subprocess.run(
            [
                sys.executable,
                str(Path(idf) / "components/partition_table/gen_esp32part.py"),
                str(csv),
                str(raw),
            ],
            capture_output=True,
            text=True,
        )
        if r.returncode:
            raise SystemExit("failed to generate partition table")
        expected = raw.read_bytes().ljust(PARTITION_TABLE_SIZE, b"\xff")
        padded = d / "padded.bin"
        padded.write_bytes(expected)
        esptool(
            port,
            "--before",
            "default_reset",
            "--after",
            "no_reset",
            "write_flash",
            "--flash_mode",
            "dio",
            "--flash_size",
            "8MB",
            "--flash_freq",
            "80m",
            hex(PARTITION_TABLE_OFFSET),
            str(padded),
        )
        verify = d / "verify.bin"
        read_flash(port, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, verify)
        if verify.read_bytes() != expected or parse_partition_table(expected) != parts:
            raise SystemExit(f"partition table verification failed; backup: {backup}")


def verify_image(port, target, image):
    esptool(
        port,
        "--before",
        "default_reset",
        "--after",
        "no_reset",
        "verify_flash",
        "--flash_mode",
        "dio",
        "--flash_size",
        "8MB",
        "--flash_freq",
        "80m",
        hex(target["offset"]),
        str(image),
        quiet=True,
    )
    print("flash digest verified")


def write_image(port, target, image, baud, attempts=3):
    for attempt in range(1, attempts + 1):
        try:
            esptool(
                port,
                "-b",
                baud,
                "--before",
                "default_reset",
                "--after",
                "no_reset",
                "write_flash",
                "--flash_mode",
                "dio",
                "--flash_size",
                "8MB",
                "--flash_freq",
                "80m",
                hex(target["offset"]),
                str(image),
            )
            return
        except SystemExit:
            if attempt == attempts:
                raise
            print(f"flash write interrupted; retrying ({attempt}/{attempts})")


def select_ota(port, label, baud):
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise SystemExit("IDF_PATH is not set")
    r = subprocess.run(
        [
            sys.executable,
            str(Path(idf) / "components/app_update/otatool.py"),
            "--port",
            port,
            "--baud",
            baud,
            "switch_ota_partition",
            "--name",
            label,
        ],
        capture_output=True,
        text=True,
    )
    if r.returncode:
        raise SystemExit("failed to select OTA partition")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--port", default=next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    )
    ap.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    ap.add_argument("--label")
    ap.add_argument("--create-partition", action="store_true")
    ap.add_argument("--partition-backup", type=Path)
    ap.add_argument("--baud", default="460800")
    a = ap.parse_args()
    if not a.port:
        raise SystemExit("no USB modem port found")
    image = a.image.resolve()
    if not image.exists():
        raise SystemExit("image not found")
    if (
        image == DEFAULT_IMAGE.resolve()
        and BUILD_IMAGE.exists()
        and file_sha256(image) != file_sha256(BUILD_IMAGE)
    ):
        raise SystemExit("dist/Codex.bin is stale")
    label = validate_label(a.label or image.stem)
    size = image.stat().st_size
    aligned = (size + APP_ALIGN - 1) & ~(APP_ALIGN - 1)
    original, parts = read_partition_table(a.port)
    validate_partitions(parts)
    target = next(
        (p for p in parts if p["type"] == TYPE_APP and p["label"] == label), None
    )
    changed = False
    if target and target["subtype"] == SUBTYPE_FACTORY:
        raise SystemExit("refusing factory partition")
    if target and size > target["size"]:
        if not a.create_partition:
            raise SystemExit("image does not fit; use --create-partition")
        if target["offset"] + target["size"] != max_partition_end(parts):
            raise SystemExit("target is not last partition")
        if target["offset"] + aligned > FLASH_SIZE:
            raise SystemExit("resize exceeds flash")
        target["size"] = aligned
        changed = True
    elif target is None:
        if not a.create_partition:
            raise SystemExit("target partition not found")
        off = next_offset(parts)
        if off + aligned > FLASH_SIZE:
            raise SystemExit("not enough flash")
        target = {
            "type": TYPE_APP,
            "subtype": next_ota_subtype(parts),
            "offset": off,
            "size": aligned,
            "label": label,
            "flags": 0,
        }
        parts.append(target)
        changed = True
    if changed:
        write_partition_table(
            a.port,
            parts,
            original,
            a.partition_backup
            or PROJECT
            / "backups"
            / f"partition-table-{datetime.now():%Y%m%d-%H%M%S}.bin",
        )
    write_image(a.port, target, image, a.baud)
    verify_image(a.port, target, image)
    select_ota(a.port, label, "115200")
    print(f"installed and launched {label!r}")


if __name__ == "__main__":
    main()
