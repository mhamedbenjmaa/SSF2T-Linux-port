# SSF2T-Linux-port — notes for Claude Code sessions

Native Linux (and later Windows) port of **Super Street Fighter II Turbo** (DOS, 1995,
GameTek/Capcom), built by statically recompiling the original executable to C. No DOSBox,
no emulator. It works: intro, menus, fights, CD music, sound effects, keyboard, gamepads,
optional xBR 4x smoothing filter.

## Working with the user

- **Reply in French, short and simple.** The user knows the game by heart: ask them to test
  gameplay and report, instead of running long automated menu tests yourself.
- **Never chain actions they did not ask for.** Propose, wait for a yes. On "stop", stop at once.
- Don't install things without asking (no lib32 packages, no Node, etc.). They can't run sudo
  through `!`; give them the command to run in their own terminal.
- **Never modify the original game files** (game folder, CD image). Saves and config go to
  `~/.local/share/ssf2t/` (or `$XDG_DATA_HOME/ssf2t`, `%APPDATA%/ssf2t` on Windows).
- This repo is public (github.com/mhamedbenjmaa/SSF2T-Linux-port, Apache 2.0). **Never commit
  game code or data**: `src/recomp/` generated files, the exe, assets, the CD image
  (.gitignore covers them). No links to ROM/download sites.
- **No Claude attribution**: no `Co-Authored-By` lines, commits are the user's only.
- Commit/push only when asked.

## Layout

```
Makefile        make GAME=/path/to/game -> generates src/recomp/ once, then builds ./sf2t
                (gen_*.c at -O1 -w, the rest -O2); make distclean drops the generated code
sf2t-hires      launcher = sf2t --hires
overrides.txt   functions whose F_xxxxxx is hand-written instead of linked to R_xxxxxx
include/sf2t/   mem.h (g_mem, rd/wr), obj.h (fight object fields), game.h
src/recomp/     GENERATED (git-ignored): gen_00..19.c, funcs.h, functable.c, links.c, apis.h;
                hand-written: recomp.h (CPU state, flags, macros), runtime.c
src/platform/   main.c video.c io.c sys.c sfx.c cdaudio.c fs.c win32api.c memory.c
                overrides.c controls.c filter.c
src/game/       hand-rewritten game functions (anim.c = 0x16eee6 obj_anim_step)
tools/          recomp.py (Capstone 5; checks the exe sha256), map_functions.py,
                function-ranges.json (Ghidra function boundaries, addresses only)
docs/           binary-map.md, symbols.txt (identified addresses)
```

The game (SF2TURBO.EXE v1.0, Borland C++ PE32 under the 32RTM DOS extender) and its CD image
(SF2.cue + SF2.bin: track 1 ISO9660, tracks 2-45 CD audio) live outside the repo. On the
author's machine: `../G` and `../cd/SF2.cue`. The pre-publication local history (tags v1, v2,
with generated code) is in `../private-history.bundle`; never push it.

## How the port works

- **Recompiler**: every original function becomes `R_xxxxxx(void)` on a global CPU state `R`
  (registers) with memory `M8/M16/M32` over `g_mem` (game addresses as-is). Calls go through
  `F_xxxxxx`; `links.c` maps F->R except the addresses listed in `overrides.txt`.
  - Local flags struct `F`, saved/restored around calls (FLAGS_IN/FLAGS_OUT).
  - `IRQ_POLL()` at loop back-edges, driven by a 1 kHz SDL timer (`g_irq_tick`), delivers
    timer/keyboard IRQs with full register save/restore like a DPMI host.
  - Shadow return stack (`RS_PUSH`/`RET`); `computed_return()` handles Borland's
    `int n; ret` stubs at 0x20dc00 and other non-standard returns.
  - Import table entries hold `0xFFFF0000|index` -> `api_dispatch` (win32api.c).
  - Hand-written overrides end with `HW_RET(n)`.
- **Platform layer** (SDL3 loaded with dlopen/LoadLibrary, no dev headers needed):
  - The game runs on its own thread; the main thread presents frames and reads input
    (a vsync'd RenderPresent inside the game thread caused random freezes).
  - io.c: PIC, PIT (75 Hz), keyboard controller (key held >= 40 ms), VGA DAC and retrace
    (70 Hz, each retrace reported once).
  - sys.c: LDT/DPMI/DOS/MSCDEX/int10 services. fs.c: DOS files mapped to G/ + save dir.
  - Sound: the HMI SOS driver is replaced by sfx.c (32 voices, 16 kHz mixer, per-sample rate:
    the game sets pitch = rate * 4.096). Overrides 140d29/1410d1/1411f1/141224/141278/1412a1.
    CD music: cdaudio.c streams the .bin tracks at 44.1 kHz.
  - `F_16b5e4` override sleeps while the game waits for its logic tick (no busy spin).
  - Game paths are never hard-coded: `--game DIR` / `--cd FILE.cue`, else env `SSF2T_GAME` /
    `SSF2T_CD`, else `[paths]` in sf2t.ini (written when given on the command line). No game
    folder -> clear error. All graphics/sounds/tables are read from the game folder at run time.
  - controls.c: settings file `sf2t.ini` (was controls.ini, renamed on first run): `[paths]`
    (kept last), `[video] filter`, `[general]` stick/trigger, `[player1]`/`[player2]` key.* pad.*.
    Every binding presses that player's default game key.
  - filter.c: xBR 4x (Hyllian), 320x200 palette indices -> 1280x800, 4 threads, ~1 ms/frame.
    `--hires`/`--lowres`, Alt+F toggles, Alt+Enter fullscreen.
- **Debug env vars**: `SSF2T_SHOTS=dir` (PPM screenshot every 2 s), `SSF2T_INPUT="sec:scancode,..."`
  (scripted keys, hex, >0xFF = E0 prefix), `SSF2T_STALLS`; `-v` for logs.

## Game facts

- Fight code is translated 68000 arcade code: pseudo-registers d0..d6 at 0x2f45d0..0x2f45e8,
  current object pointer 0x2f4e80 (a6), other object 0x2f4e84; objects ~2 KB, odd offsets.
- Logic tick: 16.16 accumulator 0x2f02b4 += 0x2f02b8 (0xcccc in menus = 60/s, 0x10000 in fights).
- Input objects: P1 0x2e7b54, P2 0x2e8385; +0x73 current bits (1/2/4/8 directions,
  0x10/0x20/0x40 punches, 0x100/0x200/0x400 kicks), +0x77 previous. Key table 0x2f01a4.
- Options menu values: words at 0x229824 (index 7 PARALLAX -> 0x22e4e4, 8 FRAME LOCK ->
  0x22e4ec: waits for VGA retrace before drawing a fight frame; 0x229830 BACKGROUND ANIMATION).
- Default keys: P1 W A S D, punches T Y U, kicks G H J; P2 P ; L ', Ins Home PgUp, Del End PgDn.

## Possible next steps (ask the user first)

- Windows build (code has `_WIN32` paths; needs MinGW/CMake and SDL3.dll).
- Readable decompilation: replace generated functions with clean C in src/game/, verifying
  each against the generated version.
- Stereo panning for sound effects; in-game controls menu; respecting keys redefined in the
  game's own options instead of its defaults.
- Never-hit unsupported instructions (ljmp, aam) abort with "unsupported code at ...".
