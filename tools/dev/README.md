# Developer tools

Host-side tools for working on the core firmware with the hardware attached: a Souffle or Croissant
board (Pico 2 W, RP2350) with a Raspberry Pi Debug Probe wired to the Pico's SWD pins and to its
debug UART. They build, flash and check a board, read its state while it runs, drive it remotely,
and run hardware harnesses, so a change is judged on hardware in one command. The Atari need not be
connected for any of them. Ported from the Booster repository's `tools/dev` (itself from
sidecartos's), so both firmwares are driven the same way; Python tools use the standard library
only.

## Wiring

| Debug Probe | Souffle / Croissant (Pico 2 W) |
|---|---|
| D (SWD): SC, SD, GND | SWCLK, SWDIO, GND |
| U (UART): RX | GPIO 0 (UART0 TX, the debug console) |
| U (UART): TX | GPIO 1 (UART0 RX) |

The Atari serial link uses UART1 on GPIO 4/5 and is not involved. Power the board from its micro
USB or from the Atari.

## Environment

- OpenOCD with RP2350 support and the `rp2350.cm0`/`rp2350.cm1` target names: `$OPENOCD`,
  `openocd` on `PATH`, or `../pico/openocd/src/openocd` (the Raspberry Pi fork); its scripts from
  `$PICO_OPENOCD_PATH`. (The older `../pico/openocd-rp2350` build that `.vscode/launch.json` uses
  names the targets `rp2350.dap.core0`/`core1` and does not work with `swd.py`.)
- The Arm toolchain on `PATH` (`arm-none-eabi-gcc`, `-nm`, `-objdump`, `-objcopy`, `-addr2line`),
  and `arm-none-eabi-gdb` (or `$ARM_GDB_PATH`) for `postmortem`.
- Python 3.10 or later.
- The network once per checkout: in a fresh clone `flash.sh` first clones the submodules. Every
  later run works from the local history.

## Firmware support these tools rely on

- Every build carries its build ID in flash as the `release_build_id` string
  (`src/build_id.cmake`): `<sha7>`, or `<sha7>-dirty.<diff7>` with uncommitted changes under
  `src/`, then `+debug` for a debug build. The same tree gives the same ID.
- Debug builds run the console at 921,600 baud, the same as Booster's, and start with a banner
  naming the image and its build: `main.c:506:main(): SidecarTridge IKBD core. v1.3.0 (...). Build
  a5a66ba+debug. DEBUG mode.` (`DPRINTF` puts the source location first). Release builds have no
  console.
- Release ELFs keep their symbols (only `--gc-sections`), so every command works on either build
  type.

## Rules

Every tool follows these; follow them too when driving the probe by hand.

- **Reset through the watchdog**, never with OpenOCD's `reset`: PSM `WDSEL` = `0x01FFFFF3`, then
  `WATCHDOG_CTRL.TRIGGER`. It restarts both cores together and leaves PIO and DMA stopped, as a
  power-on does.
- **No OpenOCD work area** except while writing flash: its default, 64 KB at `0x20010000`, is in
  the running firmware's RAM. `verify` then compares on the host, and GDB connects with
  `gdb_memory_map disable`.
- **Quiesce before a flash write:** halting the cores does not stop PIO and DMA, which the USB host
  and the Atari link use. `swd.py program` and `settings set` halt both cores, stop every PIO state
  machine and abort every DMA channel, write, then reset.
- **Never read the flash over SWD while the firmware writes it** (settings, Bluetooth keys): the
  QMI can wedge for good. `swd.py` reads the flash only with core 0 halted outside a flash
  operation, and resumes it.
- **One owner per probe and per UART.** Close a VS Code debug session before running a tool, and
  any serial terminal (or another project's `console.py watch`, which grabs the probe's UART when
  it is plugged in) before `console.py watch`.
- **Halting stops the HD6301 emulator, USB, Bluetooth and the Atari link** until the RP resumes.
  Core 1 runs the HD6301; a halt is fine for inspection but not timing-neutral.

## How the board switches between the two firmwares

