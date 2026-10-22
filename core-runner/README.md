# Core runner

A small program that loads **one** libretro emulator core and runs **one** game. Project Light Gun starts a
new runner for every game and talks to it over one local connection. Everything that crosses that connection
is listed in [CONTRACT.md](CONTRACT.md); the byte format is [src/protocol.h](src/protocol.h).

The runner supports all five systems in the app (PlayStation, Saturn, Dreamcast, NES, SNES), including
the GPU-drawn cores (it creates its own off-screen OpenGL ES context). It ships inside the app package as
`libcorerunner.so` (an Android arm64 **executable**, named `lib*.so` only so Android unpacks it with the app).
The emulator cores themselves are downloaded separately, from this repository.

**Licence:** GPL-3.0-or-later ([LICENSE](LICENSE)). `src/libretro.h` is the libretro API header (MIT,
copyright the RetroArch team, unchanged; its licence text is in the file).

## Files

| File | What it does |
|---|---|
| `src/runner.c` | Loads the core and game, answers the core's requests, runs the frame loop and the connection, writes saves |
| `src/input.c` | What each Quest button means in each game, per system and device |
| `src/hw_render.c` | The off-screen graphics context and picture read-back for GPU-drawn cores |
| `src/protocol.h` | The message format |
| `src/dump.c` | PNG / WAV output for `--test` |
| `src/gpu_test.c` | Draws fake pictures on the GPU (used to test graphics, no core needed) |
| `tools/test_client.py` | Pretends to be the app and checks every message type |

## Build

```
powershell -File core-runner/build-windows.ps1   # -> build/core-runner.exe (MinGW-w64, C:\mingw64)
powershell -File core-runner/build-android.ps1   # -> build/android/libcorerunner.so (Android NDK r27)
```

## Run on its own (no app)

```
core-runner --test --core <fceumm_libretro.dll> --game <game.nes> --cart-save out/game.srm
            --option fceumm_zapper_mode=clightgun --option fceumm_zapper_tolerance=6
            --frames 600 --dump-every 150 --out out
```

Runs the frames as fast as it can (no input) and writes `frame_NNNNN.png` and `sound.wav`.

## Run as the app would

```
python -I core-runner/tools/test_client.py --runner <full path to core-runner.exe> --core <fceumm dll> --game <game.nes>
```

Checks HELLO and the token, START to READY, pictures at the game's speed, sound at its rate, PAUSE and
RESUME, STOP to STOPPED, that the runner exits, and that the cartridge save is written.

## On the headset by hand

Push `libcorerunner.so`, a core `.so` and a game to `/data/local/tmp/<dir>`, `chmod 755` the runner and run
the `--test` command there; `adb pull` the PNGs.
