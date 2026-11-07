# Teenage Mutant Ninja Turtles (2003) for PS Vita

An unofficial port of *Teenage Mutant Ninja Turtles* (Konami, 2003, PC version) to the PS Vita.

> **This project does not include any game code or data.** You need your own copy of the PC game. The game's executable is
> translated to C on your computer and its data files are copied to the Vita.

## Status

- The game is playable on the Vita: menus, stages, saves, checkpoints, sound and movies.
- Levels run at a steady 30 FPS most of the time; in the heaviest fights the game slows down slightly instead of dropping
  frames.
- Movies play through the Vita's hardware video decoder, after a one-time conversion on a Mac (see [Movies](#movies)).

## How it works

The port is a static recompilation: every function of `TMNT_EN.EXE` (x86, 32-bit) is translated to C ahead of time and
compiled for the Vita's ARM CPU. A host runtime replaces what the game expected from Windows:

| The game uses | The port provides |
| --- | --- |
| Win32 (files, threads, timers, window) | `rt/win32.c`, on the Vita kernel |
| Direct3D 9 (RenderWare 3.5) | `rt/d3d9.c` + `rt/d3d9_gl.c`, OpenGL through vitaGL, drawn on a separate render thread |
| DirectInput 8 | `rt/dinput.c`, the Vita's buttons as a game pad |
| DirectSound (CRI ADX) | `rt/dsound.c`, SceAudioOut at 48 kHz |
| Sofdec movies (MPEG-1) | `rt/movie.c`, H.264 on the hardware decoder |

Hot paths of the game (skinning, cel shading, animation curves, collision, render state flushes) have native versions in
`rt/game_tmnt.c`, checked against the translated code with `TMNT_HLE_CHECK=1`.

## What you need

