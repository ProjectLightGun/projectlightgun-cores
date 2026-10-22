# Core runner

The GPL half of Project Light Gun after the app split ([docs/app-splitting-plan.md](../docs/app-splitting-plan.md),
Part 3). A small program that loads **one** libretro emulator core and runs **one** game. The app starts a
new runner for every game and talks to it over one local connection. Everything that crosses that
connection is listed in [docs/core-runner-contract.md](../docs/core-runner-contract.md); the byte format is
[src/protocol.h](src/protocol.h).

**Licence:** GPL-3.0-or-later (it loads GPL cores in its own process). `src/libretro.h` is the libretro API
header (MIT, copyright the RetroArch team, copied unchanged from the FCEUmm source).

**Status (2026-10-11): S3 done for NES.** Runs FCEUmm + Project Light Gun on the PC and on the headset
(from `adb shell`). Not yet started by the app (S4) or used by it (S5).

## Files

| File | What it does |
|---|---|
| `src/runner.c` | Loads the core and game, answers the core's requests, runs the frame loop, the connection, the cartridge save |
| `src/input.c` | What each Quest button means in the game (was `GunInputMapper.cs` / `PadInputMapper.cs` in the app). NES only so far |
| `src/protocol.h` | The message format |
| `src/dump.c` | PNG / WAV output for `--test` |
| `tools/test_client.py` | Pretends to be the app and checks every message type |

## Build

```
powershell -File core-runner/build-windows.ps1   # -> build/core-runner.exe (MinGW-w64, C:\mingw64)
powershell -File core-runner/build-android.ps1   # -> build/android/libcorerunner.so (Unity's NDK)
```

`libcorerunner.so` is an Android **executable**, named `lib*.so` only so Android unpacks it with the app,
like `libchdman.so`.

## Run on its own (no app)

```
core-runner --test --core <fceumm_libretro.dll> --game <plg.nes> --cart-save out/plg.srm
            --option fceumm_zapper_mode=clightgun --option fceumm_zapper_tolerance=6
            --frames 600 --dump-every 150 --out out
```

Runs the frames as fast as it can (no input) and writes `frame_NNNNN.png` and `sound.wav`.

## Run as the app would

```
python -I core-runner/tools/test_client.py --runner <full path to core-runner.exe> --core <fceumm dll> --game <plg.nes>
```

Checks: HELLO + token, START → READY, pictures at the game's speed, sound at its rate, a shot at NEON SIEGE
changes the picture, PAUSE stops everything, RESUME restarts it, STOP → STOPPED, the runner exits, the
cartridge save is written.

## On the headset by hand

Push `libcorerunner.so`, `libfceumm_libretro_android.so` and the ROM to `/data/local/tmp/<dir>`,
`chmod 755` the runner and run the `--test` command there; `adb pull` the PNGs.

## Not yet

- GPU-drawn cores (SwanStation, YabaSanshiro, Flycast) need an off-screen graphics context: Part 4. The
  early test for it is in S4.
- Only the NES button meanings exist in `input.c`. The rest arrive with each system in Part 4.
- The connection is TCP on 127.0.0.1 on both platforms (the app's C# side can't reliably use an Android
  local socket under IL2CPP). It is still a single connection on the device, not shared memory.
