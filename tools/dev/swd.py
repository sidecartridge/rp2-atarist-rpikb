#!/usr/bin/env python3
"""
swd — read, check and drive a running SidecarTridge IKBD board (Souffle or
Croissant, Pico 2 W / RP2350) through the Raspberry Pi Debug Probe.

Single-file, stdlib-only (Python >= 3.10). Runs OpenOCD with the CMSIS-DAP
probe and target/rp2350.cfg for each command. It never uses the firmware's
own services: memory is read through the debug port while the CPU keeps
running, so it works on a release build as on a debug one, and on a hung RP.
Ported from Booster's tools/dev (itself from sidecartos's).

Usage:
    python3 tools/dev/swd.py board
    python3 tools/dev/swd.py running ELF [--timeout S]
    python3 tools/dev/swd.py verify ELF...
    python3 tools/dev/swd.py build-id [ELF ...]
    python3 tools/dev/swd.py read ADDRESS LENGTH OUTFILE
    python3 tools/dev/swd.py program ELF...
    python3 tools/dev/swd.py reset
    python3 tools/dev/swd.py resume
    python3 tools/dev/swd.py crash [--elf ELF]
    python3 tools/dev/swd.py heap [--elf ELF] [--watch SECONDS] [--csv FILE]
    python3 tools/dev/swd.py postmortem [--elf ELF] [--leave-halted]
    python3 tools/dev/swd.py settings [set KEY=VALUE ...] [--show-secrets]
    python3 tools/dev/swd.py boot booster|ikbd [--timeout S]
    python3 tools/dev/swd.py clock
    python3 tools/dev/swd.py gpio
    python3 tools/dev/swd.py drive-low GPIO...|all
    python3 tools/dev/swd.py release GPIO...|all
    python3 tools/dev/swd.py factory

The flash holds two images: the core firmware (rp2-atarist-rpikb) at
0x10000000 and Booster at 0x10120000. The core firmware always starts first
and enters Booster on MODE 255 or the CONFIG button; both put their RAM vector table at 0x20000000, so VTOR cannot
tell them apart. Which one runs is read from core 0's program counter,
sampled through the DWT while the CPU keeps running: samples inside one
image's flash and none inside the other's decide.

`board` says which chip and which image run, with its build ID. `running`
waits until the RP runs the ELF's image past its startup code. `verify`
compares the flash with each ELF. `build-id` reads the `release_build_id`
string from flash: with ELFs, at the address each ELF puts it; with none, for
each image from the cached ELFs in tools/dev/builds/elf, or by scanning the
image's flash when no cached ELF matches (a product UF2 flashed by other
means). `program` flashes the ELFs (only their own flash: the config sectors,
the settings and the Bluetooth keys are kept) and resets the chip; `reset`
only resets it; `resume` releases both cores after a debugger left them
halted.

`crash` explains the last reset: the power manager's reset causes, the
watchdog's reason and its scratch registers (scratch 4-7 the SDK's reboot
vector), resolving code addresses to source lines. `heap` reads newlib's
malloc state: the heap's size up to __HeapLimit, the arena, the peak and the
free blocks. `postmortem` halts the RP, prints both cores' backtraces and
registers through GDB, then resumes it. Halting stops Wi-Fi, Bluetooth and
the Atari link until the RP resumes. Commands that need symbols use the cached
ELF of the running image whose build ID the RP carries, unless --elf is given,
so they need a build from tools/dev/flash.sh or --elf. The core firmware's
release ELF keeps its symbols; Booster's shipped one is stripped.

`settings` decodes the settings sector both firmwares share (0x101FD000):
127-byte entries (30-byte key, 1-byte type, 96-byte value), MAGICVERSION
first; the Wi-Fi password is masked unless --show-secrets. `settings set`
rewrites entries (existing keys only, values checked against their type),
verifies the sector and resets the chip, because the running firmware keeps
its own copy in RAM. `boot booster` presses the CONFIG button over SWD
(drives GPIO 9 high through SIO, then releases it): every mode loop of the core
firmware jumps into Booster on it, without touching the settings. It does not
write BOOT_FEATURE: the core firmware treats any value but IKBD as a mismatch
and erases all settings. `boot ikbd` resets the chip; the core firmware then
stays unless MODE is 255 (configuration), which sends it straight to Booster.
`clock` decodes PLL_SYS, the core voltage and the flash interface and checks
the low-power profile (96 MHz, 1.00 V). `gpio` reads the pin levels through
SIO without symbols: the CONFIG button (GPIO 9) and Souffle's twelve joystick
and mouse lines (GPIO 10-21, low = pressed). `drive-low` drives joystick lines
low as outputs over SWD, so the firmware reads them pressed with nothing
plugged in (how the factory test is run without its harness), and `release`
hands them back; driving low is used instead of switching the pads to
pull-down, which erratum RP2350-E9 can leave reading high. Souffle only:
Croissant uses these GPIOs differently (refused when a cached ELF says the
running build is a Croissant one). `factory` decodes the factory test's
settings, FACTORY_MODE and FACTORY_RESULT.

Three rules, from the siblings:

- No OpenOCD work area except while writing flash. Its RP2350 default, 64 KB
  at 0x20010000, is in the running firmware's RAM; every run that does not
  write flash disables it, and `verify_image` then compares on the host.
- Reset through the watchdog, never with OpenOCD's `reset`: a watchdog-style
  reset restarts both cores together and leaves PIO and DMA stopped, as a
  power-on does.
- Flash only with both cores halted and every PIO state machine and DMA
  channel stopped: the core firmware's USB host and Atari link run on PIO and
  DMA, which halting the cores does not stop.

OpenOCD is $OPENOCD, `openocd` on PATH, or ../pico/openocd/src/openocd (the
Raspberry Pi fork with the rp2350.cm0/cm1 target names); its scripts come
from $PICO_OPENOCD_PATH. Close any VS Code debug session first: only one
program can use the probe.

Exit codes: 0 success, 1 error, 2 usage, 3 the check failed (not running,
flash differs, no build ID).
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ELF_CACHE = os.path.join(HERE, "builds", "elf")

CORES = ("rp2350.cm0", "rp2350.cm1")
VTOR = 0xE000ED08
DHCSR = 0xE000EDF0
DHCSR_S_HALT = 1 << 17
DHCSR_RELEASE = 0xA05F0000  # DBGKEY; clears C_HALT and C_DEBUGEN
DWT_PCSR = 0xE000101C       # core 0's program counter, sampled while it runs
BUILD_ID_SYMBOL = "release_build_id"

# RP2350 register map (pico-sdk src/rp2350/hardware_regs).
PSM_WDSEL = 0x40018008
PSM_WDSEL_ALL_BUT_OSCILLATORS = 0x01FFFFF3  # what watchdog_reboot() selects
WATCHDOG_CTRL = 0x400D8000
WATCHDOG_CTRL_TRIGGER = 0x80000000
DMA_BASE = 0x50000000
DMA_CHANNELS = 16
DMA_AL1_CTRL = 0x10      # CTRL alias that does not trigger the channel
DMA_CHAN_ABORT = 0x50000464
PIO_CTRL = (0x50200000, 0x50300000, 0x50400000)

# Each image's flash (memmap_booster_rp2350.ld and the core firmware's
# memmap_rp2-ikbd_rp2350.ld): start, end.
IMAGES = {"core": (0x10000000, 0x10120000), "booster": (0x10120000, 0x101DE000)}
APP_FLASH = {name: span[0] for name, span in IMAGES.items()}

SYSINFO_CHIP_ID = 0x40000000
SYSINFO_PACKAGE_SEL = 0x40000004  # 1 = QFN-60 (RP2350A), 0 = QFN-80; wrong on A2
OTP_NUM_GPIOS = 0x40130030        # OTP row 0x18, ECC window: 30 = QFN-60, 48 = QFN-80
WATCHDOG_REASON = 0x400D8008      # followed by SCRATCH0-7
POWMAN_CHIP_RESET = 0x4010002C
CHIP_RESET_CAUSES = (            # POWMAN CHIP_RESET HAD_* bits
    (1 << 28, "watchdog (RSM)"), (1 << 27, "SYSRESETREQ (hazard)"),
    (1 << 26, "glitch detector"), (1 << 25, "switched-core power-down"),
    (1 << 24, "watchdog (switched core)"), (1 << 23, "watchdog (power manager)"),
    (1 << 22, "watchdog (power manager, async)"), (1 << 21, "debugger rescue"),
    (1 << 19, "debug port reset request"), (1 << 18, "RUN pin low"),
    (1 << 17, "brown-out"), (1 << 16, "power-on"))
SDK_REBOOT_MAGIC = 0xB007C0D3    # watchdog_reboot() with a PC: scratch 4
GDB_PORT = 3333
# The firmware's state, printed by postmortem when the ELF has them.
POSTMORTEM_VARIABLES: tuple[str, ...] = ()


class SwdError(Exception):
    pass


def openocd_command(work_area: bool = False, core0_only: bool = False) -> list[str]:
    ocd = os.environ.get("OPENOCD") or shutil.which("openocd")
    if not ocd:
        local = os.path.join(REPO, "..", "pico", "openocd", "src", "openocd")
        if os.access(local, os.X_OK):
            ocd = local
    if not ocd:
        raise SwdError("no openocd: set OPENOCD")
    cmd = [ocd]
    scripts = os.environ.get("PICO_OPENOCD_PATH") or os.path.join(
        os.path.dirname(os.path.realpath(ocd)), "..", "tcl")
    if os.path.isfile(os.path.join(scripts, "interface", "cmsis-dap.cfg")):
        cmd += ["-s", scripts]
    if core0_only:
        # Core 0 alone, without the SMP pairing: halt and resume act on it only.
        cmd += ["-c", "set USE_CORE cm0"]
    cmd += ["-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg",
            "-c", "adapter speed 5000"]
    # The default work area (64 KB at 0x20010000) is in the running
    # firmware's RAM. Only a flash write, with the cores halted, PIO and DMA
    # stopped (quiesce_commands) and a reset after, may use it.
    if not work_area:
        cmd += ["-c", f"{CORES[0]} configure -work-area-size 0"]
    return cmd


# The debug port can drop for a moment, for example while the firmware changes
# the clock and core voltage early in boot. OpenOCD then reports one of these.
TRANSIENT = re.compile(r"Failed to read memory|Error connecting DP|"
                       r"Examination failed|DP initialisation failed")
ATTEMPTS = 4


def openocd(*commands: str, check: bool = True, work_area: bool = False,
            core0_only: bool = False) -> str:
    """Run OpenOCD with `init`, the commands and `exit`; return its output.
    A run that failed on a transient debug-port error is repeated."""
    args = openocd_command(work_area, core0_only) + ["-c", "init"]
    for c in commands:
        args += ["-c", c]
    args += ["-c", "exit"]
    for attempt in range(ATTEMPTS):
        proc = subprocess.run(args, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        failed = proc.returncode != 0 or "Failed to read memory" in out
        if not (failed and TRANSIENT.search(out)) or attempt == ATTEMPTS - 1:
            break
        print(f"swd: transient debug-port error, retrying ({attempt + 1}/"
              f"{ATTEMPTS - 1})", file=sys.stderr)
        time.sleep(0.5)
    if check and proc.returncode != 0:
        lines = [l for l in out.splitlines()
                 if l.startswith("Error") and "algo" not in l]
        raise SwdError("openocd failed: " + (" / ".join(lines[-3:]) or
                                             out.strip()[-300:]))
    return out


# Reading the flash's memory-mapped window over SWD while the firmware erases
# or programs the flash wedges the QMI: the bootrom drives the flash in direct
# mode then, and a debug read landing meanwhile left the direct transfer BUSY
# for good, the firmware stuck in the bootrom with interrupts off (seen in
# sidecartos, 2026-10-08). Booster writes the flash when it saves settings or
# Bluetooth keys, the core firmware when it saves settings. So a read that
# touches the window first halts core 0, which does those writes, waits until
# the QMI is out of direct mode, reads, and resumes it as it was. The guarded run attaches
# to core 0 alone (USE_CORE): with the SMP pairing, halt and resume act on both
# cores and left core 1 halted.
QMI_DIRECT_CSR = 0x400D0000  # bit 0: direct mode enabled
XIP_WINDOW = (0x10000000, 0x20000000)


def touches_flash(ranges) -> bool:
    return any(a < XIP_WINDOW[1] and a + max(n, 1) > XIP_WINDOW[0]
               for a, n in ranges)


def flash_guard(commands: list[str]) -> list[str]:
    """`commands` wrapped so they run with core 0 halted outside a flash
    operation. Run them with `openocd(..., core0_only=True)`."""
    return [
        f"targets {CORES[0]}",
        "set swd_was_halted [expr {([lindex [read_memory 0x%08x 32 1] 0] >> 17)"
        " & 1}]" % DHCSR,
        "if {!$swd_was_halted} { halt 100 }",
        "set swd_ok 0",
        "for {set i 0} {$i < 100} {incr i} { if {([lindex [read_memory "
        "0x%08x 32 1] 0] & 1) == 0} { set swd_ok 1; break }; resume; "
        "sleep 10; halt 100 }" % QMI_DIRECT_CSR,
        "if {!$swd_ok} { if {!$swd_was_halted} { resume }; "
        "error \"the flash stays in direct mode: not read\" }",
        *commands,
        f"targets {CORES[0]}",
        "if {!$swd_was_halted} { resume; mww 0x%08x 0x%08x }"
        % (DHCSR, DHCSR_RELEASE),
    ]


def read_word(address: int, core: str = CORES[0]) -> int:
    cmds = [f"targets {core}", f"mdw 0x{address:08x}"]
    guarded = touches_flash([(address, 4)])
    out = openocd(*(flash_guard(cmds) if guarded else cmds), core0_only=guarded)
    m = re.search(rf"0x{address:08x}:\s+([0-9a-fA-F]{{8}})", out)
    if not m:
        raise SwdError(f"cannot read 0x{address:08x}")
    return int(m.group(1), 16)


def read_memory(address: int, length: int) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "dump.bin")
        cmds = [f"dump_image {path} 0x{address:08x} {length}"]
        guarded = touches_flash([(address, length)])
        openocd(*(flash_guard(cmds) if guarded else cmds), core0_only=guarded)
        with open(path, "rb") as f:
            data = f.read()
    if len(data) != length:
        raise SwdError(f"read {len(data)} of {length} bytes")
    return data


def elf_symbols(elf: str, *names: str) -> dict[str, tuple[int, int]]:
    """Address and size of each named symbol present in the ELF."""
    nm = shutil.which("arm-none-eabi-nm")
    if not nm:
        raise SwdError("arm-none-eabi-nm not on PATH")
    out = subprocess.run([nm, "-S", elf], capture_output=True, text=True,
                         check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in names:
            found[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        elif len(parts) == 3 and parts[2] in names:
            found.setdefault(parts[2], (int(parts[0], 16), 0))
    return found


def elf_build_id(elf: str) -> str | None:
    """The build ID string stored in the ELF file itself."""
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym:
        return None
    headers = subprocess.run(["arm-none-eabi-objdump", "-h", elf],
                             capture_output=True, text=True, check=True).stdout
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
                         headers, re.M):
        name, size, vma = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if vma <= sym[0] < vma + size:
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "section.bin")
                subprocess.run(["arm-none-eabi-objcopy", "-O", "binary",
                                f"--only-section={name}", elf, path],
                               check=True)
                with open(path, "rb") as f:
                    f.seek(sym[0] - vma)
                    data = f.read(sym[1])
            return data.split(b"\0", 1)[0].decode("ascii", errors="replace")
    return None


def read_build_id(elf: str) -> str | None:
    """The build ID string in flash, at the address this ELF puts it."""
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym or sym[1] == 0:
        return None
    data = read_memory(sym[0], sym[1])
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def elf_head_in_flash(elf: str, length: int = 4096) -> bool:
    """True when the first bytes of the ELF's image are what flash holds. A
    build ID string alone can match a stale copy: a smaller build flashed over
    a larger one leaves the larger one's string intact past its own end. 4 KB,
    because the vector table and crt0 at the start are the same in every
    build type."""
    start = elf_symbols(elf, "__flash_binary_start").get("__flash_binary_start")
    if not start:
        return False
    headers = subprocess.run(["arm-none-eabi-objdump", "-h", elf],
                             capture_output=True, text=True, check=True).stdout
    sections = sorted((int(m.group(3), 16), int(m.group(2), 16), m.group(1))
                      for m in re.finditer(
                          r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
                          headers, re.M))
    for vma, size, name in sections:
        if size and start[0] <= vma < start[0] + 0x20000:
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "head.bin")
                subprocess.run(["arm-none-eabi-objcopy", "-O", "binary",
                                f"--only-section={name}", elf, path], check=True)
                with open(path, "rb") as f:
                    want = f.read(length)
            return read_memory(vma, len(want)) == want
    return False


def cached_elfs(app: str | None = None) -> list[str]:
    """Cached ELFs, newest first; only one app's when `app` is given."""
    pattern = f"{app}-*.elf" if app else "*.elf"
    return sorted(glob.glob(os.path.join(ELF_CACHE, pattern)),
                  key=os.path.getmtime, reverse=True)


