# SSF2T-Linux-port

A native Linux port of **Super Street Fighter II Turbo** for DOS (GameTek, 1995), with no DOSBox
and no emulator. The original executable is statically recompiled to C, then linked with a
small SDL3 platform layer that replaces the DOS hardware: graphics, keyboard, gamepads, sound
effects and CD music.

This is a nostalgia project made by a fan, for fans of the game, and for **educational
purposes**: it shows how a 1990s DOS game can be brought back to a modern system by
reverse engineering and static recompilation.

> **You need the complete original game.** This repository contains **no game code and no game
> data** (graphics, sounds, music, executable). The game's code is generated on your machine
> from your own copy of `SF2TURBO.EXE`, and all assets are read from your game folder at run
> time.

## What works

- Intro, menus, all modes, fights, endings
- CD music (streamed from the CD image) and sound effects
- Keyboard for two players, gamepads (hot-plug, one per player)
- Configurable controls (`sf2t.ini`)
- Optional **xBR 4x** smoothing filter (`--hires`), switchable in game with Alt+F
- Fullscreen with Alt+Enter

## Requirements

**The original game**, complete:

- the **installed game folder** (full install), containing `SF2TURBO.EXE` version 1.0
  (sha256 `279144e7d6e9ab6c6a65c72dc367ebef9a0c6113153594525e444d399407167a`);
- the **CD image** as `.cue` + `.bin`, for the music.

**To build:**

- `gcc` and `make`;
- Python 3 with [capstone](https://www.capstone-engine.org/) 5, used once to generate the
  game code (`pip install --user capstone`, or in a virtualenv);
- **SDL3** at run time (`libSDL3.so.0`, e.g. `sdl3` on Fedora, `libsdl3-0` on Debian/Ubuntu).
  No development headers are needed: SDL3 is loaded dynamically.

## Build

```sh
git clone https://github.com/mhamedbenjmaa/SSF2T-Linux-port.git
cd SSF2T-Linux-port
make GAME=/path/to/game -j8     # first build: generates src/recomp/ (about 30 s)
```

Later builds only need `make -j8`. `make distclean` removes the generated game code.

## Run

The first time, tell the program where the game is. The paths are then saved in the settings
file:

```sh
./sf2t --game /path/to/game --cd /path/to/SF2.cue
```

After that:

```sh
./sf2t            # original pixels
./sf2t --hires    # xBR 4x smoothing (or ./sf2t-hires)
```

The game paths can also be given with the `SSF2T_GAME` and `SSF2T_CD` environment variables.
Without a CD image, the game runs without music.

**The original game files are never modified.** Saves, high scores and settings go to
`~/.local/share/ssf2t/` (or `$XDG_DATA_HOME/ssf2t`).

## Controls

| | Player 1 | Player 2 |
|---|---|---|
| Directions | W A S D | P ; L ' |
| Punches (light, medium, heavy) | T Y U | Insert Home PageUp |
| Kicks (light, medium, heavy) | G H J | Delete End PageDown |

These are the game's default keys. Enter is Start and Escape goes back.

Gamepads, Xbox layout: d-pad or left stick to move, X Y RB for punches, A B RT for kicks,
Start, Back. The first gamepad plugged in is player 1.

| Shortcut | Action |
|---|---|
| Alt+Enter | Fullscreen |
| Alt+F | Smoothing filter on/off |

Everything can be changed in `~/.local/share/ssf2t/sf2t.ini`, created on the first run with
comments:

- `[player1]` and `[player2]`: `key.<action>` for keyboard keys and `pad.<action>` for gamepad
  buttons;
- `[general]`: stick dead zone and trigger threshold;
- `[video]`: `filter = xbr | none`;
- `[paths]`: the game folder and the CD image.

## How it works

1. **Static recompilation.** `tools/recomp.py` disassembles `SF2TURBO.EXE` (a 32-bit Borland
   C++ program run under the 32RTM DOS extender) with Capstone. It translates every function
   into C that runs on an emulated register set, using the function boundaries listed in
   `tools/function-ranges.json` (addresses only). The output goes to `src/recomp/` and is
   ignored by git.
2. **Platform layer** (`src/platform/`). This replaces what the game expected from DOS and
   the PC:
   - DPMI and DOS services, MSCDEX, and the Win32 calls of the extender;
   - the timer, keyboard controller and VGA, through SDL3;
   - sound: the HMI SOS sound driver is replaced by a small mixer, and the CD audio tracks are
     read from the image.
3. **Hand-written replacements** (`src/game/`, `src/platform/overrides.c`). Functions listed
   in `overrides.txt` are replaced with readable C. This is the start of a real decompilation.

The game itself runs on its own thread, and the main thread displays frames and reads input.
Notes on the original binary are in [docs/binary-map.md](docs/binary-map.md) and
[docs/symbols.txt](docs/symbols.txt).

## Legal

This is an unofficial, non-commercial fan project, not affiliated with or endorsed by Capcom
or GameTek. *Street Fighter* and *Super Street Fighter II Turbo* are trademarks of Capcom.

No copyrighted game code or data is distributed here. Use this project only with a copy of
the game you legally own.

The code in this repository is released under the [Apache License 2.0](LICENSE). It covers
only the code written for this project, not the original game.
