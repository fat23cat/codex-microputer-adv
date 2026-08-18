#!/usr/bin/env python3
"""Exercise the companion's diagnostic protocol over USB.

Streams device telemetry, and can push a synthetic task deck so the UI can be
exercised with no Codex session running. Useful for checking input, animations
and the completion chime in isolation.

  tools/devlink.py                 stream telemetry
  tools/devlink.py --demo          push a demo deck, then stream
  tools/devlink.py --complete 2    mark slot 2 done (fires the chime and toast)
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: source tools/env.sh first, it has the IDF venv")

DEMO = [
    ("RUNNING", "t-001", "Прошить M5Stack Cardputer"),
    ("INPUT",   "t-002", "Review the BLE bonding patch"),
    ("DONE",    "t-003", "Ship the deck redesign"),
    ("IDLE",    "t-004", "Refine companion controls"),
    ("ERROR",   "t-005", "Rebuild against ESP-IDF 5.5.3"),
    ("IDLE",    "t-006", "A deliberately long task title that has to be truncated"),
]


def send(port, line):
    port.write((line + "\n").encode("utf-8"))
    port.flush()
    time.sleep(0.03)


def push_deck(port, tasks, selected=0):
    send(port, "HOST|DEMO HOST")
    # Supply representative options so the OPT parser and the
    # settings wheels are exercised without a Codex session.
    send(port, "OPT|MODEL|gpt-5.6-luna|SOL=gpt-5.6-sol|LUNA=gpt-5.6-luna|5.5=gpt-5.5|SPARK=gpt-5.3-codex-spark")
    send(port, "OPT|EFFORT|max|LOW=low|MED=medium|HIGH=high|XHIGH=xhigh|MAX=max")
    send(port, "OPT|SPEED|priority|STD=default|FAST=priority")
    send(port, "CFG|gpt-5.6-luna|max|priority")
    for slot, (status, task_id, title) in enumerate(tasks):
        send(port, f"TASK|{slot}|{status}|{task_id}|{title}")
    send(port, f"TASKS|{len(tasks)}|{selected}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None))
    parser.add_argument("--demo", action="store_true", help="push the demo deck")
    parser.add_argument("--complete", type=int, metavar="SLOT",
                        help="push the demo deck with SLOT flipped to DONE")
    parser.add_argument("--seconds", type=float, default=0, help="stream for N seconds (0 = forever)")
    args = parser.parse_args()

    if not args.port:
        raise SystemExit("no /dev/cu.usbmodem* port found")

    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(0.3)
        port.reset_input_buffer()

        if args.demo or args.complete is not None:
            push_deck(port, DEMO)
        if args.complete is not None:
            time.sleep(1.2)
            tasks = list(DEMO)
            status, task_id, title = tasks[args.complete]
            tasks[args.complete] = ("DONE", task_id, title)
            push_deck(port, tasks)

        deadline = time.time() + args.seconds if args.seconds else None
        buffer = ""
        while deadline is None or time.time() < deadline:
            chunk = port.read(4096).decode("utf-8", errors="replace")
            if not chunk:
                continue
            buffer += chunk
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if line.startswith("CCP_"):
                    print(line, flush=True)


if __name__ == "__main__":
    main()