def matching_elf(explicit: str | None, app: str = "core") -> str:
    """The ELF given, or the cached ELF of `app` whose build ID the RP
    carries in flash."""
    if explicit:
        return explicit
    for elf in cached_elfs(app):
        expected = elf_build_id(elf)
        if (expected and read_build_id(elf) == expected
                and elf_head_in_flash(elf)):
            return elf
    raise SwdError(f"no cached {app} ELF matches the RP's build ID: "
                   "flash with tools/dev/flash.sh or pass --elf")


def resolve_address(elf: str, address: int) -> str:
    """`function at file:line` for a code address, or '' when not code."""
    if not (0x10000000 <= address < 0x11000000 or
            0x20000000 <= address < 0x20082000):
        return ""
    out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-i", "-p", "-e",
                          elf, f"0x{address & ~1:08x}"],
                         capture_output=True, text=True).stdout.strip()
    return "" if out.startswith("??") else out.replace("\n", "; ")


HEAP_SYMBOLS = ("end", "__HeapLimit", "__StackLimit", "heap_end.0",
                "__malloc_sbrk_base", "__malloc_max_sbrked_mem", "__malloc_av_")


def heap_snapshot(elf: str) -> dict:
    """Heap figures read from newlib's own malloc state, no firmware help.

    The heap grows by sbrk from `end` towards __HeapLimit, the end of the
    app's RAM window, and never gives memory back, so __malloc_max_sbrked_mem
    is the peak it ever reached. Free space inside it comes from walking the
    chunks: each chunk's size word carries PREV_INUSE for the chunk before
    it, and the walk ends at the top chunk (av_[2]). The walk reads RAM while
    the CPU runs, so a chunk that changes mid-read can break it: `walk_ok` is
    False then and the caller may simply read again.

    The SDK's sbrk stops at __StackLimit, not __HeapLimit; when a linker
    script puts __StackLimit beyond __HeapLimit, an overflow there would not
    fail. `sbrk_beyond` reports it."""
    sym = elf_symbols(elf, *HEAP_SYMBOLS)
    if "__malloc_av_" not in sym:
        return {"no_malloc": True}
    missing = [n for n in HEAP_SYMBOLS if n not in sym]
    if missing:
        raise SwdError(f"{os.path.basename(elf)} lacks {', '.join(missing)}")
    start, limit = sym["end"][0], sym["__HeapLimit"][0]
    brk, base, peak, top = struct.unpack("<4I", b"".join(
        read_memory(sym[n][0] + off, 4) for n, off in
        (("heap_end.0", 0), ("__malloc_sbrk_base", 0),
         ("__malloc_max_sbrked_mem", 0), ("__malloc_av_", 8))))
    snap = {"size": limit - start, "arena": (brk - start) if brk else 0,
            "peak": peak, "headroom": limit - (brk or start),
            "peak_headroom": limit - start - peak, "walk_ok": False,
            "sbrk_beyond": sym["__StackLimit"][0] - limit}
    if not brk:
        snap["unused"] = True  # sbrk never called: nothing allocated yet
        return snap
    if base in (0, 0xFFFFFFFF) or not (base <= top < brk):
        return snap
    heap = read_memory(base, brk - base)

    def word(addr: int) -> int:
        return struct.unpack_from("<I", heap, addr - base)[0]

    used = free = largest = free_chunks = 0
    # The first chunk is not always at the sbrk base: malloc moves it forward
    # so that its user pointer (chunk + 8) is 8-byte aligned.
    p = base + (-(base + 8) & 7)
    while p < top:
        size = word(p + 4) & ~3
        if size < 16 or p + size > top:
            return snap
        if word(p + size + 4) & 1:
            used += size
        else:
            free += size
            free_chunks += 1
            largest = max(largest, size)
        p += size
    top_size = word(top + 4) & ~3
    if p != top or top + top_size > brk + 16:
        return snap
    snap.update(walk_ok=True, used=used, free=free + top_size,
                largest=max(largest, top_size), free_chunks=free_chunks)
    return snap


