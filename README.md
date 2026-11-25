# Manic Miner for the original Xbox

Native ports of **Manic Miner** to the original Xbox, built with [nxdk](https://github.com/XboxDev/nxdk) and its SDL2 port:

* **Manic Miner** (Matthew Smith, Bug-Byte 1983), from the 48K ZX Spectrum: `bin/default.xbe`.
* **Manic Miner 128** (Mihai Novitchi's 128K/AY version of the Software Projects release, as fixed and re-packaged by Bob Fossil): `mm128/bin/default.xbe`. See [Manic Miner 128](#manic-miner-128) below.
* **Both in one XBE**: `combined/bin/default.xbe`. See [Combined build](#combined-build).

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
make -f Makefile.host test      # needs ManicMiner.bin / ManicMiner128.tap in the repo folder
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
| Left stick click | | Diagnostics overlay (both versions) |
| LT + RT + Back + Start, or LT + RT + Back + Black | | In-game reset: back to the dashboard (`HalReturnToFirmware(1)`) |

The desktop build maps the PC keyboard onto the Spectrum keyboard (arrow keys also work; Esc quits). The Xbox build uses the controller only.

**How the teleport button works.** The game teleports when its teleport check sees the activator key (`6`, or `9` on Manic Miner 128) together with the cavern number in binary on keys `1`–`5`. Holding those keys down has side effects. On Manic Miner 128, key `1` also opens Mihai's options menu, which would toggle infinite lives and block the teleport for odd-numbered caverns. On both versions, key `5` also means "left". So the button doesn't hold keys. It answers exactly the two port reads the teleport check makes: the `0`–`6` row shows the activator, and the `1`–`5` read that immediately follows it shows the cavern number. Every other read sees `1`–`5` released. The game code is unchanged; the result is the same as a human pressing the keys at exactly the right moment. It fires once per press and reaches every cavern.

## Sound and timing on the Xbox

The game is paced by its sound: the emulated Spectrum runs only as fast as its audio is played, with about 80 ms queued. On the Xbox the AC97 controller is fed directly (not through SDL's audio thread): one DMA buffer per Spectrum frame in a ring of 32, with the queue level read from the controller's Current Index register. Speed and pitch therefore follow the 48 kHz hardware clock exactly. The picture is drawn straight into the framebuffer at 2×, and only when the Spectrum screen has changed. If the queue drops below ~40 ms, frames are emulated without being drawn until it recovers, so the sound never waits for the picture.

**Memory contention.** A real Spectrum's ULA delays CPU access to 0x4000–0x7FFF while it fetches the screen (6,5,4,3,2,1,0,0 T-states across each 8-T cycle, for the first 128 T-states of each display line). Manic Miner keeps its screen and attribute buffers there and copies about 9 KB through them every pass, so contention slows the real game by about 20%. The port models it: `LDIR` cycle by cycle (`hl:3, de:3, de:1×2, de:1×5`), ULA port I/O exactly, and other buffer accesses at the pattern's average of 21/8 T-states. 48K timing is used for Manic Miner and 128K timing (70,908-T frames at 3.5469 MHz) for Manic Miner 128. The comparison tests switch contention off, because the reference Z80 core doesn't model it.

A stopwatch check against a real Spectrum or a contention-accurate emulator such as JSSpeccy: start a game and don't touch anything. In Central Cavern, Willy dies when the air runs out:

| Version | Cavern appears → Willy dies of lack of air |
|---|---|
| Manic Miner (48K) | 154 s (128 s without contention) |
| Manic Miner 128 (128K machine) | 144 s (117 s without contention) |

Clicking the left stick shows a diagnostics box:

| Line | Meaning |
|---|---|
| `SPEED` | emulated frames per second, timed by the Xbox's system clock; should match the `REAL` figure (50.08 on 48K, 50.02 on 128K) |
| `AUDIO RATE` | samples played per second; should read about 48000 |
| `AUDIO QUEUED` / `GAPS` | audio waiting to play (~60–100 ms), and how many times it ran dry (each is an audible gap; should stay 0) |
| `DRAW` / `SKIPPED` | average time to draw a frame; frames emulated without drawing, to catch up |
| `ULA CONTENTION` | share of emulated time spent waiting for the ULA (about 17%) |

## Desktop build (for development)

```sh
make -f Makefile.host            # needs SDL2 development files
./manicminer                     # ManicMiner.bin in the current directory
./manicminer128                  # ManicMiner128.tap in the current directory
```

Debug environment variables (desktop only): `MM_FAST=1` (no frame pacing), `MM_EXIT_AFTER=n`, `MM_SHOTS=f1,f2` (save BMP screenshots), `MM_ENTER_AT=f`, `MM_KEYS=f:c,...` (press keys), `MM_PCM=file` (dump audio).

## Layout

| File | |
|---|---|
| `src/mm_game.c` | The game: Z80 routines translated to C, same labels as the disassembly |
| `src/mm_mem.h` | The original memory map |
| `src/zx.c`, `src/zx.h` | 64K memory, T-state clock, keyboard/Kempston ports, beeper, screen renderer |
| `src/mm_font.c` | Character set at `0x3D00` (from `48.rom` or built-in) |
| `src/main.c` | SDL2 front end: video, audio, controller, pacing (Xbox and desktop) |
| `src/mm128_load.c` | Manic Miner 128: TAP parsing and the tape's unpacker |
| `src/st_player.c` | Manic Miner 128: Sound Tracker AY player, translated |
| `src/ay.c` | AY-3-8912 emulation |
| `mm128/` | nxdk build for Manic Miner 128 |
| `combined/` | nxdk build with both games and the start-up chooser |
| `tools/difftest128.c` | Comparison test for Manic Miner 128 |
| `tools/difftest.c` | Comparison test against the original Z80 code |
| `tools/z80ref/` | Z80 emulator used only by the test ([superzazu/z80](https://github.com/superzazu/z80), MIT, patched for 16-bit port addresses) |

## Manic Miner 128

[Manic Miner 128](https://jswcentral.org/mminmods_mm128ay.html) is Mihai Novitchi's early-2000s version of the Software Projects edition. It has AY music (a full "In the Hall of the Mountain King") and an in-game options menu. Bob Fossil fixed and re-packaged it in 2022–23 ([his page](http://www.thefossilrecord.co.uk/coding/room-for-improvements/)). No source exists for it. This port is built from the tape **"Manic Miner 128 by Mihai Novitchi and Bob Fossil - Software Projects version"**: the version with the in-game menu, not the one with a trainer menu at start-up.

How it was ported:

* The tape's own unpacker (a backwards Elias-gamma LZ routine at 30000) is translated to C ([`src/mm128_load.c`](src/mm128_load.c)). At start-up the Xbox builds the same 64K memory image the Spectrum would have, byte for byte, including the loader's three patches.
* The game was disassembled and aligned instruction by instruction against the Bug-Byte disassembly. Most of it is the same code moved up by 6–48 bytes. The differences are compiled into the shared engine with `-DMM128` ([`src/mm_game.c`](src/mm_game.c), [`src/mm_mem.h`](src/mm_mem.h)):
  * Software Projects keys: cheat **TYPEWRITER**; teleport by holding **9**; `5`/`6` left, `7`/`8` right, `0` jump; ENTER or `0` starts.
  * Caverns, title screen and title attributes are stored packed and unpacked by the game's LZ routine (37835), translated with its self-modifying hooks.
  * The beeper in-game tune is gone. The AY tune restarts at every cavern and life, driven by an IM2 interrupt (vector table 39680 → handler 40092).
  * Mihai's options menu (key `1`): infinite lives, infinite air, music on/off. As in the original, the options patch the game's own code (`DEC (HL)` at 35147, `CALL DecreaseAirRemaining` at 34843 and 36264), and the engine checks those bytes.
* The Sound Tracker music player (49152) is translated to C ([`src/st_player.c`](src/st_player.c)) and runs from a 50 Hz interrupt. The front end has an AY-3-8912 emulation ([`src/ay.c`](src/ay.c)) at the Spectrum 128's 1.7734 MHz clock.

Verified the same way as the 48K game ([`tools/difftest128.c`](tools/difftest128.c)). The original tape is run on a Z80 emulator with real 50 Hz IM2 interrupts, and memory is compared at every main-loop pass (34622). Each music tick's AY registers are compared too:

| Script | Result |
|---|---|
| `demo` (title, all 20 caverns) | identical, 1,400 passes |
| `play:1` … `play:6` (TYPEWRITER, teleports, menu: lives/air/music toggles, deaths, game over) | identical: seeds 3–6 for 12,000 passes each, seeds 1–2 for 6,000 |
| AY register stream | identical tick for tick; the only differences are ±1 tick where the tune restarts, because a restart can land a fraction of a frame apart |

**Known difference: a bug in the original that the port doesn't have.** In this version, a collision with a *vertical* guardian jumps to `KillWilly2` without dropping its return address, so each such death leaves 2 bytes on the Z80 stack. Nothing resets the stack until the game is reloaded, not even returning to the title screen. After roughly 28 vertical-guardian deaths in one session, the music interrupt's pushes overwrite its handler at 40092 and the game crashes. The comparison test hit this with infinite lives on in Solar Power Generator. The C port has no Z80 stack, so it keeps running.

### Playing it

1. Get the TAP from Bob Fossil's page or the [JSW Central page](https://jswcentral.org/mminmods_mm128ay.html). The port checks that it is this exact tape (code block 15,212 bytes at 30000).
2. Rename it to **`ManicMiner128.tap`**. The original file name is longer than the Xbox's 42-character limit.
3. Put it next to `mm128/bin/default.xbe` (optionally with `48.rom`), or in the repo's top folder before `make -C mm128`.

| Xbox controller | Spectrum | Action |
|---|---|---|
| D-pad / left stick ← → | Kempston, O / P | Walk |
| A or B | Kempston fire, M | Jump |
| Start | ENTER + 4 | Start game; close the options menu |
| Y | 1 | Open the options menu; in the menu, toggle infinite lives |
| Left trigger / Right trigger | 2 / 3 | In the menu: infinite air / music on-off |
| X | A | Pause |
| Back | SHIFT + SPACE | Quit to title |
| LT + RT + Back + Start, or LT + RT + Back + Black | | In-game reset (back to the dashboard) |
| White | TYPEWRITER | Types the cheat code |
| White / Black, with cheat on | 9 + 1–5 | Teleport to the previous / next cavern (any cavern; see below) |

## Combined build

`make -C combined` builds one XBE containing both games (both engines are linked in, as `mm_run48` and `mm_run128`). At start-up it looks next to `default.xbe`:

| Files present | What happens |
|---|---|
| `ManicMiner.bin` only | Manic Miner (Bug-Byte) starts |
| `ManicMiner128.tap` only | Manic Miner 128 starts |
| both | a chooser |
| neither | a message saying which files to add |

A file that is present but isn't the expected one (wrong size, unsupported tape) counts as missing, so the other game still starts.

The chooser is built from Manic Miner's own title screen, taken from `ManicMiner.bin`: the house and sun, the MANIC MINER band and the piano keyboard. Willy walks at the top right, and the Blue Danube plays on the beeper with the piano keys lighting up. It uses the original's note loop and key mapping, so the border flickers in time just like the original. The bottom third lists the two games, with the selected one in the yellow cavern-name style, followed by a scrolling banner and a hint line.

| Chooser control | |
|---|---|
| D-pad / left stick up-down | choose |
| A or Start | play |

Once a game is chosen it runs exactly as in its own XBE, with the same controls, in-game reset and diagnostics. Returning to the chooser means restarting the XBE (IGR).

## Credits and licence

* Manic Miner © 1983 Matthew Smith. All rights reserved. The game data isn't distributed here.
* Manic Miner 128: Mihai Novitchi (128K version, AY arrangement), Bob Fossil (fixes and TAP). Not distributed here.
* Disassembly: William Humphreys, with Simon Brattel ([WHumphreys/Manic-Miner-Source-Code](https://github.com/WHumphreys/Manic-Miner-Source-Code)).
* Z80 core used by the test: Nicolas Allemand (MIT).
* Fallback font: Daniel Hepper, font8x8 (public domain).
* Port code: MIT licence, see [LICENSE](LICENSE).
