# Atari ST IKBD Bluetooth + USB + Native Emulator with Raspberry Pi Pico 2 W

This project lets you use a humble **Raspberry Pi Pico 2 W** as a full emulator for the **HD6301**, the microcontroller inside the Atari ST keyboard (ST / STe / TT).

In other words: the Pico pretends to be the real IKBD. No hacks. No half measures. The Atari thinks it’s talking to its keyboard. Because it is.

Two carrier boards are supported (both on Pico 2 W / RP2350):

* **Croissant** — Native Atari input path + Bluetooth. You can plug original Atari joysticks and mice into the joystick port.
* **Souffle** — USB host + Bluetooth. USB keyboards / mice / gamepads work directly.

> 🛒 **Get the hardware:** [Croissant](https://sidecartridge.com/products/sidecartridge-croissant-atari-st/) or [Souffle](https://sidecartridge.com/products/sidecartridge-souffle-mega-st/)

Typical use cases:

* **Mega ST / Mega STe / TT with no keyboard**
  The emulator gives you Bluetooth / USB keyboard + mouse + gamepads.
  And you can still plug **native Atari joysticks and mice** into the joystick port.

* **Standard ST / STe** where you just want modern input devices
  Bluetooth keyboard. Bluetooth mouse. Controllers. Done.


## How it works

The Atari ST keyboard isn’t just “a keyboard”. Inside there’s a **HD6301 microcontroller** running firmware. TOS (and also user programs) can send commands to it, and the keyboard replies with events: keys, mouse movement, joystick state… the whole thing.

Communication between the Atari and the IKBD happens over a **serial link**. So you’ve got two options:

1. implement the full IKBD protocol yourself, byte by byte
2. emulate the actual HD6301 so the original firmware logic exists “for free”

This project goes for option 2.

We emulate the **HD6301 CPU** and the hardware around it, so to the Atari it looks like a real keyboard controller. That means:

* maximum compatibility
* software can program it like the real thing
* no “almost works” edge cases

The HD6301 emulator core comes from **Steem SSE**. Credit where credit is due. Also, the original Pico-based IKBD approach deserves a shout-out:
[atari-st-rpikb](https://github.com/fieldofcows/atari-st-rpikb) by **fieldofcows**.

I don’t think much of the original code remains here (besides the 6301 core), but the idea and the inspiration are clearly from there. Give to Caesar what is Caesar’s.


## Building the firmware

Follow `AGENTS.md` in this repo for prerequisites and supported build flows.
In short:

```sh
./build.sh <board_type> <build_type> <release_type> <board_flavor>
```

Examples:

```sh
# Raspberry Pi Pico W
./build.sh pico_w release final croissant

# Raspberry Pi Pico 2 W
./build.sh pico2_w release final souffle

# Debug build (either board)
./build.sh pico_w debug final croissant
./build.sh pico2_w debug final souffle
```

Manual build (no script):

```sh
mkdir -p build
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Using the emulator

1. Build the firmware and copy the UF2 from `dist/` to the board (Pico W or Pico 2 W).
2. Plug the board into the Atari ST IKBD interface using your hardware adapter or carrier board.
3. Power on the Atari. The on-board LEDs tell you which mode the firmware booted into — **Native**, **USB**, or **Bluetooth**.
4. To change modes, pair Bluetooth devices, pick a keyboard layout, choose native vs USB mouse, etc., drop into the **configuration app**:
   * On **Souffle**, hold the **config button**.
   * On **Croissant**, hold the Atari's keyboard reset for 10+ seconds (3–10 s just toggles the IKBD source).

The configuration app is a separate firmware that lives in its own flash region. The IKBD firmware tears down whatever it's doing (USB host, Bluetooth, native input) before handing control over, so the jump is clean. When you're done, reboot and you're back in the active mode.

Notes:
- Bluetooth uses Bluepad32. Some devices may need specific pairing steps. Pairings persist across reboots once configured.
- USB host support is basic. Works for common stuff, not trying to be a full desktop USB stack.

## Known limitations

### Bluetooth limitations

Bluetooth supports **Classic** and **BLE**, but don’t expect miracles. Not every keyboard, mouse or controller behaves nicely.

Some devices may:

* fail pairing
* lose special keys
* have weird rollover behavior
* connect, but then act like they’re half asleep

If something feels off, don’t waste your life debugging it: try another device.

Also, since this project uses **Bluepad32**, supported device compatibility largely depends on Bluepad32 itself. Check their docs / lists for known-good controllers.


### USB limitations

USB host support is intentionally simple. It works for the common stuff, but it’s not trying to be a full desktop USB stack.

So yes:

* some keyboards won’t enumerate correctly
* some mice will work “sort of”
* some devices will have limited functionality

If it doesn’t behave, swap device. That’s the fastest fix.


### Emulator limitations

The HD6301 emulator core comes from **Steem SSE**. And as anyone who has fought Steem / Hatari edge cases knows… some software is a pain.

The problematic programs usually do one of these:

* run **custom code** on the HD6301
* rely on non-standard IKBD behavior
* send weird commands that are “technically legal” but basically evil

So: most things work. Some things might not. Here’s the list of known troublemakers.

| Title                            | Tested | Working | Notes                                |
| -------------------------------- | ------ | ------- | ------------------------------------ |
| Carrier Command                  | ❓      | ❓       |                                      |
| Corporation Megademo.            | ✅      | ✅       |                                      |
| Dragonnels Demo                  | ✅      | ✅       |                                      |
| Fokker.prg                       | ✅      | ✅       |                                      |
| Froggies                         | ✅      | ❌       | Doesn’t move at all. Not even a bit. |
| Hades Nebula                     | ✅      | ✅       |                                      |
| Hammerfist                       | ✅      | ❌       | Not responding to any key presses    |
| Jumping Jackson (Auto 239)       | ❓      | ❓       |                                      |
| Lightning Demo                   | ✅      | ✅       |                                      |
| Manchester United (FOF 12)       | ❓      | ❓       |                                      |
| Over The Fence Demo              | ❓      | ❓       |                                      |
| Overdrive Demo                   | ❓      | ❓       |                                      |
| Pandemonium Demo                 | ❓      | ❓       |                                      |
| Platoon                          | ❓      | ❓       |                                      |
| RipDis                           | ❓      | ❓       |                                      |
| Sentinel (Auto 1)                | ❓      | ❓       |                                      |
| The Final Conflict [2] (Med 101) | ❓      | ❓       |                                      |
| UUS John Young                   | ❓      | ❓       |                                      |
| V8 Music disk                    | ❓      | ❓       |                                      |
| X-Out                            | ❓      | ❓       |                                      |
| Yogi Bear                        | ✅      | ✅       |                                      |

## Acknowledgements

This project is basically a Frankenstein built from good parts.

The HD6301 side comes from code extracted from [Steem SSE](https://sourceforge.net/projects/steemsse/). Steem did the real hard work: wiring up the IKBD hardware and its behavior to the HD6301 CPU core. This project carries a stripped-down version of that interface, adapted to run on a Raspberry Pi Pico W and talk to the Atari over the serial link.

Steem itself uses the HD6301 emulator core from **sim68xx**, developed by **Arne Riiber**. The original website is long gone, but thankfully an archive still exists [here](http://www.oocities.org/thetropics/harbor/8707/simulator/sim68xx/).

Bluetooth support is powered by [Bluepad32](https://github.com/ricardoquesada/bluepad32), developed by **Ricardo Quesada**. For USB host support it uses **TinyUSB**, and of course everything is built on top of the official **Raspberry Pi Pico SDK**.

And one last thing.

Sometimes [Bluetooth on the Pico W is just... brutal](https://github.com/raspberrypi/pico-sdk/issues/2725). The stack is complex, the behavior is inconsistent, and [issues can be ridiculously hard to reproduce](https://github.com/ricardoquesada/bluepad32/pull/199). And no, you won’t solve it by asking an LLM agent to “fix the code”. Codex won’t. Claude Code won’t. Not really.

At some point it’s just the old way: logs, traces, trial and error, and asking the right experts.

## License

This project is licensed under the GNU General Public License v3.0. See the LICENSE file for details.

## Copyright

Except where otherwise noted, this project is copyright of GOODDATA LABS S.L. All rights reserved.