The flash holds the core firmware at `0x10000000` and Booster at `0x10120000`. The core firmware
always starts first and enters Booster when `MODE` is 255 (configuration) or when the CONFIG button
(GPIO 9) is pressed. A stored `BOOT_FEATURE` other than `IKBD` makes it erase all settings and
reboot, so the tools never write it: `swd.py boot booster` presses CONFIG over SWD (drives GPIO 9
high, then releases it), and `swd.py boot ikbd` resets the chip.

## Debug console: `console.py`

Captures the console of a debug build to `tools/dev/logs/console.log`, a timestamp on every line,
and mirrors it to the terminal. Use it instead of a serial terminal.

```bash
python3 tools/dev/console.py watch          # leave running in a terminal
python3 tools/dev/console.py since-boot     # everything since the last banner
python3 tools/dev/console.py tail 100
python3 tools/dev/console.py grep 'HD6301' --since-boot
python3 tools/dev/console.py wait 'Entering HD6301 core loop' --timeout 20
```

`watch` finds the Debug Probe by its USB name (`--port` to choose another device), waits for it
when it is unplugged, and reopens it when it returns. A boot starts at either firmware's banner,
so one log follows the board across the jump into Booster. `grep` and `wait` exit with 3 when
nothing matches; `wait` only matches lines that arrive after it starts. The log rotates at 32 MB.

## Build, flash and verify: `flash.sh`

```bash
tools/dev/flash.sh souffle debug               # build, flash, verify
tools/dev/flash.sh souffle release
tools/dev/flash.sh croissant debug --build-only
```

Builds out of tree, incrementally, in `tools/dev/builds/<flavor>-<type>/core`, with the settings of
`build.sh` and the CMake presets (Pico 2 W for both flavors; release is CMake Release, debug is
CMake Debug with the 921,600 baud console). Everything builds against worktrees in `tools/dev/sdk/`
made from the submodules' own history at the pinned refs (pico-sdk 2.2.0, pico-extras sdk-2.2.0,
Bluepad32 `synthetic-HID-descriptor`), so the submodules, `build-*/` and `dist/` are never touched,
and local edits inside the submodules do not reach these builds. The ELFs are kept as
`tools/dev/builds/elf/core-<flavor>-<type>-<id>.elf`.

Then it flashes through the probe (`swd.py program`: only the core firmware's own flash, so Booster,
the config sectors, the settings and the Bluetooth keys are kept) and checks over SWD: the core
firmware runs, the flash matches the ELF byte for byte, and it carries the new build ID. With `MODE`
255 the core firmware jumps straight into Booster and the run check fails: set `MODE` first. On
failure it exits 1 and prints the console since the last boot. picotool is not used.
`RELEASE_DATE` defaults to the HEAD commit's date, so builds of one tree are byte-identical;
`FLASH_BUILDS_DIR` moves the build directories. The product build, `./build.sh`, is separate and
still makes the UF2s in `dist/`. The merged image with Booster is flashed from the Booster
repository (its `flash.sh --full` builds this firmware at the commit it pins).

## Debug probe: `swd.py`

Reads memory through the debug port while the CPU keeps running, so it works on release builds as
on debug ones, and on a hung RP. Exit codes: 0 success, 1 error, 2 usage, 3 the check failed.

### Generic

```bash
python3 tools/dev/swd.py board                  # chip, which image runs, its build
python3 tools/dev/swd.py running ELF            # wait until the RP runs this ELF
python3 tools/dev/swd.py verify ELF...          # flash identical to each ELF?
python3 tools/dev/swd.py build-id [ELF ...]     # each image's build ID
python3 tools/dev/swd.py read 0x20000000 1024 out.bin
python3 tools/dev/swd.py program ELF...         # quiesce, flash, reset
python3 tools/dev/swd.py reset                  # watchdog-style reset
python3 tools/dev/swd.py resume                 # release cores a debugger left halted
python3 tools/dev/swd.py crash                  # why did it last reset?
python3 tools/dev/swd.py heap [--watch 5 --csv FILE]
python3 tools/dev/swd.py postmortem [--leave-halted]
```

Both images put their RAM vector table at `0x20000000`, so which one runs is read from core 0's
program counter, sampled 32 times through the DWT: samples in one image's flash and none in the
other's decide. Commands that need symbols use the cached ELF whose build ID the RP carries
(`--elf` overrides); `build-id` without an ELF falls back to scanning the image's flash, so it also
names a product UF2's build. `postmortem` halts the RP, prints both cores' backtraces and
registers through GDB, then resumes it.

