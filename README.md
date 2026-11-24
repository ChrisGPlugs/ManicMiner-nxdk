# Manic Miner for the original Xbox

A native port of **Manic Miner** (Matthew Smith, Bug-Byte 1983) from the 48K ZX Spectrum to the original Xbox, built with [nxdk](https://github.com/XboxDev/nxdk) and its SDL2 port.

This isn't an emulator. The game's Z80 code was translated routine by routine into C ([`src/mm_game.c`](src/mm_game.c)), working from the annotated disassembly by William Humphreys and Simon Brattel: [WHumphreys/Manic-Miner-Source-Code](https://github.com/WHumphreys/Manic-Miner-Source-Code). The original memory map is kept:

* The game's 64K memory image (`ManicMiner.bin`) is loaded into a 64K array and used unchanged.
* Every cavern, sprite, tune, buffer and variable lives at its original Spectrum address (see [`src/mm_mem.h`](src/mm_mem.h)). The C code reads and writes those addresses exactly as the Z80 code did, including the self-modifying `LD D,n` at `0x8A9C`.
* The screen is drawn into the real display file at `0x4000` and attributes at `0x5800`, which the front end turns into pixels.
* Every routine charges its Z80 T-state cost to a 3.5 MHz clock. The game therefore runs at the original speed, and the in-game music, the title-screen Blue Danube and the sound effects come out of a beeper model at the original pitch.

## Verified against the original

[`tools/difftest.c`](tools/difftest.c) runs the original `ManicMiner.bin` on a Z80 emulator and runs the C port, feeding both the same scripted input. At every pass through the main loop (`0x870E`) it hashes all of RAM: display file, buffers, variables and cavern data. It also records the T-states since the previous pass. Results:

| Script | What it covers | Result |
|---|---|---|
| `demo` | Title screen, theme tune, scrolling banner, demo mode through all 20 caverns | identical, 1,400 passes |
| `play:1` … `play:5` | Random play with jumps, conveyors, crumbling floors, deaths, game over, extra lives, music toggle, pause, Kempston input, the `6031769` cheat and teleports to every cavern | identical, 12,000 passes per seed |
| `finale` | Completing The Final Barrier without the cheat (swordfish ending, air countdown) | identical |

Timing is within 0.1% of the Z80 original.

```sh
make -f Makefile.host test      # needs ManicMiner.bin in the repo folder
```

## Getting the game data

`ManicMiner.bin` is **not** in this repository: Manic Miner is © Matthew Smith. Download it from the disassembly project:

<https://github.com/WHumphreys/Manic-Miner-Source-Code/blob/master/ManicMiner.bin>

It must be the 65,536-byte memory image. The port checks for it at start-up.

**Optional:** put a 16K ZX Spectrum 48K ROM image named `48.rom` next to it and the port uses its character set for authentic text. Without it, a public-domain 8×8 font ([font8x8](https://github.com/dhepper/font8x8)) is used.

## Building for Xbox

1. Set up nxdk ([instructions](https://github.com/XboxDev/nxdk/wiki/Getting-Started)) and point `NXDK_DIR` at it.
2. Copy `ManicMiner.bin` (and `48.rom` if you have it) into this folder.
3. Build:

```sh
export NXDK_DIR=/path/to/nxdk
make
```

This produces `bin/default.xbe`, with `ManicMiner.bin` copied next to it, and `ManicMiner.iso`. Either copy the `bin/` folder to your Xbox (e.g. `E:\Apps\ManicMiner\`) and launch `default.xbe`, or burn or run the ISO. The game data is loaded from the same folder as the XBE (`D:\`).

Tested to build with clang 18 against nxdk master (November 2026).

## Controls

| Xbox controller | Spectrum | Action |
|---|---|---|
| D-pad / left stick ← → | Kempston, O / P | Walk |
| A or B | Kempston fire, M | Jump (also resumes after pause) |
| Start | ENTER | Start game (on the title screen) |
| Y | H | In-game music on / off |
| X | A | Pause (any move or jump resumes) |
| Back | SHIFT + SPACE | Quit to title screen |
| White | 6031769 | Types the original cheat code (a boot appears next to the lives) |
| White / Black, with cheat on | 6 + 1–5 | Teleport to the previous / next cavern |

The desktop build maps the PC keyboard straight onto the Spectrum keyboard (arrow keys also work; Esc quits).

## Desktop build (for development)

```sh
make -f Makefile.host            # needs SDL2 development files
./manicminer                     # ManicMiner.bin in the current directory
```

Debug environment variables (desktop only): `MM_FAST=1` (no frame pacing), `MM_EXIT_AFTER=n`, `MM_SHOTS=f1,f2` (save BMP screenshots), `MM_ENTER_AT=f`, `MM_PCM=file` (dump audio).

## Layout

| File | |
|---|---|
| `src/mm_game.c` | The game: Z80 routines translated to C, same labels as the disassembly |
| `src/mm_mem.h` | The original memory map |
| `src/zx.c`, `src/zx.h` | 64K memory, T-state clock, keyboard/Kempston ports, beeper, screen renderer |
| `src/mm_font.c` | Character set at `0x3D00` (from `48.rom` or built-in) |
| `src/main.c` | SDL2 front end: video, audio, controller, pacing (Xbox and desktop) |
| `tools/difftest.c` | Comparison test against the original Z80 code |
| `tools/z80ref/` | Z80 emulator used only by the test ([superzazu/z80](https://github.com/superzazu/z80), MIT, patched for 16-bit port addresses) |

## Credits and licence

* Manic Miner © 1983 Matthew Smith. All rights reserved. The game data isn't distributed here.
* Disassembly: William Humphreys, with Simon Brattel ([WHumphreys/Manic-Miner-Source-Code](https://github.com/WHumphreys/Manic-Miner-Source-Code)).
* Z80 core used by the test: Nicolas Allemand (MIT).
* Fallback font: Daniel Hepper, font8x8 (public domain).
* Port code: MIT licence, see [LICENSE](LICENSE).
