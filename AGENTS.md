# AGENTS.md
This repository contains **rp2-atarist-rpikb**, an Atari ST IKBD replacement
firmware for RP2040 boards (e.g., Pico W). The code lives under `src/` and is
built with the Pico SDK plus a vendored Bluepad32 tree.

---
## Project layout (important)
- `src/` — RP2040 firmware sources (main loop, HID, Bluetooth, USB, 6301 emu).
  - `src/main.c` — firmware entry point.
  - `src/6301/` — HD6301 emulation and peripherals.
  - `src/include/` — project headers and constants.
  - `src/settings/` — configuration handling.
- `bluepad32/` — embedded Bluepad32 sources used by the project.
- Submodules at repo root (do not vendor these):
  - `pico-sdk/`
  - `pico-extras/`
- `rom/` — ROM assets used by the emulator.
- `build/` — CMake build output (deleted by `build.sh`).
- `dist/` — UF2 artifacts copied by `build.sh`.

---
## Build prerequisites
- ARM embedded toolchain (`arm-none-eabi-*`).
- CMake 3.26+ (used by `src/CMakeLists.txt`).
- Git submodules initialized.

---
## Build commands
### Project build (scripted)
```sh
./build.sh <board_type> <build_type> <release_type>
```
Example:
```sh
./build.sh pico_w debug
```
Notes:
- `build.sh` pins submodule tags and **deletes `build/`** before building.
- Artifacts are copied into `dist/` as `rp2-ikbd-<board>.uf2`.

### Manual build (no script)
```sh
mkdir -p build
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---
## Linting and formatting
Follow `.clang-format` and `.clang-tidy` in the repo root.
- `src/CMakeLists.txt` enables `clang-tidy` when available.
- Use `clang-format -i src/<file>.c` for single-file formatting.

---
## Tests
- There is **no first-party test suite** in this repo.
- Validation is typically done by building the firmware and testing on device.

---
## Code style guidelines
Follow `.clang-format` and `.clang-tidy` in the repo root.
### Formatting
- 2-space indent, 80-column limit.
- `Attach` style braces, no tabs.
- Pointer alignment is left (`type* ptr`).
- Keep include blocks sorted and grouped; clang-format enforces ordering.
### Naming (from `.clang-tidy`)
- Namespaces: `CamelCase`.
- Classes/structs: `CamelCase`.
- Functions: `camelBack` with a capitalized suffix (regex enforced).
- Variables/parameters: `camelBack`.
- Members: `camelBack` with `m_` prefix for private members.
### Types and constants
- Prefer fixed-width types (`uint32_t`, `int16_t`) for firmware interfaces.
- Use `bool` for boolean logic; avoid implicit integer truthiness.
- Avoid magic numbers; `0`, `1`, `-1` are allowed per `.clang-tidy`.
### Imports/includes
- Standard/system headers before project headers.
- Keep include blocks minimal; prefer forward declarations in headers.
- Don’t reorder includes manually if `clang-format` will do it.
### Error handling
- Check return codes and null pointers from hardware/SDK calls.
- Propagate errors upward rather than silently ignoring failures.
- Prefer explicit error logs over bare asserts in runtime paths.

---
## Workflow rules for agents
- Do not discard or overwrite local changes without explicit user approval.
- Avoid running `./build.sh` unless asked; it deletes `build/` and pins tags.
- You may compile targets directly (for verification) without invoking `build.sh`.
- Respect `.clang-format` and `.clang-tidy` in the repo root.
- Do not change submodule pins unless explicitly requested.
- No Cursor or Copilot rule files are present in this repo.

---
## “Done” checklist
- Firmware compiles successfully without using `./build.sh` unless requested.
