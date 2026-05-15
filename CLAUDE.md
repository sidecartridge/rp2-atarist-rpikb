# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `AGENTS.md` (project layout, build prereqs, code style, agent workflow rules) and `README.md` (high-level project background, supported hardware, known limitations). This file covers what's not obvious from those sources or from the code itself.

## Build commands

The scripted build pins submodule tags, deletes the relevant preset build dir, and copies the UF2 to `dist/`:

```sh
./build.sh <board_type> <build_type> <release_type> <board_flavor>
# board_type:   pico_w | pico2 | pico2_w
# build_type:   release | debug
# release_type: final | <name>  (selects version-<name>.txt)
# board_flavor: croissant (BOARD_TARGET=1) | souffle (BOARD_TARGET=2)
```

Important: `./build.sh` checks out **specific tags/branches** of the submodules (`pico-sdk` 2.2.0, `pico-extras` sdk-2.2.0, `bluepad32` branch `synthetic-HID-descriptor`). Do not run it casually — it will move submodule HEADs and wipe a preset build directory.

**Prefer presets for iterative compilation** (no submodule retagging, no `dist/` copy):

```sh
# From src/
cmake --preset souffle-debug      # also: souffle-release, croissant-debug, croissant-release
cmake --build --preset souffle-debug
```

Each preset writes to its own out-of-source dir at the repo root: `build-<flavor>-<kind>/`. The presets set `CMAKE_BUILD_TYPE` and the `BOARD_TARGET` env var; `build.sh` additionally exports `PICO_BOARD`, `PICO_PLATFORM`, `DEBUG_MODE`, and the release-version env vars that `src/CMakeLists.txt` reads.

There is **no test suite**. Validation happens by flashing the UF2 and exercising the firmware on real hardware.

## Architecture

### Two-core split

The firmware runs as a two-core program on RP2040 / RP2350:

- **Core 1** (`core1_entry` in `src/main.c`) owns the **HD6301 emulator** (`src/6301/`, ROM image in `src/include/HD6301V1ST.h`). It loads the IKBD ROM at `IKBD_ROMBASE` (256), resets the 6301, seeds a TOD via `IKBD_CMD_SET_TOD`, then runs `hd6301_run_clocks(IKBD_CYCLES_PER_LOOP)` in a tight pacing loop driven by `time_us_64()`.
- **Core 0** runs the host-side loops: serial RX from the Atari (`serialp.c` / `handle_rx_from_st`), plus exactly one of three mode loops chosen at boot from the `PARAM_MODE` setting: native (`nativeloop.c`), USB host (`usbloop.c`), or Bluetooth via Bluepad32 + BTstack (`btloop.c`). HID events feed into `hidinput.c`, `mouse.c`, `joystick.c`, `stkeys.c`, which inject bytes back into the 6301 via `rx_buffer_put` / `hd6301_receive_byte`.

BTstack TLV flash persistence only works when Core 1 is running with multicore lockout enabled — there's a load-bearing comment in `main.c` about this. Disabling Core 1 silently breaks BT flash writes.

### Board flavors & feature gates

Two physical board flavors are supported and select different GPIO maps and feature sets at compile time:

- `BOARD_TARGET=1` → Croissant Rev 2 (RP2040 / Pico W). Has a native Atari-side keyboard/joystick path; `nativeloop.c` is compiled in only when this is set.
- `BOARD_TARGET=2` → Souffle Rev 2 (RP2350 / Pico 2 W). USB + Bluetooth only.

The capability mask `COMPUTER_TARGET` is derived from `BOARD_TARGET` (Croissant=5 → Native+BT, Souffle=6 → USB+BT) and split into `COMPUTER_TARGET_NATIVE` / `COMPUTER_TARGET_USB` / `COMPUTER_TARGET_BT` macros. Code paths and includes around the three mode loops in `main.c` are gated by these macros — **mirror this `#if` pattern when touching mode-specific code**; don't unconditionally include headers like `nativeloop.h` or `usbloop.h`.

GPIO pin definitions in `src/include/constants.h` differ between the two boards — check the `BOARD_TARGET` guards before adding pin constants.

### Settings and configuration mode

Persistent settings live in flash, managed by `src/settings/` (the `settings` library) and accessed via `gconfig.c` (context name `"IKBD"`). Key invariant: a `GCONFIG_MISMATCHED_APP` result triggers `settings_erase` followed by `watchdog_reboot` — the firmware reformats its settings region rather than running with stale config.

Boot dispatches on `PARAM_MODE` (read via `settings_find_entry`):
- `0` → native, `1` → USB, `2` → BT, `255` (or missing/invalid) → configuration mode.

