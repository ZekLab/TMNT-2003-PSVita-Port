# TMNT (2003, PC) → PS Vita

Static recompilation of the PC version of *Teenage Mutant Ninja Turtles* (Konami, 2003): `TMNT.EXE` is translated to C and
runs on a host runtime that replaces Win32, Direct3D 9, DirectInput 8 and DirectSound. The Mac build is playable (menus,
stages, saves, sound); the PS Vita target is next.

This repository contains no game code or data. You need your own PC copy of the game.

## Layout

- `recomp/` – function discovery, translation sweep and C generation (`discover.py`, `sweep.py`, `gen_c.py`, `gen_tables.py`);
  `recomp/xlat/` is a modified copy of the x86 → C translator from [repentogxm](https://github.com/0xl0cal/repentogxm) (GPL-2.0+).
- `rt/` – runtime: guest memory and dispatch (`runtime.c`, `guest.h`), Win32 (`win32.c`), COM (`com.c`), Direct3D 9 over
  OpenGL (`d3d9.c`, `d3d9_gl.c`), DirectInput (`dinput.c`), DirectSound (`dsound.c`), the Mac platform layer (`plat_sdl.c`)
  and game-specific additions (`game_tmnt.c`: area checkpoints).
- `tools/` – header-to-table generators for the DirectX interfaces and constants, run scripts.
- `docs/` – inventory of the executable and the progress log (Romanian).

## Building on the Mac

1. Extract the game with `innoextract --collisions=rename` into `game/`; the English executable is `game/app/TMNT_EN.EXE`
   and `game/app/TMNT.DAT` must point to the English data file.
2. Fetch the mingw-w64 headers `d3d9.h d3d9types.h d3d9caps.h dinput.h dsound.h` into `ref/headers/`, clone repentogxm into
   `ref/repentogxm`, create `.venv` with `pefile` and `capstone`.
3. `python recomp/discover.py && python recomp/gen_c.py && python recomp/gen_tables.py`
4. `DEVELOPER_DIR=/Library/Developer/CommandLineTools make -j8` (needs SDL2), then `tools/play.sh` (`AUDIO=1` for sound).
