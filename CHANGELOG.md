# Changelog

## v1.1.0 (2026-05-16)

A round of fixes focused on the Souffle (Pico 2 W) experience, plus the build system getting honest about what it actually does.

### What you'll notice

- **Bluetooth + config button on Souffle: no more hangs.** Holding the config button while the device was in Bluetooth mode used to freeze the board (and you'd have to power-cycle). Now it cleanly tears down Bluetooth and drops into the configuration app, like it always should have.
- **Bluetooth pairings on Pico 2 W are reliable now.** Persisting pairings across reboots used to work by chance — the BT pairing storage and the flash region the linker reserved for it weren't actually aligned, so sometimes a pairing survived a reboot and sometimes it didn't. Now they line up. 100% deterministic.
- **Mode LEDs match reality on Souffle.** USB LED lights up in USB mode, BT LED in Bluetooth mode. Sounds obvious, was wrong before: the BT LED stayed lit even in USB mode.
- **Native Atari ST mouse on Souffle feels right.** With an original ST mouse plugged into the joystick port while running USB mode, the cursor now actually tracks the physical motion — no more crawling, no stutter at slow speeds, no reverses at fast flicks.

### Under the hood

- `cmake --preset` now pins `PICO_PLATFORM` and `DEBUG_MODE` correctly. Iterative builds finally match what `./build.sh` produces.
- Flash layout reorganized: the booster region no longer overlaps the config region, and BTstack TLV storage lives at the address the linker actually reserved.
- UF2 is ~17 KB smaller after dropping unused TinyUSB host class drivers.
- Cleaner booster-app entry: IRQs are masked before the vector-table swap, the new vector is validated first, and the RP2350 SCRATCH_X / SCRATCH_Y memory layout was fixed.
- An attempt to clean up HD6301 SCI receive pacing was shipped, hurt gamepad sensitivity on real hardware, and reverted before release.

## v1.0.0 (2026-01-21) - release

First official release of the 1.0.x series. The code has been tested and is considered stable.

### Changes

### New features

### Fixes