Configuration mode jumps into a separate **booster app** flashed at `_booster_app_flash_start` via `jump_to_booster_app()`. The jump sequence depends on whether the target is RP2040 (M0+) or RP2350 (M33) — the VTOR register and offset differ. Before jumping, `usbloop_shutdown_for_jump()` and `serialp_close()` must run, and Core 1 is reset. Read this carefully before touching the jump path — getting the order or the asm wrong leaves the device in an unrecoverable state.

On Croissant only, the Atari IKBD reset sequence (`0x80 0x01`) is timestamped; holding reset for 3–10s toggles the IKBD source (and halts BT), holding ≥10s enters configuration mode. See `handle_reset_sequence_cb` in `main.c`.

### Linker scripts and flash regions

Custom linker scripts (`memmap_rp2-ikbd_default.ld`, `memmap_rp2-ikbd_rp2350.ld`) reserve flash regions for the booster app, config, global lookup, global config, and the BTstack TLV bank. `main.c` references these via `extern unsigned int _booster_app_flash_start` etc. (declared in `constants.h`). When changing flash layout, update both the linker script and the consuming `extern` symbols.

The build runs **XIP (execute-in-place from flash)** — `.text` lives in flash and is fetched by the XIP cache during execution. Earlier revisions of `src/CMakeLists.txt` declared `PICO_DEFAULT_BINARY_TYPE=copy_to_ram` *after* `pico_sdk_init()` (too late) and the custom linker scripts never did the `AT > FLASH > RAM` staging copy_to_ram requires; the settings had no effect. Those lines are now removed. If a future change wants RAM execution for timing-jitter reasons, the linker scripts need a `memmap_copy_to_ram.ld`-style rewrite.

Clock is overclocked to 225 MHz at 1.20 V (`RP2040_CLOCK_FREQ_KHZ`, `RP2040_VOLTAGE` in `constants.h`).

### Vendored dependencies

- `pico-sdk/`, `pico-extras/`, `bluepad32/` are git submodules at the repo root. `build.sh` pins them to specific refs; **never bump submodule pins without an explicit ask**.
- The `bluepad32` pin is a branch (`synthetic-HID-descriptor`), not a tag — it can move under you if you don't fetch carefully.
- BTstack comes from `pico-sdk/lib/btstack` (not the Bluepad32 copy). Commented-out lines in `src/CMakeLists.txt` show how to switch, but it requires a Pico SDK patch.
- The HD6301 core in `src/6301/` is extracted/adapted from Steem SSE (originally sim68xx by Arne Riiber). Treat it as third-party — don't reformat or refactor for style.

## Conventions

Inherit the rules in `AGENTS.md` (clang-format, clang-tidy, naming, no unsolicited `build.sh` runs, don't change submodule pins). Two extras worth calling out:

- The `_DEBUG` macro (driven by env `DEBUG_MODE`) gates `DPRINTF` and stdio UART. Release builds strip sections and disable UART output entirely.
- `joystick.c.old` in `src/` is an intentional leftover, not stray clutter — leave it alone unless asked.

## Planning artifacts

Project-level planning (Epics / Stories / Tasks, progress tracking, design notes) lives under `docs/` at the repo root. `docs/` is **gitignored** (`.gitignore`) and is private to the author's local workspace. Treat it like scratch paper.

**Hard rule:** never reference the planning structure or any Epic/Story/Task identifier, number, slug, or index in any committed artifact. That means **no** mentions in:
- commit messages, PR titles, or PR descriptions
- `CHANGELOG.md` entries
- source-code comments
- any file outside `docs/` itself

When writing a commit, describe the change in its own terms ("fix BT keyboard report layout", "consolidate booster-jump teardown") — not as "completes the booster-jump-safety epic" or similar.

**Branch-to-Epic rule:** one branch per **Epic**, not per Story. A Story is a single commit on the Epic's branch (or a small set of related commits). The Epic ships as one PR containing the full sequence of Story commits. Branch names describe the work area in their own terms (e.g., `fix/booster-jump-safety`), never citing a planning ID. This keeps the public git history shaped around themes the user understands, not around the internal planning structure.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `bluepad32/` — they are git submodules pinned by `build.sh` on every run (the bluepad32 pin is a *branch*, `synthetic-HID-descriptor`, not a tag).
- Treat `src/6301/` as third-party (Steem SSE / sim68xx). Don't reformat or refactor for style; fix bugs surgically.
- Don't add features to `main.c` boot/dispatch logic — extend the mode-specific loop (`btloop.c`, `usbloop.c`, `nativeloop.c`) instead.
- Match the existing C style (`.clang-format`, `.clang-tidy` in repo root — wired into CMake when the binaries are on `PATH`).

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
