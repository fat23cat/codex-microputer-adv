#!/usr/bin/env python3
import importlib.util
import pathlib
import struct
import tempfile
import unittest
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("screenshot_tool", ROOT / "tools" / "screenshot.py")
SHOT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHOT)


class ScreenshotToolTest(unittest.TestCase):
    def test_fnv_matches_firmware_byte_order(self):
        self.assertEqual(SHOT.fnv1a565([0x1234, 0xABCD]), 0xE46F3EBF)

    def test_png_decodes_m5canvas_byte_order(self):
        # Stored 0x00F8 becomes RGB565 0xF800 after byte swap: pure red.
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "pixel.png"
            SHOT.write_png(path, 1, 1, [0x00F8])
            data = path.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            offset = 8
            raw = None
            while offset < len(data):
                size = struct.unpack(">I", data[offset : offset + 4])[0]
                kind = data[offset + 4 : offset + 8]
                payload = data[offset + 8 : offset + 8 + size]
                if kind == b"IDAT":
                    raw = zlib.decompress(payload)
                    break
                offset += 12 + size
            self.assertEqual(raw, b"\x00\xff\x00\x00")


if __name__ == "__main__":
    unittest.main()
