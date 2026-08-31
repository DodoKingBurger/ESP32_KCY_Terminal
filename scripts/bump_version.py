#!/usr/bin/env python3
"""Инкремент patch-версии при каждой сборке. Пишет build/esp_fw_version.h."""
import argparse
import os
import re
import sys

SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)\s*$")

def read_ver(path, default="1.0.0"):
    if not os.path.isfile(path):
        return default
    with open(path, "r", encoding="utf-8") as f:
        s = f.read().strip().replace("\r", "")
    return s if SEMVER.match(s) else default

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--no-increment", action="store_true",
                    help="только записать header из state/base, без +1")
    args = ap.parse_args()

    base = read_ver(args.base, "1.0.0")
    bm, bn, bp = map(int, base.split("."))

    prev = read_ver(args.state, "") if os.path.isfile(args.state) else ""
    if args.no_increment:
        if prev and SEMVER.match(prev):
            ver = prev
        else:
            ver = base
    elif prev and SEMVER.match(prev):
        pm, pn, pp = map(int, prev.split("."))
        if pm != bm or pn != bn:
            # version.txt сменили major/minor — стартуем с базы
            ver = f"{bm}.{bn}.{bp}"
        else:
            ver = f"{pm}.{pn}.{pp + 1}"
    else:
        ver = base

    for path in (args.state, args.header):
        d = os.path.dirname(os.path.abspath(path))
        if d:
            os.makedirs(d, exist_ok=True)

    with open(args.state, "w", encoding="utf-8", newline="\n") as f:
        f.write(ver + "\n")
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/* auto-generated — do not edit */\n"
            "#pragma once\n"
            f'#define ESP_FW_VERSION_STR "{ver}"\n'
        )
    print(f"Firmware version: {ver}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
