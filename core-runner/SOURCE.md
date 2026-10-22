## Core runner - `core-runner` (ships inside the app, not downloaded)

- Licence: GPL-3.0-or-later (full text: `core-runner/LICENSE`). Written by Project Light Gun; the one third-party
  file is `src/libretro.h` (MIT, RetroArch team).
- Complete source: the `core-runner/` folder of this repository (source files: dump.c, dump.h, gpu_test.c, gpu_test.h, hw_render.c, hw_render.h, input.c, input.h, libretro.h, protocol.h, runner.c).
  It is the exact source of the binary in the app: app-repo commit `afb6a35009f420eb2a1b46d7112e8d56106713db`.
- Binary in the app: `libcorerunner.so` (Android arm64 executable), SHA-256 `6456e297c5548a7da2ba486d992d14f85678475e1e52d2252023da5acd1dd442`
- Rebuild: `core-runner/build-android.ps1` (Android NDK r27 clang, arm64-v8a / android-32).
- What it does: loads one core, runs one game, talks to the app over one local connection using only the
  messages in `src/protocol.h`.
