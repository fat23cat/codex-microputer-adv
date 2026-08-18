#!/usr/bin/env python3
"""Fail when a publishable tree contains credentials or machine-local data."""

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()

FORBIDDEN_PATHS = re.compile(
    r"(^|/)(\.env(?:\..+)?|backups?|build|dist|managed_components|"
    r"credentials?(?:\..+)?|secrets?(?:\..+)?|id_(?:rsa|ed25519)(?:\..+)?)$",
    re.IGNORECASE,
)
LOCAL_DATA = [
    re.compile(r"/(?:Users|home)/[^/\s]+/"),
    re.compile(r"[A-Z]:\\Users\\[^\\\s]+\\", re.IGNORECASE),
    re.compile(r"\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b", re.IGNORECASE),
]
SECRET_DATA = [
    re.compile(r"BEGIN (?:RSA |OPENSSH |EC |DSA )?PRIVATE KEY"),
    re.compile(r"AKIA[0-9A-Z]{16}"),
    re.compile(r"AIza[0-9A-Za-z_-]{35}"),
    re.compile(r"github_pat_[0-9A-Za-z_]{20,}"),
    re.compile(r"gh" + r"[pousr]_[0-9A-Za-z_]{20,}"),
    re.compile(r"sk" + r"-[0-9A-Za-z_-]{20,}"),
    re.compile(r"xox" + r"[baprs]-[0-9A-Za-z-]{10,}"),
]


def publishable_files() -> list[Path]:
    if not (ROOT / ".git").exists():
        return [path for path in ROOT.rglob("*") if path.is_file()]
    result = subprocess.run(
        ["git", "ls-files", "-co", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [ROOT / item.decode() for item in result.stdout.split(b"\0") if item]


def main() -> int:
    failures: list[str] = []
    for path in publishable_files():
        relative = path.relative_to(ROOT).as_posix()
        if FORBIDDEN_PATHS.search(relative):
            failures.append(f"forbidden path: {relative}")
            continue
        if path.resolve() == SELF or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for pattern in LOCAL_DATA + SECRET_DATA:
            if pattern.search(text):
                failures.append(f"sensitive data pattern in: {relative}")
                break

    if failures:
        print("FAIL public-tree audit")
        print("\n".join(failures))
        return 1
    print("PASS public-tree audit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
