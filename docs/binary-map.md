# SF2TURBO.EXE (version 1.0): a map

SF2TURBO.EXE is a Borland C++ 4.x PE32 executable from 1994–95, started under DOS by the
`32RTM` extender. It holds 1 MB of code, and Ghidra finds 7,675 functions in it.

Identified symbols are listed in [symbols.txt](symbols.txt).

## The fight engine is translated arcade code

The fight engine was not rewritten for the PC. It is the 68000 code of the CPS-2 arcade
board, translated instruction by instruction to x86. The signs:

- **Global variables play the role of the 68000 registers.** `d0` is `0x2f45d0` (used by
  4,119 functions), `d1` is `0x2f45d4`, `d2` is `0x2f45d8`, and so on up to `d6` at
  `0x2f45e8`.
- **A current-object pointer works like register `a6`.** It lives at `0x2f4e80` and is used
  by 6,286 of the 7,675 functions. A second one at `0x2f4e84` points to the opponent.
- **An object structure** of about 2 KB uses odd offsets (`+0x15f`, `+0x1cf`, …). It copies
  the memory layout of the arcade board.
- **Positions are 16.16 fixed point** (`0x280000` = 40.0).
- **Debug names are left in the strings**, such as `C_Player->S_F_KYORI`, `ATCK_SIT_PUNCH`
  and `MUSOU_CHECK`, partly in romanized Japanese (*kyori* = distance).

As a result, most functions are tiny, share the same shape and work on the same structure.
Rebuilding that structure unlocks two thirds of the code at once.

## Modules

| Area | Addresses | Functions | Size |
|---|---|---:|---:|
| Fight logic A (translated arcade code) | 110000-13bfff | 1,630 | 174 KB |
| Debug memory allocator | 13c000-13ffff | 119 | 16 KB |
| Front end: init, config, intro, menus, endings | 140000-157fff | 293 | 96 KB |
| Low level: timer, keyboard, VGA, joystick | 158000-15bfff | 49 | 23 KB |
| Front end (sprites, select screen) + main loop | 15c000-16ffff | 115 | 73 KB |
| Fight logic B (translated arcade code) | 170000-1f7fff | 4,614 | 533 KB |
| Files and decompression | 1f8000-1fbfff | 96 | 16 KB |
| Rendering, sound effects, CD | 1fc000-203fff | 196 | 31 KB |
| Sound: KoolKat + HMI SOS + MSCDEX | 204000-20bfff | 195 | 29 KB |
| Borland C library | 20c000-218fff | 368 | 34 KB |

## Entry points

```
entry (110000)
 └─ __startup (2173dc)               Borland C runtime
     └─ main (16d7ea)                memory, CD, sound, config, high scores, tables
         └─ game_flow (148d08)       attract mode, select, fights, endings
             └─ fight_loop (16c2d8)  round loop
                 └─ fight_frame (16b5e4)  one fight frame
```

The game installs two interrupt handlers:

- **keyboard**: `isr_keyboard` (158abe);
- **timer**: `isr_timer` (158b68), at 75 Hz. The game logic advances through a 16.16
  accumulator: 60 steps per second in menus, 75 in fights.

## What the port replaces

- **The Borland C library** stays as recompiled code. It only works on game memory.
- **The KoolKat/HMI sound driver and its `.386` drivers** are replaced by a mixer
  (`src/platform/sfx.c`). The game's sample calls are hooked in `overrides.c`.
- **MSCDEX and the CD**: the data track is read as ISO 9660 and the audio tracks are streamed
  from the `.bin` image (`fs.c`, `cdaudio.c`).
- **The low level** (PIC, PIT, keyboard controller, VGA) is emulated at the I/O-port level
  (`io.c`) and shown or read through SDL3 (`video.c`).

## Next steps for a readable decompilation

Work goes from the leaves up. Each hand-written function is checked against the generated
one.

1. Object structure and pseudo-registers as C types.
2. Borland C library: identify the functions (`memcpy`, `malloc`, `fopen`, …).
3. Files, decompression, palette, sprite rendering.
4. Animation and objects (`obj_anim_step` is done, in `src/game/anim.c`, and has 1,498
   callers).
5. Fight logic, character by character.
6. Front end and main loop.