| Item | Why |
| --- | --- |
| A PS Vita with homebrew enabled (HENkaku/Ensō) | To run the game. |
| `libshacccg.suprx` extracted on the Vita | vitaGL compiles its shaders with it. |
| The PC game, English executable `TMNT_EN.EXE` | The code and data come from it. See [Supported version](#supported-version). |
| A Mac | To build the port and convert the movies (Windows and Linux are not supported yet). |
| About 2.5 GB free on the Vita | Game data 2.0 GB, converted movies 0.45 GB. |

### Supported version

The translated code matches one exact executable:

| File | Size | SHA-1 |
| --- | --- | --- |
| `TMNT_EN.EXE` | 2,150,400 bytes | `4e56dd020e9b3c9fdf755f3d655fd5f2a84e4777` |

It comes from the English/Russian PC release, which installs without copy protection. The US and European retail
discs use SecuROM 4, so their executables are different and do not work.

## Building

1. Extract the game with `innoextract --collisions=rename` into `game/`. The English executable is
   `game/app/TMNT_EN.EXE`, and `game/app/TMNT.DAT` must point to `TMNT_EN.DAT`.
2. Fetch the mingw-w64 headers `d3d9.h d3d9types.h d3d9caps.h dinput.h dsound.h` into `ref/headers/`, clone
   [repentogxm](https://github.com/0xl0cal/repentogxm) into `ref/repentogxm`, and create `.venv` with `pefile` and
   `capstone`.
3. Translate the executable:

   ```bash
   .venv/bin/python recomp/discover.py
   .venv/bin/python recomp/gen_c.py
   .venv/bin/python recomp/gen_tables.py
   ```

   This writes the C code to `recomp/out/c/` and the game's memory image to `recomp/out/image.bin`. Both are made from
   your executable and stay on your computer.
4. **Vita:** with [VitaSDK](https://vitasdk.org) installed, `cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release && cmake --build build/vita -j8`
   gives `build/vita/TMNTVita.vpk` (a full build takes about 10 minutes).
5. **Mac (optional, for testing):** `DEVELOPER_DIR=/Library/Developer/CommandLineTools make -j8` (needs SDL2), then
   `tools/play.sh` (`AUDIO=1` for sound).

## Installation

1. Install `build/vita/TMNTVita.vpk` with VitaShell.
2. Create `ux0:data/TMNT2003/` and copy into it, from the game folder:
   `TMNT_EN.DAT` (renamed to `TMNT.DAT`), `TMNT_V00.DAT`, `TMNT_V01.DAT`, `TMNT_V02.DAT`, `bgm.afs`, `se.afs`, `voice.afs`.
3. Copy `recomp/out/image.bin` to `ux0:data/TMNT2003/`.
4. Copy the converted movies to `ux0:data/TMNT2003/movies/` (see [Movies](#movies)).
5. Start the game from the LiveArea.

### Movies

The Vita is too slow to decode the game's MPEG-1 movies in software, so they are converted once to H.264 for its hardware
decoder:

```bash
mkdir -p build/movconv && cd tools/movies
clang -O2 -c plm_frames.c -o ../../build/movconv/plm_frames.o
swiftc -O -swift-version 5 -import-objc-header plm_frames.h movconv.swift ../../build/movconv/plm_frames.o \
    -o ../../build/movconv/movconv
cd ../.. && build/movconv/movconv game/app build/movies
```

This writes 18 `.avc` files (about 450 MB) to `build/movies/`, each checked frame by frame against the original. Copy
them to `ux0:data/TMNT2003/movies/`. Only the picture is converted: the sound still comes from the game's own files.
Without the converted movies the game falls back to a software decoder, which is choppy on the Vita.

## Controls

| Vita | Game |
| --- | --- |
| Left stick, D-pad | Move, menus |
| Cross | Weak attack, confirm |
| Circle | Strong attack, cancel |
| Square | Jump |
| Triangle | Shuriken |
| L | Dash |
| R | Air juggling |
| Select | Change shuriken |
| L + R | Special attack |
| Start | Pause (Press Start on the title screen) |

The game's own **Options > Controller Settings > Game Pad** page reassigns the actions; it shows the Vita buttons
next to the game's button numbers.

## Saves

The game saves to `ux0:data/TMNT2003/SaveGame.dat` (and the stage checkpoint to `TMNTCheckpoint.dat`). A
`SaveGame.dat` from the PC game works too: copy it into the same folder.

## Options

Optional settings go in `ux0:data/TMNT2003/tmnt_env.txt`, one `NAME=value` per line:

| Setting | Default | What it does |
| --- | --- | --- |
| `TMNT_FPS_LOCK` | 1 | Keeps levels at 30 FPS: the game slows down slightly in the heaviest moments instead of skipping frames. `0` lets it skip frames as on the PC. |
| `TMNT_ASPECT` | stretched | `4:3` keeps the original shape with black bars. |
| `TMNT_AUDIO` | 1 | `0` turns the sound off. |
| `TMNT_PERF` | 0 | `1` writes frame times and per-thread CPU use to `log.txt` every few seconds. |
| `TMNT_RENDER_THREAD` | 1 | `0` draws on the game thread, as in older versions. |
| `TMNT_PAR` | 2 | Helper threads for skinning and cel shading: `0` none, `1` one. |

## Reporting issues

Open an issue with:

- What you were doing, and whether it happens again.
- The log file `ux0:data/TMNT2003/log.txt` (it is rewritten on every start, so copy it before starting the game again).
- After a crash, the newest `psp2core-*.psp2dmp` file in `ux0:data/`.

## Repository layout

- `recomp/`: function discovery, translation and C generation. `recomp/xlat/` is a modified copy of the x86 → C
  translator from [repentogxm](https://github.com/0xl0cal/repentogxm).
- `rt/`: the runtime (guest memory and dispatch, Win32, COM, Direct3D 9, DirectInput, DirectSound, movies, the Mac and
  Vita platform layers) and the game-specific native code.
- `tools/`: table generators, the movie converter (`tools/movies/`), the LiveArea builder and run scripts.
- `vita/`: the Vita build (`CMakeLists.txt`, LiveArea template).
- `docs/`: inventory of the executable and the progress log (in Romanian).

## Support

If you enjoy the port, you can [buy me a coffee](https://buymeacoffee.com/tavasiberiu).

## Legal notice

This project is not affiliated with or endorsed by Konami, Mirage Studios or Nickelodeon. *Teenage Mutant Ninja Turtles*
is a trademark of its respective owners. This project does not distribute game code or data: you need your own copy of
the game.

## Credits and licenses

- [repentogxm](https://github.com/0xl0cal/repentogxm) by 0xl0cal: the x86 → C translator and the guest CPU model this
  port started from (GPL-2.0-or-later).
- [VitaSDK](https://vitasdk.org) and [vitaGL](https://github.com/Rinnegatamante/vitaGL) (LGPL-3.0): toolchain and
  renderer.
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski (MIT): MPEG-1 decoding for the movie converter
  and the fallback decoder.
- The [NFS: Most Wanted Vita port](https://github.com/germanalvarez15/nfsmw-vita): the idea of sharing loops between
  cores and of converting the movies for the hardware decoder.
