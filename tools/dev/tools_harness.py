#!/usr/bin/env python3
"""Exercise every tools/dev command against the running core firmware and say,
check by check, whether it behaves.

Needs the core firmware on the RP (a build from tools/dev/flash.sh, so a cached
ELF matches it), MODE other than 255 (with 255 the core firmware jumps straight
into Booster), the Debug Probe, and, for a debug build, `tools/dev/console.py
watch` running: it reads the console log, never the UART. The Atari need not be
connected. The RP is reset on the way; the settings are not changed. The
joystick lines are driven low and released again only on a Souffle build.

    python3 tools/dev/tools_harness.py                    # against what runs
    python3 tools/dev/tools_harness.py --flash souffle    # flash the debug build first

Exits 0 when every check passes; writes tools/dev/logs/tools-harness-<time>.json.
"""

import argparse
import os
import re
import sys

from harnesslib import (LogCursor, check, flash, report, running_build, skip,
                        swd, swdpy)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--flash", metavar="FLAVOR", choices=("souffle", "croissant"),
                    help="flash the debug build for this board flavor first")
    args = ap.parse_args()
    if args.flash and not flash(args.flash, "debug"):
        return report("tools-harness")

    app, kind, build_id = running_build()
    if not check("the core firmware runs, from a cached ELF", app == "core",
                 f"{app} {kind}, build {build_id}"):
        return report("tools-harness")
    debug = kind == "debug"
    elf = swd.running_elf(None)
    souffle = "-souffle-" in os.path.basename(elf)
    rc, out = swdpy("build-id")
    check("build-id: the core firmware's matches its cached ELF",
          rc == 0 and f"core: {build_id}" in out,
          " / ".join(out.strip().splitlines()))
    rc, out = swdpy("verify", elf)
    check("verify: the flash matches the ELF", rc == 0)

    cursor = LogCursor()
    rc, _ = swdpy("reset")
    if debug:
        banner = cursor.wait(r"SidecarTridge IKBD core\. .*Build " +
                             re.escape(build_id), 20)
        check("reset, and the console shows the boot banner with the build ID",
              rc == 0 and banner is not None,
              "" if banner else "is console.py watch running?")
        check("the HD6301 emulator starts", cursor.wait(
            "Entering HD6301 core loop", 20) is not None)
    else:
        check("reset", rc == 0)
    rc, out = swdpy("running", elf, "--timeout", "20")
    check("running: the core firmware ELF", rc == 0, out.strip())

    for name, args_, expect in (
            ("board", ("board",), r"running: core"),
            ("settings", ("settings",), r"^MODE=\d+$"),
            ("clock", ("clock",), r"low-power profile .*: yes"),
            ("gpio", ("gpio",), r"^joystick lines low: \d+ of 12$"),
            ("heap", ("heap",), r"heap |does not link malloc"),
            ("crash", ("crash",), r"last reset: the watchdog, forced")):
        rc, out = swdpy(*args_)
        check(name, rc == 0 and re.search(expect, out, re.M) is not None,
              out.strip().splitlines()[0] if out.strip() else "")
    rc, out = swdpy("settings")
    check("settings: the Wi-Fi password is masked",
          rc == 0 and re.search(r"^WIFI_PASSWORD=(\*+)?$", out, re.M) is not None)
    rc, out = swdpy("factory")
    check("factory: decodes the factory keys, or says they are absent",
          rc in (0, 3) and "FACTORY" in out, out.strip().splitlines()[0])

    rc, out = swdpy("gpio")
    if not souffle:
        skip("drive-low and release", "Souffle only")
    elif rc or "joystick lines low: 0 of 12" not in out:
        # Lines already low (a plugged harness or joystick) would make the
        # check pass whether or not drive-low works.
        skip("drive-low and release", "some joystick lines already read low: "
             "unplug the joystick ports")
    else:
        rc, _ = swdpy("drive-low", "all")
        rc2, out = swdpy("gpio")
        check("drive-low all: the twelve joystick lines read low",
              rc == 0 and rc2 == 0 and "joystick lines low: 12 of 12" in out)
        rc, _ = swdpy("release", "all")
        rc2, out = swdpy("gpio")
        check("release all: the lines read high again, none driven over SWD",
              rc == 0 and rc2 == 0 and "joystick lines low: 0 of 12" in out
              and "driven low over SWD" not in out)

    mode = swd.settings_value(swd.read_settings(), "MODE")
    if mode == "255":
        skip("boot booster, then boot ikbd", "MODE is 255: the core firmware "
             "goes straight back into Booster")
    else:
        rc, out = swdpy("boot", "booster")
        check("boot booster: CONFIG pressed over SWD, Booster runs", rc == 0,
              out.strip())
        rc, out = swdpy("boot", "ikbd")
        check("boot ikbd: the core firmware runs again", rc == 0, out.strip())

    rc, out = swdpy("postmortem", timeout=120)
    check("postmortem: backtraces, then both cores released",
          rc == 0 and "=== backtraces" in out and "both cores released" in out)
    rc, out = swdpy("board")
    check("both cores run after the postmortem",
          rc == 0 and "rp2350.cm0 running, rp2350.cm1 running" in out)
    return report("tools-harness", {"build": build_id})


if __name__ == "__main__":
    sys.exit(main())
