#!/usr/bin/env python3
"""Копирует app.bin → esp32_MK3_app_vX.Y.Z.bin по version_state.txt."""
import argparse
import os
import re
import shutil
import sys

SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)\s*$")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--dst-dir", required=True)
    ap.add_argument("--prefix", default="uart_echo")
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        print(f"copy_named_firmware: source not found: {args.src}", file=sys.stderr)
        return 1

    ver = "0.0.0"
    if os.path.isfile(args.state):
        with open(args.state, "r", encoding="utf-8") as f:
            s = f.read().strip().replace("\r", "")
        if SEMVER.match(s):
            ver = s

    dst = os.path.join(args.dst_dir, f"{args.prefix}_v{ver}.bin")
    shutil.copy2(args.src, dst)
    print(f"Versioned firmware: {dst} ({os.path.getsize(dst)} bytes)")

    latest = os.path.join(args.dst_dir, f"{args.prefix}_latest.bin")
    try:
        if os.path.isfile(latest) or os.path.islink(latest):
            os.remove(latest)
        shutil.copy2(dst, latest)
    except OSError:
        pass
    return 0

if __name__ == "__main__":
    sys.exit(main())