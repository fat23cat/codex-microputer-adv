#!/usr/bin/env python3
import importlib.util
import calendar
import os
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "verify_codex_connection", ROOT / "tools/verify_codex_connection.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def main():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        older = root / "2026/08/20/codex-desktop-old.log"
        newer = root / "2026/08/21/codex-desktop-new.log"
        older.parent.mkdir(parents=True)
        newer.parent.mkdir(parents=True)
        older.write_text("old")
        newer.write_text("new")
        now = time.time()
        os.utime(older, (now - 10, now - 10))
        os.utime(newer, (now, now))
        assert module.newest_log(root) == newer
    stamp = module.line_timestamp("2026-08-20T22:41:25.732Z info")
    assert stamp == calendar.timegm(time.strptime("2026-08-20T22:41:25", "%Y-%m-%dT%H:%M:%S"))
    assert module.line_timestamp("not a timestamp") is None
    print("PASS verify_codex_connection")


if __name__ == "__main__":
    main()
