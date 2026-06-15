# Changelog

## v1.2.0 (2026-06-15)

A power-and-thermal pass: the board now does exactly the same job at a fraction of the clock and voltage, so it draws less current and runs cooler — plus a couple of robustness fixes.

### What you'll notice

- **Lower power, runs cooler.** The board now runs at 96 MHz / 1.00 V instead of the previous 225 MHz / 1.20 V. Nothing about how it behaves changes — keyboard, mouse, joystick, USB and Bluetooth all work exactly as before — it just sips less current and stays cooler doing it.
- **The mode LED is dimmer.** On Souffle the USB/BT indicator LED is now dimmed (still clearly lit, just not at full blast). It was on continuously, so at the new low power level it was a surprisingly large slice of the total draw.
- **USB hubs come up more reliably from cold.** Some USB hubs occasionally failed to initialize when the board was powered on from cold. The firmware now gives the hub a moment to stabilize before starting the USB host, so it enumerates reliably.

### Under the hood

- Core clock dropped to 96 MHz at 1.00 V. The floor is set by the **CYW43 radio**, not the keyboard emulation: below ~96 MHz the radio's SPI bus stops enumerating (Bluetooth won't start), while the HD6301 emulator, the Atari serial link and USB are all comfortable far below that.
- The HD6301 emulator core now idles in a low-power wait between its ~1 ms pacing windows instead of busy-spinning a whole core at 100%, with no change to emulation timing.
- The Souffle mode-indicator LED is driven by PWM at low duty instead of full-on.
- USB host start-up is held off briefly after power-on so a connected hub's power/PLL circuit has time to settle before enumeration.

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