### Settings, boot and board state

```bash
python3 tools/dev/swd.py settings [--show-secrets]          # the settings sector
python3 tools/dev/swd.py settings set MODE=1 MOUSE_SPEED=7  # rewrite entries, verify, reset
python3 tools/dev/swd.py boot booster                       # press CONFIG over SWD
python3 tools/dev/swd.py boot ikbd                          # reset; stays in the core unless MODE is 255
python3 tools/dev/swd.py clock                              # 96 MHz / 1.00 V low-power profile?
python3 tools/dev/swd.py gpio                               # CONFIG and the joystick lines
python3 tools/dev/swd.py drive-low all                      # joystick lines low (Souffle)
python3 tools/dev/swd.py release all                        # hand them back
python3 tools/dev/swd.py factory                            # FACTORY_MODE and FACTORY_RESULT
```

The settings sector (`0x101FD000`, shared by both firmwares) holds 127-byte entries: a 30-byte key,
a 1-byte type and a 96-byte value, `MAGICVERSION` first. `settings set` accepts existing keys only
and checks values against their type; it resets the chip afterwards because the running firmware
keeps its own copy in RAM. `MODE` selects the mode loop: 0 native, 1 USB, 2 Bluetooth, 255
configuration (Booster).

`gpio` reads the levels through SIO, without symbols: the CONFIG button and Souffle's twelve
joystick and mouse lines (GPIO 10-21, low = pressed). `drive-low` drives those lines low as outputs
over SWD, so the firmware reads them pressed with nothing plugged in, which is how the factory test
runs without its harness; `release` makes them inputs again. Driving low is used instead of
switching the pads to pull-down because erratum RP2350-E9 can leave such an input reading high.
Croissant wires these GPIOs differently: `drive-low` refuses when the running build is
identified (a cached ELF matches it) as a Croissant one, and otherwise trusts you.

## Hardware harnesses

Scripts that run a set of checks on the board and print PASS, FAIL or SKIP for each. They need the
Debug Probe, `MODE` other than 255, and, for debug builds, `console.py watch` running (they read its
log, never the UART). Each writes a JSON report to `tools/dev/logs/` and exits 0 only when no check
failed. Run them as `python3 tools/dev/<harness>.py` (not `python3 -I`: they import `harnesslib.py`
beside them).

```bash
python3 tools/dev/tools_harness.py [--flash souffle]       # every swd.py command
python3 tools/dev/boot_harness.py --boots 20 [--flash souffle release]
```

- `tools_harness.py` checks the tools themselves: build ID, `verify`, a reset with the banner and
  the HD6301 emulator starting on the console, `running`, `board`, `settings`, `clock`, `gpio`,
  `heap`, `crash`, `factory`, `drive-low`/`release` on Souffle with the joystick ports empty, the round trip through Booster, and
  `postmortem` with both cores running after it.
- `boot_harness.py` resets the board N times and checks every boot: the flashed build runs at the
  low-power profile; with a debug build it reports the time from the banner to the HD6301
  emulator's loop.

## Measuring builds

```bash
tools/dev/measure_builds.sh souffle OUT_DIR                  # release and debug
tools/dev/stackdepth.py OUT_DIR/souffle-debug/core roots main core1_entry
tools/dev/stackdepth.py OUT_DIR/souffle-release/core top
```

`measure_builds.sh` builds through `flash.sh --build-only` into `OUT_DIR` (a temporary directory by
default) with `-fstack-usage -fcallgraph-info=su`, and prints each ELF's sections, its flash use
against the core firmware's 1152K slot and its heap room (`__end__` to `__HeapLimit`).
`stackdepth.py` computes worst-case stack depth from those builds: static call edges only (USB,
Bluetooth and timer callbacks are called indirectly: name them as roots), without interrupt
stacking or precompiled libraries, so treat its answer as a floor. Core 0's stack grows down from the
top of SCRATCH_Y and core 1's from the top of SCRATCH_X, 4 KB banks each
(`src/memmap_rp2-ikbd_rp2350.ld`).

`logs/`, `builds/` and `sdk/` are generated here and are gitignored.
