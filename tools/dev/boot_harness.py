#!/usr/bin/env python3
"""Reset the board N times in a row and check every boot: the core firmware
runs the build that was flashed, at the low-power profile, and (debug builds)
how long it takes from its banner to the HD6301 emulator's loop.

Each boot is a watchdog-style reset, so the figures do not include the crystal
startup a cold start goes through. MODE must not be 255 (configuration), or
the core firmware jumps straight into Booster. Release and debug builds both
work; with a debug build `console.py watch` must run and every boot's banner
is checked.

    python3 tools/dev/boot_harness.py --boots 20
    python3 tools/dev/boot_harness.py --boots 20 --flash souffle release

Exits 0 when every boot passes; writes tools/dev/logs/boot-harness-<time>.json.
"""

import argparse
import datetime
import re
import statistics
import sys

from harnesslib import (LogCursor, check, flash, report, running_build, swd,
                        swdpy)

STAMP = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ")


def stamp(line: str | None) -> datetime.datetime | None:
    m = STAMP.match(line or "")
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--boots", type=int, default=10)
    ap.add_argument("--flash", nargs=2, metavar=("FLAVOR", "TYPE"),
                    help="flash this board flavor and build type first")
    args = ap.parse_args()
    if args.flash and not flash(*args.flash):
        return report("boot-harness")

    app, kind, build_id = running_build()
    if not check("the core firmware runs", app == "core",
                 f"{app} {kind}, build {build_id}"):
        return report("boot-harness")
    elf = swd.running_elf(None)
    debug = kind == "debug"
    emu_ms = []
    for boot in range(1, args.boots + 1):
        cursor = LogCursor()
        rc, _ = swdpy("reset")
        ok = rc == 0
        rc, _ = swdpy("running", elf, "--timeout", "20")
        ok &= rc == 0
        detail = []
        if debug:
            banner = cursor.wait(r"SidecarTridge IKBD core\. .*Build "
                                 + re.escape(build_id), 20)
            emu = cursor.wait("Entering HD6301 core loop", 20)
            ok &= banner is not None and emu is not None
            if banner and emu:
                ms = (stamp(emu) - stamp(banner)).total_seconds() * 1000
                emu_ms.append(ms)
                detail.append(f"banner to HD6301 loop {ms:.0f} ms")
        rc, clock = swdpy("clock")
        ok &= rc == 0
        detail.append("96 MHz / 1.00 V" if rc == 0 else "NOT the low-power profile")
        check(f"boot {boot}: the core firmware runs, build {build_id}", ok,
              ", ".join(detail))
    extra = {"build": build_id, "banner_to_hd6301_loop_ms": emu_ms}
    if emu_ms:
        print(f"\nbanner to HD6301 loop: min {min(emu_ms):.0f} ms, median "
              f"{statistics.median(emu_ms):.0f} ms, max {max(emu_ms):.0f} ms "
              f"over {len(emu_ms)} boots")
    return report("boot-harness", extra)


if __name__ == "__main__":
    sys.exit(main())