def heap_line(snap: dict) -> str:
    kb = lambda n: f"{n / 1024:.1f} KB"
    line = (f"heap {kb(snap['size'])}: arena {kb(snap['arena'])}, "
            f"peak {kb(snap['peak'])} (closest to the limit: "
            f"{kb(snap['peak_headroom'])}), never used {kb(snap['headroom'])}")
    if snap.get("unused"):
        line += "; malloc has not been called yet"
    elif snap["walk_ok"]:
        line += (f"; in use {kb(snap['used'])}, free in arena "
                 f"{kb(snap['free'])} in {snap['free_chunks'] + 1} blocks, "
                 f"largest {kb(snap['largest'])}")
    else:
        line += "; chunk walk failed (heap changed while reading)"
    return line


def cmd_heap(args: argparse.Namespace) -> int:
    elf = running_elf(args.elf)
    csv = None
    if args.csv:
        new = not os.path.exists(args.csv)
        csv = open(args.csv, "a")
        if new:
            csv.write("time,arena,peak,peak_headroom,headroom,used,free,"
                      "largest,free_blocks\n")
    try:
        warned = False
        while True:
            snap = heap_snapshot(elf)
            if snap.get("no_malloc"):
                print(f"{os.path.basename(elf)} does not link malloc: no heap")
                return 0
            if not snap["walk_ok"] and not snap.get("unused"):
                snap = heap_snapshot(elf)
            print(time.strftime("%H:%M:%S ") + heap_line(snap), flush=True)
            if snap["sbrk_beyond"] > 0 and not warned:
                print(f"  note: sbrk stops at __StackLimit, "
                      f"{snap['sbrk_beyond'] / 1024:.0f} KB past __HeapLimit: "
                      "a heap overflow would not fail", flush=True)
                warned = True
            if csv:
                csv.write(",".join([time.strftime("%Y-%m-%d %H:%M:%S")] + [
                    str(snap.get(k, "")) for k in
                    ("arena", "peak", "peak_headroom", "headroom", "used",
                     "free", "largest")] +
                    [str(snap["free_chunks"] + 1) if snap["walk_ok"] else ""])
                    + "\n")
                csv.flush()
            if not args.watch:
                return 0
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0
    finally:
        if csv:
            csv.close()


