#!/usr/bin/env python3
"""Wait for a complete Codex Micro control-plane handshake after flashing."""

import argparse
import calendar
from pathlib import Path
import time
from typing import Dict, Optional, Set

METHODS = ("v.oai.rgbcfg", "v.oai.thstatus", "device.status")


def newest_log(log_dir: Path) -> Optional[Path]:
    logs = list(log_dir.glob("codex-desktop-*.log"))
    return max(logs, key=lambda path: path.stat().st_mtime) if logs else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument(
        "--lookback", type=float, default=180,
        help="also accept a complete handshake already logged this many seconds ago",
    )
    parser.add_argument(
        "--log-dir", type=Path,
        default=Path.home() / "Library/Logs/com.openai.codex" / time.strftime("%Y/%m/%d"),
    )
    args = parser.parse_args()
    deadline = time.time() + args.timeout
    seen: Set[str] = set()
    offsets: Dict[Path, int] = {}

    while time.time() < deadline:
        path = newest_log(args.log_dir)
        if path:
            if path not in offsets and args.lookback > 0:
                # Read a bounded tail instead of the complete long-running app log.
                offset = max(0, path.stat().st_size - 256_000)
            else:
                offset = offsets.get(path, path.stat().st_size)
            with path.open(errors="replace") as stream:
                stream.seek(offset)
                for line in stream:
                    if args.lookback > 0 and line[:4].isdigit():
                        try:
                            stamp = time.strptime(line[:19], "%Y-%m-%dT%H:%M:%S")
                            if time.time() - calendar.timegm(stamp) > args.lookback:
                                continue
                        except ValueError:
                            pass
                    if "control-plane initialization failed" in line:
                        print("FAIL device_handshake: control-plane initialization failed")
                        return 1
                    if "Received answer" not in line:
                        continue
                    for method in METHODS:
                        if f'"{method}"' in line:
                            seen.add(method)
                offsets[path] = stream.tell()
        if len(seen) == len(METHODS):
            print("PASS device_handshake: " + ", ".join(METHODS))
            return 0
        time.sleep(0.25)
    missing = ", ".join(method for method in METHODS if method not in seen)
    print(f"FAIL device_handshake: timeout; missing {missing}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
