#!/usr/bin/env python3
"""Capture real Cardputer UI frames over USB Serial/JTAG as PNG files."""

from __future__ import annotations

import argparse
import binascii
import glob
import pathlib
import struct
import sys
import time
import zlib

SCENES = ("splash", "pairing", "deck", "recording", "composer", "settings", "debug")
CAPTURE_CHOICES = ("live", *SCENES)


def find_port() -> str:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not candidates:
        raise SystemExit("Cardputer USB serial port was not found")
    if len(candidates) > 1:
        raise SystemExit("Multiple USB serial ports found; pass --port explicitly")
    return candidates[0]


def fnv1a565(pixels: list[int]) -> int:
    value = 2166136261
    for pixel in pixels:
        value = ((value ^ (pixel >> 8)) * 16777619) & 0xFFFFFFFF
        value = ((value ^ (pixel & 0xFF)) * 16777619) & 0xFFFFFFFF
    return value


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF)


def write_png(path: pathlib.Path, width: int, height: int, pixels: list[int]) -> None:
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for value in pixels[y * width : (y + 1) * width]:
            # M5Canvas stores 16-bit sprite pixels in display byte order.
            value = ((value & 0xFF) << 8) | (value >> 8)
            r5, g6, b5 = (value >> 11) & 0x1F, (value >> 5) & 0x3F, value & 0x1F
            rows.extend((r5 * 255 // 31, g6 * 255 // 63, b5 * 255 // 31))
    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += png_chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def _capture_once(port: str, scene: str, timeout: float = 12.0) -> tuple[int, int, list[int]]:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required; run ./tools/setup.sh") from exc
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as device:
        device.reset_input_buffer()
        device.write(f"SCREENSHOT|{scene}\n".encode())
        device.flush()
        deadline = time.monotonic() + timeout
        width = height = expected = None
        chunks: dict[int, str] = {}
        while time.monotonic() < deadline:
            raw = device.readline()
            if not raw:
                continue
            line = raw.decode("ascii", errors="ignore").strip()
            if line.startswith("CCP_SHOT|ERROR|"):
                raise RuntimeError(line)
            if line.startswith("CCP_SHOT|BEGIN|"):
                parts = line.split("|")
                if len(parts) != 7 or parts[2] != scene or parts[5] != "RGB565":
                    raise RuntimeError(f"invalid screenshot header: {line}")
                width, height, expected = int(parts[3]), int(parts[4]), int(parts[6])
                chunks.clear()
            elif line.startswith("CCP_SHOT|DATA|") and expected is not None:
                _, _, sequence, payload = line.split("|", 3)
                chunks[int(sequence)] = payload
            elif line.startswith("CCP_SHOT|END|") and expected is not None:
                payload = "".join(chunks[index] for index in sorted(chunks))
                if len(payload) % 4:
                    raise RuntimeError("truncated RGB565 payload")
                pixels = [int(payload[i : i + 4], 16) for i in range(0, len(payload), 4)]
                if len(pixels) != expected or expected != width * height:
                    raise RuntimeError(f"expected {expected} pixels, received {len(pixels)}")
                expected_checksum = int(line.rsplit("|", 1)[1], 16)
                if fnv1a565(pixels) != expected_checksum:
                    raise RuntimeError("screenshot checksum mismatch")
                return width, height, pixels
    raise TimeoutError(f"timed out waiting for {scene} screenshot")


def capture(port: str, scene: str, timeout: float = 12.0) -> tuple[int, int, list[int]]:
    # USB Serial/JTAG shares the physical interface with native HID. A cable
    # remount can truncate a frame even though every complete frame carries a
    # checksum, so retry the whole atomic capture instead of writing bad PNG.
    last_error: Exception | None = None
    for _ in range(3):
        try:
            return _capture_once(port, scene, timeout)
        except (OSError, RuntimeError, TimeoutError, ValueError, KeyError) as error:
            last_error = error
            time.sleep(0.2)
    raise RuntimeError(f"failed to capture {scene} after 3 attempts: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial device; auto-detected by default")
    parser.add_argument("--scene", choices=(*CAPTURE_CHOICES, "all"), default="deck")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("screenshots"))
    args = parser.parse_args()
    port = args.port or find_port()
    scenes = SCENES if args.scene == "all" else (args.scene,)
    output_is_file = len(scenes) == 1 and args.output.suffix.lower() == ".png"
    for scene in scenes:
        width, height, pixels = capture(port, scene)
        target = args.output if output_is_file else args.output / f"{scene}.png"
        write_png(target, width, height, pixels)
        print(f"{scene}: {target} ({width}x{height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