def gdb_command() -> str:
    gdb_dir = os.environ.get("ARM_GDB_PATH")
    candidates = [os.path.join(gdb_dir, "bin", "arm-none-eabi-gdb")] if gdb_dir else []
    candidates.append(shutil.which("arm-none-eabi-gdb") or "")
    for c in candidates:
        if c and os.access(c, os.X_OK):
            return c
    raise SwdError("no arm-none-eabi-gdb: set ARM_GDB_PATH")


def cmd_postmortem(args: argparse.Namespace) -> int:
    elf = running_elf(args.elf)
    gdb = gdb_command()
    # No GDB memory map: to build one, OpenOCD probes the flash on connect
    # with a routine in its work area, which here is disabled. GDB needs no
    # memory map to read the stacks and variables.
    server = openocd_command() + [
        "-c", f"gdb_port {GDB_PORT}", "-c", "tcl_port disabled",
        "-c", "telnet_port disabled", "-c", "gdb_memory_map disable"]
    if args.leave_halted:
        # OpenOCD resumes the target when GDB detaches, unless told not to.
        for core in CORES:
            server += ["-c", f"{core} configure -event gdb-detach {{}}"]
    proc = subprocess.Popen(server, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 10
        ready = False
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if f"port {GDB_PORT}" in line:
                ready = True
                break
        if not ready:
            raise SwdError("OpenOCD did not start its GDB server")
        present = elf_symbols(elf, *POSTMORTEM_VARIABLES)
        gdb_cmds = ["set pagination off", "set confirm off",
                    "set print pretty on",
                    f"target extended-remote localhost:{GDB_PORT}",
                    "monitor halt",
                    "echo \\n=== threads (one per core)\\n", "info threads",
                    "echo \\n=== backtraces\\n", "thread apply all bt",
                    "echo \\n=== registers (current core)\\n",
                    "info registers",
                    "echo \\n=== watchdog reason and scratch 0-7\\n",
                    f"x/9xw 0x{WATCHDOG_REASON:08x}"]
        if present:
            gdb_cmds.append("echo \\n=== variables\\n")
            for name in POSTMORTEM_VARIABLES:
                if name in present:
                    gdb_cmds += [f"echo {name} = ", f"output {name}",
                                 "echo \\n"]
        if not args.leave_halted:
            gdb_cmds.append("monitor resume")
        gdb_cmds.append("detach")
        argv = [gdb, "-nx", "-batch", elf]
        for c in gdb_cmds:
            argv += ["-ex", c]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        print(out.stdout.rstrip())
        errors = [l for l in out.stderr.splitlines()
                  if l.strip() and "warning" not in l.lower()]
        if errors:
            print("\n".join(errors[-5:]), file=sys.stderr)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if args.leave_halted:
        halted = [c for c in CORES if read_word(DHCSR, c) & DHCSR_S_HALT]
        print("\nleft halted: " + (", ".join(halted) or "nothing") +
              " (swd.py resume to continue)")
        return 0
    # OpenOCD leaves debug mode enabled (C_DEBUGEN) after GDB detaches; clear
    # it on both cores so the RP runs exactly as before the halt.
    return cmd_resume(args)


def cmd_verify(args: argparse.Namespace) -> int:
    status = 0
    for elf in args.elf:
        # No work area: OpenOCD warns that it cannot checksum on the target
        # and compares on the host instead.
        out = openocd(*flash_guard([f"verify_image {elf}"]), check=False,
                      core0_only=True)
        m = re.search(r"verified (\d+) bytes in ([\d.]+)s", out)
        if m:
            print(f"flash matches {elf} ({m.group(1)} bytes)")
            continue
        diffs = len(re.findall(r"^diff \d+ address", out, re.M))
        if diffs:
            print(f"flash differs from {elf} (at least {diffs} bytes)",
                  file=sys.stderr)
            status = 3
            continue
        raise SwdError("verify failed: " + out.strip()[-300:])
    return status


def cmd_read(args: argparse.Namespace) -> int:
    data = read_memory(int(args.address, 0), int(args.length, 0))
    with open(args.outfile, "wb") as f:
        f.write(data)
    print(f"read {len(data)} bytes from {args.address} into {args.outfile}")
    return 0


def release_cores() -> None:
    commands = []
    for core in CORES:
        commands += [f"targets {core}", f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}"]
    openocd(*commands)


def cmd_resume(args: argparse.Namespace) -> int:
    """Release both cores from a debug halt and leave debug mode."""
    release_cores()
    states = [read_word(DHCSR, core) & DHCSR_S_HALT for core in CORES]
    if any(states):
        print("still halted: " + ", ".join(
            c for c, h in zip(CORES, states) if h), file=sys.stderr)
        return 3
    print("both cores released")
    return 0


def chip_reset() -> None:
    """Reset the whole chip at once, the way the watchdog does.

    Not OpenOCD's own `reset`: its multi-core sequence touches core 1 again
    after core 0 has started. A watchdog-style reset restarts both cores
    together, like a power-on reset, with PIO and DMA stopped. OpenOCD exits
    with an error on this run, as the chip resets under it."""
    openocd(f"mww 0x{PSM_WDSEL:08x} 0x{PSM_WDSEL_ALL_BUT_OSCILLATORS:08x}",
            f"mww 0x{WATCHDOG_CTRL:08x} 0x{WATCHDOG_CTRL_TRIGGER:08x}",
            check=False)
    time.sleep(0.5)
    # `program` leaves the cores halted and every OpenOCD run leaves debug
    # mode on; release both so the RP runs exactly as after a power-on.
    try:
        release_cores()
    except SwdError:
        pass  # the debug port can blink while the chip restarts


def quiesce_commands() -> list[str]:
    """Halt both cores, then stop every PIO state machine and DMA channel.

    Halting the cores does not stop PIO and DMA, which the core firmware's
    USB host and Atari link use. PIO first, so no new DREQ arrives; then
    clear each channel's enable without triggering it, then abort whatever is
    in flight."""
    cmds = []
    for core in CORES:
        cmds += [f"targets {core}", "halt"]
    cmds += [f"mww 0x{ctrl:08x} 0" for ctrl in PIO_CTRL]
    cmds += [f"mww 0x{DMA_BASE + 0x40 * n + DMA_AL1_CTRL:08x} 0"
             for n in range(DMA_CHANNELS)]
    cmds += [f"mww 0x{DMA_CHAN_ABORT:08x} 0x{(1 << DMA_CHANNELS) - 1:x}",
             f"targets {CORES[0]}"]
    return cmds


def cmd_reset(args: argparse.Namespace) -> int:
    chip_reset()
    print("chip reset (watchdog-style, both cores together)")
    return 0


def qfn60(package_sel: int, num_gpios: int) -> bool:
    """Whether the chip is the QFN-60 package (the RP2350A board). The OTP's
    GPIO count decides: on A2 chips PACKAGE_SEL reads 0 whatever the package,
    as picotool notes; PACKAGE_SEL only when the OTP row reads neither count."""
    count = num_gpios & 0xFF
    if count in (30, 48):
        return count == 30
    return bool(package_sel & 1)


# --- Which image runs -----------------------------------------------------------

def elf_app(elf: str) -> str:
    """'core' or 'booster', from where the image starts in flash."""
    start = elf_symbols(elf, "__flash_binary_start").get("__flash_binary_start")
    for app, address in APP_FLASH.items():
        if start and start[0] == address:
            return app
    raise SwdError(f"{elf}: not a core firmware or Booster image")


def image_of(address: int) -> str | None:
    """The image whose flash holds `address`, 'ram', or None."""
    for name, (start, end) in IMAGES.items():
        if start <= address < end:
            return name
    if 0x20000000 <= address < 0x20082000:
        return "ram"
    return None


PC_SAMPLES = 32


def sample_pcs(count: int = PC_SAMPLES) -> list[int]:
    """Core 0's program counter, sampled `count` times while it runs."""
    out = openocd(f"targets {CORES[0]}",
                  *[f"mdw 0x{DWT_PCSR:08x}"] * count)
    return [int(w, 16) for w in re.findall(
        rf"^0x{DWT_PCSR:08x}:\s+([0-9a-fA-F]{{8}})", out, re.M)]


def running_image() -> tuple[str | None, str]:
    """The image core 0 runs, and how the samples fell. Code in RAM (the
    SDK's and Booster's __not_in_flash_func functions) and a sleeping core
    say nothing; one image's flash with none in the other's decides."""
    counts = {"core": 0, "booster": 0, "ram": 0, None: 0}
    for pc in sample_pcs():
        counts[image_of(pc)] += 1
    how = (f"PC samples: {counts['booster']} Booster, {counts['core']} core, "
           f"{counts['ram']} RAM, {counts[None]} other")
    if counts["booster"] and not counts["core"]:
        return "booster", how
    if counts["core"] and not counts["booster"]:
        return "core", how
    return None, how


def running_elf(explicit: str | None) -> str:
    """The ELF given, or the cached ELF of the image that runs whose build ID
    is the one in flash."""
    if explicit:
        return explicit
    app, how = running_image()
    if not app:
        raise SwdError(f"cannot tell which image runs ({how})")
    for elf in cached_elfs(app):
        expected = elf_build_id(elf)
        if (expected and read_build_id(elf) == expected
                and elf_head_in_flash(elf)):
            return elf
    raise SwdError(f"no cached {app} ELF matches the build ID in flash: "
                   "flash with tools/dev/flash.sh or pass --elf")


# A build ID as build_id.cmake writes it, between NULs in flash.
BUILD_ID_RE = re.compile(rb"\0([0-9a-f]{7}(?:-dirty\.[0-9a-f]{7})?(?:\+debug)?|nogit(?:\+debug)?)\0")


def scan_build_id(app: str) -> str | None:
    """The build ID found by scanning the image's flash, for an image with no
    matching cached ELF (a product UF2 flashed by other means). Only a single
    candidate counts."""
    start, end = IMAGES[app]
    found = {m.group(1).decode() for m in BUILD_ID_RE.finditer(
        read_memory(start, end - start))}
    return found.pop() if len(found) == 1 else None


def cmd_board(args: argparse.Namespace) -> int:
    out = openocd(f"mdw 0x{SYSINFO_CHIP_ID:08x}", f"mdw 0x{SYSINFO_PACKAGE_SEL:08x}",
                  f"mdw 0x{OTP_NUM_GPIOS:08x}",
                  f"targets {CORES[0]}", f"mdw 0x{DHCSR:08x}",
                  f"targets {CORES[1]}", f"mdw 0x{DHCSR:08x}")
    words = [int(w, 16) for w in re.findall(r"^0x[0-9a-f]{8}:\s+([0-9a-f]{8})",
                                           out, re.M)]
    if len(words) != 5:
        raise SwdError("cannot read the chip: " + out.strip()[-200:])
    chip_id, package, num_gpios, dhcsr0, dhcsr1 = words
    pkg = "RP2350A (QFN-60)" if qfn60(package, num_gpios) else "RP2350B (QFN-80)"
    print(f"chip: RP2350 rev {chip_id >> 28}, CHIP_ID 0x{chip_id:08x}; {pkg}")
    app, how = running_image()
    if app:
        try:
            elf = running_elf(None)
            kind = os.path.basename(elf).split("-")[2]
            print(f"running: {app} {kind}, build {elf_build_id(elf)} ({how})")
        except SwdError:
            build_id = scan_build_id(app)
            print(f"running: {app}, build {build_id or 'unknown'} "
                  f"(no cached ELF; {how})")
    else:
        print(f"running: unknown ({how})")
    states = ["halted" if d & DHCSR_S_HALT else "running" for d in (dhcsr0, dhcsr1)]
    print(f"cores: {CORES[0]} {states[0]}, {CORES[1]} {states[1]}")
    return 0


def cmd_running(args: argparse.Namespace) -> int:
    # The SDK's runtime init moves VTOR to the RAM vector table; together with
    # PC samples inside this image's flash it proves the image got past its
    # startup code.
    app = elf_app(args.elf)
    table = elf_symbols(args.elf, "ram_vector_table").get("ram_vector_table")
    if not table:
        raise SwdError(f"{args.elf} has no ram_vector_table symbol")
    deadline = time.monotonic() + args.timeout
    last = "no answer"
    while True:
        try:
            vtor = read_word(VTOR)
            halted = read_word(DHCSR) & DHCSR_S_HALT
            image, how = running_image()
            if vtor == table[0] and not halted and image == app:
                print(f"running: {app} (VTOR 0x{vtor:08x}; {how})")
                return 0
            last = (f"VTOR 0x{vtor:08x}, {image or 'unknown'} ({how})"
                    + (", core 0 halted" if halted else ""))
        except SwdError as exc:
            last = str(exc)
        if time.monotonic() >= deadline:
            print(f"not running {args.elf} after {args.timeout:g} s ({last})",
                  file=sys.stderr)
            return 3
        time.sleep(0.5)


def cmd_crash(args: argparse.Namespace) -> int:
    try:
        elf = running_elf(args.elf)
    except SwdError:
        elf = None
    data = read_memory(WATCHDOG_REASON, 36)
    reason, *scratch = struct.unpack("<9I", data)
    chip_reset = read_word(POWMAN_CHIP_RESET)
    causes = [name for bit, name in CHIP_RESET_CAUSES if chip_reset & bit]
    # The watchdog's REASON is zero after a chip-level reset, so a non-zero
    # value means the watchdog caused the last reset; the power manager's
    # CHIP_RESET only records chip-level resets and keeps them across
    # watchdog resets.
    if reason & 3:
        # Neither firmware arms the watchdog against hangs: its timer only runs
        # out after a watchdog_reboot(), which Booster uses for its factory
        # reset and the core firmware to enter configuration mode or to
        # reformat its settings.
        how = ("forced: a tool's reset (swd.py reset, picotool reboot)"
               if reason & 2 else "its timer expired: the firmware's own "
               "watchdog_reboot()")
        print(f"last reset: the watchdog, {how} (REASON 0x{reason:08x})")
        print("last chip-level reset before it: " +
              (", ".join(causes) or "none recorded") +
              f" (POWMAN CHIP_RESET 0x{chip_reset:08x})")
    else:
        print("last reset: chip-level: " + (", ".join(causes) or "none recorded")
              + f" (POWMAN CHIP_RESET 0x{chip_reset:08x})")
    print("scratch 0-7: " + " ".join(f"0x{v:08x}" for v in scratch))
    if scratch[4] == SDK_REBOOT_MAGIC:
        print(f"  scratch 4-7: SDK reboot to PC 0x{scratch[7]:08x}, "
              f"SP 0x{scratch[6]:08x}")
    if elf:
        for i, v in enumerate(scratch):
            where = resolve_address(elf, v)
            if where:
                print(f"  scratch {i}: {where}")
    return 0


def cmd_build_id(args: argparse.Namespace) -> int:
    if args.elf:
        status = 0
        for elf in args.elf:
            in_flash = read_build_id(elf)
            built = elf_build_id(elf)
            note = "" if in_flash == built else f" (the ELF has {built})"
            print(f"{elf_app(elf)}: {in_flash or 'none'}{note}")
            if not in_flash or in_flash != built:
                status = 3
        return status
    found = 0
    for app in IMAGES:
        tried = set()
        build_id = None
        for elf in cached_elfs(app):
            sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
            if not sym or (sym, elf_build_id(elf)) in tried:
                continue
            tried.add((sym, elf_build_id(elf)))
            in_flash = read_build_id(elf)
            if (in_flash and in_flash == elf_build_id(elf)
                    and elf_head_in_flash(elf)):
                build_id = in_flash
                break
        how = ""
        if not build_id:
            build_id = scan_build_id(app)
            how = " (scanned; no cached ELF matches)" if build_id else ""
        print(f"{app}: {build_id or 'none'}{how}")
        found += bool(build_id)
    return 0 if found else 3


def cmd_program(args: argparse.Namespace) -> int:
    # Not OpenOCD's `program`: it resets with OpenOCD's own sequence first,
    # which leaves DMA running and touches core 1. `flash write_image erase`
    # erases and writes only the sectors the ELF covers.
    writes = []
    for elf in args.elf:
        elf_app(elf)  # refuse anything that is not one of the two images
        writes += [f"flash write_image erase {elf}", f"verify_image {elf}"]
    out = openocd(*quiesce_commands(), *writes, check=False, work_area=True)
    verified = re.findall(r"verified \d+ bytes", out)
    if len(verified) != len(args.elf):
        chip_reset()
        raise SwdError("flash write did not verify: " + " / ".join(
            l.strip() for l in out.splitlines() if l.startswith("Error"))[-300:])
    chip_reset()
    for elf in args.elf:
        print(f"flashed {elf_app(elf)}: {elf}")
    return 0


# --- Board state ----------------------------------------------------------------

def read_words(addresses: list[int]) -> list[int]:
    """Several 32-bit words in one OpenOCD run."""
    cmds = [f"targets {CORES[0]}", *(f"mdw 0x{a:08x}" for a in addresses)]
    guarded = touches_flash([(a, 4) for a in addresses])
    out = openocd(*(flash_guard(cmds) if guarded else cmds), core0_only=guarded)
    values = []
    for a in addresses:
        m = re.search(rf"0x{a:08x}:\s+([0-9a-fA-F]{{8}})", out)
        if not m:
            raise SwdError(f"cannot read 0x{a:08x}")
        values.append(int(m.group(1), 16))
    return values


def read_memories(ranges: list[tuple[int, int]]) -> list[bytes]:
    """Several memory ranges in one OpenOCD run."""
    with tempfile.TemporaryDirectory() as tmp:
        cmds, paths = [], []
        for i, (address, length) in enumerate(ranges):
            path = os.path.join(tmp, f"r{i}.bin")
            cmds.append(f"dump_image {path} 0x{address:08x} {length}")
            paths.append(path)
        guarded = touches_flash(ranges)
        openocd(*(flash_guard(cmds) if guarded else cmds), core0_only=guarded)
        out = []
        for (address, length), path in zip(ranges, paths):
            with open(path, "rb") as f:
                data = f.read()
            if len(data) != length:
                raise SwdError(f"read {len(data)} of {length} bytes at 0x{address:08x}")
            out.append(data)
    return out


# The settings sector both firmwares share (GLOBAL_CONFIG in the linker
# scripts). Entries are 127 bytes, back to back, unaligned: a 30-byte key, a
# 1-byte type (the enum is packed to one byte by the ARM EABI) and a 96-byte
# value, all NUL-padded strings; MAGICVERSION comes first. The firmware loads
# as many entries as it has defaults and never clears the bytes after the
# last one, so decoding stops at the first entry that is not one.
GLOBAL_CONFIG = 0x101FD000
SETTINGS_SECTOR = 4096
KEY_LEN, VALUE_LEN = 30, 96
ENTRY_LEN = KEY_LEN + 1 + VALUE_LEN
SETTINGS_MAGIC = 0x12340001
SETTING_TYPES = {0: "INT", 1: "STR", 2: "BOOL"}
SECRET_KEYS = {"WIFI_PASSWORD"}
KEY_RE = re.compile(rb"[A-Z0-9_]+")


def decode_settings(data: bytes) -> list[tuple[str, int, str]]:
    entries = []
    for off in range(0, len(data) - ENTRY_LEN + 1, ENTRY_LEN):
        raw = data[off:off + ENTRY_LEN]
        key = raw[:KEY_LEN].split(b"\0", 1)[0]
        if not key or not KEY_RE.fullmatch(key) or raw[KEY_LEN] not in SETTING_TYPES:
            break
        value = raw[KEY_LEN + 1:].split(b"\0", 1)[0].decode("latin-1")
        entries.append((key.decode(), raw[KEY_LEN], value))
    if not entries or entries[0][0] != "MAGICVERSION" or entries[0][2] != str(SETTINGS_MAGIC):
        found = entries[0][2] if entries and entries[0][0] == "MAGICVERSION" else "none"
        raise SwdError(f"no valid settings at 0x{GLOBAL_CONFIG:08x} (magic {found}, "
                       f"expected {SETTINGS_MAGIC}): the firmware loads its defaults")
    return entries


def encode_settings(entries: list[tuple[str, int, str]]) -> bytes:
    blob = b"".join(key.encode().ljust(KEY_LEN, b"\0") + bytes([kind])
                    + value.encode("latin-1").ljust(VALUE_LEN, b"\0")
                    for key, kind, value in entries)
    return blob.ljust(SETTINGS_SECTOR, b"\xff")


def read_settings() -> list[tuple[str, int, str]]:
    return decode_settings(read_memory(GLOBAL_CONFIG, SETTINGS_SECTOR))


def settings_value(entries: list[tuple[str, int, str]], key: str) -> str | None:
    return next((v for k, _, v in entries if k == key), None)


def write_settings(entries: list[tuple[str, int, str]]) -> None:
    """Rewrite the settings sector, then reset: the running firmware keeps its
    own copy in RAM and would write it back on its next save."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "settings.bin")
        with open(path, "wb") as f:
            f.write(encode_settings(entries))
        out = openocd(*quiesce_commands(),
                      f"flash write_image erase {path} 0x{GLOBAL_CONFIG:08x} bin",
                      f"verify_image {path} 0x{GLOBAL_CONFIG:08x} bin",
                      check=False, work_area=True)
    chip_reset()
    if not re.search(r"verified \d+ bytes", out):
        raise SwdError("settings write did not verify: " + " / ".join(
            l.strip() for l in out.splitlines() if l.startswith("Error"))[-300:])


def cmd_settings(args: argparse.Namespace) -> int:
    try:
        entries = read_settings()
    except SwdError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3
    if args.action != "set":
        if args.pairs:
            raise SwdError("usage: settings [set KEY=VALUE ...]")
        for key, _, value in entries[1:]:
            if key in SECRET_KEYS and value and not args.show_secrets:
                value = "*" * len(value)
            print(f"{key}={value}")
        return 0
    if not args.pairs:
        raise SwdError("settings set needs at least one KEY=VALUE")
    changes = {}
    for pair in args.pairs:
        key, sep, value = pair.partition("=")
        kind = next((t for k, t, _ in entries if k == key), None)
        if not sep or kind is None or key == "MAGICVERSION":
            raise SwdError(f"{pair}: not KEY=VALUE for a key in the settings")
        if len(value.encode("latin-1")) >= VALUE_LEN:
            raise SwdError(f"{key}: values are at most {VALUE_LEN - 1} bytes")
        if kind == 0 and not re.fullmatch(r"-?\d+", value):
            raise SwdError(f"{key} is an integer")
        if kind == 2 and value not in ("true", "false"):
            raise SwdError(f"{key} is true or false")
        changes[key] = value
    write_settings([(k, t, changes.get(k, v)) for k, t, v in entries])
    for key, value in changes.items():
        shown = "*" * len(value) if key in SECRET_KEYS and not args.show_secrets else value
        print(f"{key}={shown}")
    print("settings written and verified; chip reset")
    return 0


# The CONFIG button: GPIO 9, pulled down, high when pressed. Every mode loop of
# the core firmware jumps into Booster on its rising edge, without touching the
# settings. Driving the pin high through SIO presses it; the button connects
# the pin to 3.3 V, so driving it high cannot fight it.
CONFIG_GPIO = 9
SIO_GPIO_OUT_SET = 0xD0000018
SIO_GPIO_OUT_CLR = 0xD0000020
SIO_GPIO_OE_SET = 0xD0000038
SIO_GPIO_OE_CLR = 0xD0000040


def press_config(pressed: bool) -> None:
    bit = 1 << CONFIG_GPIO
    if pressed:
        openocd(f"mww 0x{SIO_GPIO_OUT_SET:08x} 0x{bit:x}",
                f"mww 0x{SIO_GPIO_OE_SET:08x} 0x{bit:x}")
    else:
        openocd(f"mww 0x{SIO_GPIO_OE_CLR:08x} 0x{bit:x}",
                f"mww 0x{SIO_GPIO_OUT_CLR:08x} 0x{bit:x}")


def wait_image(app: str, timeout: float) -> tuple[bool, str]:
    deadline = time.monotonic() + timeout
    how = "no answer"
    while True:
        try:
            image, how = running_image()
            if image == app:
                return True, how
        except SwdError as exc:
            how = str(exc)
        if time.monotonic() >= deadline:
            return False, how
        time.sleep(0.5)


def cmd_boot(args: argparse.Namespace) -> int:
    if args.target == "ikbd":
        mode = settings_value(read_settings(), "MODE")
        chip_reset()
        if mode == "255":
            print("note: MODE is 255 (configuration): the core firmware jumps "
                  "straight back into Booster; set MODE first "
                  "(swd.py settings set MODE=1)", file=sys.stderr)
            ok, how = wait_image("booster", args.timeout)
            print(f"running: booster ({how})")
            return 3
        ok, how = wait_image("core", args.timeout)
        print(f"running: {'core' if ok else 'not the core firmware'} ({how})")
        return 0 if ok else 3
    ok, how = boot_booster(args.timeout)
    print(f"running: {'booster' if ok else 'not Booster'} ({how})")
    return 0 if ok else 3


def boot_booster(timeout: float) -> tuple[bool, str]:
    """Booster runs, or is made to run by pressing CONFIG over SWD."""
    image, how = running_image()
    if image == "booster":
        return True, "already; " + how
    # The core firmware polls the button from its mode loop, which starts a
    # moment after boot; press until Booster runs, releasing between presses
    # so each one is a new rising edge.
    deadline = time.monotonic() + timeout
    ok = False
    try:
        while not ok and time.monotonic() < deadline:
            press_config(True)
            ok, how = wait_image("booster", 2.0)
            press_config(False)
            if not ok:
                time.sleep(0.3)
    finally:
        try:
            press_config(False)
        except SwdError:
            pass
    return ok, how


VREG_V = [0.55 + 0.05 * i for i in range(16)] + [
    1.35, 1.40, 1.50, 1.60, 1.65, 1.70, 1.80, 1.90, 2.00, 2.35, 2.50, 2.65,
    2.80, 3.00, 3.15, 3.30]
# Both firmwares' low-power profile (MCU_CLOCK_FREQ_KHZ, MCU_VOLTAGE).
PROFILE_KHZ, PROFILE_V = 96000, 1.00


def qmi_timing(t: int, clk_khz: float) -> str:
    div = t & 0xFF
    sck = f"{clk_khz / div / 1000:.1f} MHz" if div else "?"
    pagebreak = ("none", "256 B", "1 KB", "4 KB")[(t >> 28) & 3]
    return (f"clkdiv {div} ({sck}), rxdelay {(t >> 8) & 7}, min deselect "
            f"{(t >> 12) & 0x1F}, max select {(t >> 17) & 0x3F}, page break "
            f"{pagebreak}, cooldown {(t >> 30) & 3}")


def qmi_format(f: int, cmd: int) -> str:
    widths = ("single", "dual", "quad", "?")
    prefix = (f"0x{cmd & 0xFF:02x}" if (f >> 12) & 1 else
              f"none: continuous mode, entered with 0x{cmd & 0xFF:02x}")
    suffix = f"0x{(cmd >> 8) & 0xFF:02x}" if (f >> 14) & 3 else "none"
    return (f"command {prefix}, mode {suffix}, dummy {((f >> 16) & 7) * 4} bits, "
            f"address {widths[(f >> 2) & 3]}, data {widths[(f >> 8) & 3]}"
            + (", DTR" if f & (1 << 28) else ""))


def cmd_clock(args: argparse.Namespace) -> int:
    (pll_cs, fbdiv, prim, sys_ctrl, sys_div, vreg, m0t, m0f, m0c) = read_words([
        0x40050000, 0x40050008, 0x4005000C, 0x4001003C, 0x40010040,
        0x4010000C, 0x400D000C, 0x400D0010, 0x400D0014])
    refdiv = pll_cs & 0x3F
    pd1, pd2 = (prim >> 16) & 7, (prim >> 12) & 7
    vco = 12000 * fbdiv / refdiv if refdiv else 0
    pll_khz = vco / (pd1 * pd2) if pd1 and pd2 else 0
    on_pll = (sys_ctrl & 1) and ((sys_ctrl >> 5) & 7) == 0
    div = (sys_div >> 16) or 1
    clk_khz = pll_khz / div if on_pll else 0
    vsel = (vreg >> 4) & 0x1F
    volts = VREG_V[vsel]
    print(f"clk_sys: {clk_khz / 1000:.1f} MHz" if on_pll else
          "clk_sys: not on PLL_SYS")
    print(f"  PLL_SYS: 12 MHz / {refdiv} x {fbdiv} = VCO {vco / 1000:.0f} MHz, "
          f"/ {pd1} / {pd2}" + (f", clk_sys / {div}" if div > 1 else ""))
    print(f"core voltage: {volts:.2f} V (VSEL {vsel})")
    print(f"flash (QMI M0): {qmi_timing(m0t, clk_khz)}")
    print(f"  read: {qmi_format(m0f, m0c)}")
    ok = round(clk_khz) == PROFILE_KHZ and abs(volts - PROFILE_V) < 0.01
    print(f"low-power profile ({PROFILE_KHZ // 1000} MHz, {PROFILE_V:.2f} V): "
          + ("yes" if ok else "NO"))
    return 0 if ok else 3


# Souffle's joystick and mouse lines (src/include/constants.h), GPIO 10-21:
# inputs with pull-ups, low when pressed. The factory test watches all twelve.
JOY_LINES = {10: "JOY1 UP", 11: "JOY1 DOWN", 12: "JOY1 LEFT", 13: "JOY1 RIGHT",
             14: "JOY1 FIRE", 15: "JOY1 FIRE2", 16: "JOY0 UP", 17: "JOY0 DOWN",
             18: "JOY0 LEFT", 19: "JOY0 RIGHT", 20: "JOY0 FIRE", 21: "JOY0 FIRE2"}
SIO_GPIO_IN = 0xD0000004
SIO_GPIO_OUT = 0xD0000010
SIO_GPIO_OE = 0xD0000030


def cmd_gpio(args: argparse.Namespace) -> int:
    gpio_in, gpio_out, gpio_oe = read_words([SIO_GPIO_IN, SIO_GPIO_OUT, SIO_GPIO_OE])
    print(f"GPIO_IN 0x{gpio_in:08x}, OE 0x{gpio_oe:08x}")
    print(f"GPIO {CONFIG_GPIO:2}  CONFIG       "
          + ("high (pressed)" if gpio_in >> CONFIG_GPIO & 1 else "low"))
    low = 0
    for gpio, name in JOY_LINES.items():
        level = gpio_in >> gpio & 1
        low += not level
        driven = (" (driven low over SWD)" if gpio_oe >> gpio & 1
                  and not gpio_out >> gpio & 1 else "")
        print(f"GPIO {gpio:2}  {name:11}  "
              + ("high" if level else "low (pressed)") + driven)
    print(f"joystick lines low: {low} of {len(JOY_LINES)}")
    return 0


def joy_mask(names: list[str]) -> int:
    if names == ["all"]:
        return sum(1 << g for g in JOY_LINES)
    mask = 0
    for n in names:
        if not n.isdigit() or int(n) not in JOY_LINES:
            raise SwdError(f"{n}: not a joystick line (10-21, or all)")
        mask |= 1 << int(n)
    return mask


def refuse_croissant() -> None:
    """The lines are Souffle's; on Croissant these GPIOs are wired otherwise.
    Refuse when the running build is known to be a Croissant one."""
    try:
        elf = running_elf(None)
    except SwdError:
        return
    if "-croissant-" in os.path.basename(elf):
        raise SwdError("the running build is a Croissant one: GPIO 10-21 are not "
                       "the joystick lines there")


def cmd_drive_low(args: argparse.Namespace) -> int:
    mask = joy_mask(args.gpio)
    refuse_croissant()
    # Output level first, so the pins never drive high for a moment.
    openocd(f"mww 0x{SIO_GPIO_OUT_CLR:08x} 0x{mask:x}",
            f"mww 0x{SIO_GPIO_OE_SET:08x} 0x{mask:x}")
    print(f"driving low: GPIO mask 0x{mask:08x} (swd.py release to hand them back)")
    return 0


def cmd_release(args: argparse.Namespace) -> int:
    mask = joy_mask(args.gpio)
    openocd(f"mww 0x{SIO_GPIO_OE_CLR:08x} 0x{mask:x}")
    print(f"released: GPIO mask 0x{mask:08x} (inputs again)")
    return 0


def cmd_factory(args: argparse.Namespace) -> int:
    entries = read_settings()
    mode = settings_value(entries, "FACTORY_MODE")
    result = settings_value(entries, "FACTORY_RESULT")
    if mode is None and result is None:
        print("factory: FACTORY_MODE and FACTORY_RESULT are not in the settings "
              "(written by a firmware without the factory test)")
        return 3
    print(f"FACTORY_MODE={mode}")
    print(f"FACTORY_RESULT={result or ''}")
    fields = dict(f.partition("=")[::2] for f in (result or "").split(";") if f)
    try:
        seen = int(fields["seen"], 16) if "seen" in fields else None
    except ValueError:
        seen = None
    if seen is not None:
        never = [f"GPIO {g} {n}" for g, n in JOY_LINES.items()
                 if not seen >> (g - 10) & 1]
        print("  lines never seen low: " + (", ".join(never) or "none"))
    for key, label in (("pass", "pass"), ("kbd", "keystrokes"),
                       ("held", "all low held (ms)"), ("vid", "keyboard VID:PID")):
        if key in fields:
            print(f"  {label}: {fields[key]}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="swd.py",
        description="Read, check and drive the RP2350 through the Debug Probe.")
    sub = p.add_subparsers(dest="cmd", required=True)

    bd = sub.add_parser("board", help="which chip and image run, and the build")
    bd.set_defaults(func=cmd_board)

    r = sub.add_parser("running", help="wait until the RP runs the ELF")
    r.add_argument("elf")
    r.add_argument("--timeout", type=float, default=10.0)
    r.set_defaults(func=cmd_running)

    v = sub.add_parser("verify", help="compare the flash with each ELF")
    v.add_argument("elf", nargs="+")
    v.set_defaults(func=cmd_verify)

    b = sub.add_parser("build-id", help="read each image's build ID from flash")
    b.add_argument("elf", nargs="*")
    b.set_defaults(func=cmd_build_id)

    m = sub.add_parser("read", help="dump a memory range to a file")
    m.add_argument("address")
    m.add_argument("length")
    m.add_argument("outfile")
    m.set_defaults(func=cmd_read)

    g = sub.add_parser("program", help="flash the ELFs and reset")
    g.add_argument("elf", nargs="+")
    g.set_defaults(func=cmd_program)

    rs = sub.add_parser("resume", help="release both cores from a debug halt")
    rs.set_defaults(func=cmd_resume)

    rst = sub.add_parser("reset", help="reset the whole chip, watchdog-style")
    rst.set_defaults(func=cmd_reset)

    cr = sub.add_parser("crash", help="explain the last reset")
    cr.add_argument("--elf")
    cr.set_defaults(func=cmd_crash)

    hp = sub.add_parser("heap", help="heap size, peak and free space")
    hp.add_argument("--elf")
    hp.add_argument("--watch", type=float, metavar="SECONDS")
    hp.add_argument("--csv", metavar="FILE")
    hp.set_defaults(func=cmd_heap)

    pm = sub.add_parser("postmortem", help="halt, dump backtraces, resume")
    pm.add_argument("--elf")
    pm.add_argument("--leave-halted", action="store_true")
    pm.set_defaults(func=cmd_postmortem)

    st = sub.add_parser("settings", help="the settings sector, or set entries")
    st.add_argument("action", nargs="?", choices=["set"])
    st.add_argument("pairs", nargs="*", metavar="KEY=VALUE")
    st.add_argument("--show-secrets", action="store_true")
    st.set_defaults(func=cmd_settings)

    bo = sub.add_parser("boot", help="run Booster (presses CONFIG) or the core "
                        "firmware (resets)")
    bo.add_argument("target", choices=["booster", "ikbd"])
    bo.add_argument("--timeout", type=float, default=20.0)
    bo.set_defaults(func=cmd_boot)

    ck = sub.add_parser("clock", help="clk_sys, core voltage and the flash "
                        "interface, against the low-power profile")
    ck.set_defaults(func=cmd_clock)

    gp = sub.add_parser("gpio", help="CONFIG and the joystick lines' levels")
    gp.set_defaults(func=cmd_gpio)

    dl = sub.add_parser("drive-low", help="drive joystick lines low over SWD "
                        "(Souffle; simulates the factory harness)")
    dl.add_argument("gpio", nargs="+", metavar="GPIO", help="10-21, or all")
    dl.set_defaults(func=cmd_drive_low)

    rl = sub.add_parser("release", help="hand joystick lines back as inputs")
    rl.add_argument("gpio", nargs="+", metavar="GPIO", help="10-21, or all")
    rl.set_defaults(func=cmd_release)

    fa = sub.add_parser("factory", help="decode the factory test's settings")
    fa.set_defaults(func=cmd_factory)
    return p


def main() -> int:
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (SwdError, subprocess.CalledProcessError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
