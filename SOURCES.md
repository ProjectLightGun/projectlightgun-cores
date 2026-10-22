# Sources and checksums

## SwanStation (PlayStation 1) - `swanstation` v1

- Licence: GPL-3.0 (full text: `cores/swanstation/LICENSE`)
- Upstream: https://github.com/libretro/swanstation, commit `89309ccf3c5c97150777730778a820745669fc45`
- Our changes: `swanstation-patches` - 16 file(s), already applied inside the source archive
- Complete source: release `swanstation-v1` asset `swanstation-v1-source.tar.gz` (SHA-256 `909b5fb07421df602ea2551ba6509f33d6e98b16cb69d024167bbd588d06ffde`)
- Binary: release `swanstation-v1` asset `libswanstation_libretro_android.so` (SHA-256 `517a6f8bc39fa1fa5c1973b36c816a5980ca50805a1c540b22f165ba7e0c5406`)
- Rebuild: the unpacked source archive is the complete, already-patched tree. Toolchain: Windows host, Android NDK r27, CMake 3.22 + Ninja, target arm64-v8a / android-29. `cores/swanstation/build-script.ps1` is the exact script we run (configure flags, link steps); it was written to run from our private app repo, so treat it as the recipe - its patch-copy step is not needed with this archive.

## YabaSanshiro (Sega Saturn) - `yabasanshiro` v1

- Licence: GPL-2.0-or-later (full text: `cores/yabasanshiro/LICENSE`)
- Upstream: https://github.com/libretro/yabause (branch yabasanshiro), commit `f448097b69a6037246a08e9dc09eabaa420d7893`
- Our changes: `yabasanshiro-patches` - 10 file(s), already applied inside the source archive
- Complete source: release `yabasanshiro-v1` asset `yabasanshiro-v1-source.tar.gz` (SHA-256 `01f033533475f5f62e02e92064d747d79e42a741e56ca7e7c6d56c7d258996e2`)
- Binary: release `yabasanshiro-v1` asset `libyabasanshiro_libretro_android.so` (SHA-256 `968c7d20ba30fbfa8d91e64267cfe8a35f92a1ec0709e5346d9abf6cea2939e3`)
- Rebuild: the unpacked source archive is the complete, already-patched tree. Toolchain: Windows host, Android NDK r27, CMake 3.22 + Ninja, target arm64-v8a / android-29. `cores/yabasanshiro/build-script.ps1` is the exact script we run (configure flags, link steps); it was written to run from our private app repo, so treat it as the recipe - its patch-copy step is not needed with this archive.

## Flycast (Dreamcast) - `flycast` v1

- Licence: GPL-2.0-or-later (binary as a whole GPL-3.0: contains GPL-3.0 libcdio/xBRZ files) (full text: `cores/flycast/LICENSE`)
- Upstream: https://github.com/flyinghead/flycast, commit `c0f2cf4319d6c77d577599906ca0a90627a3afc8`
- Our changes: `flycast-patches` - 2 file(s), already applied inside the source archive
- Complete source: release `flycast-v1` asset `flycast-v1-source.tar.gz` (SHA-256 `01a9c43ac52629c504235c8ffb1e2ab3e3fb89c412736ebcb0e77a65fb8574f6`)
- Binary: release `flycast-v1` asset `libflycast_libretro_android.so` (SHA-256 `7f205947246574eba1b6fbd0fc75a114ddf2109ef996dfe696e436366b9d5937`)
- Rebuild: the unpacked source archive is the complete, already-patched tree. Toolchain: Windows host, Android NDK r27, CMake 3.22 + Ninja, target arm64-v8a / android-29. `cores/flycast/build-script.ps1` is the exact script we run (configure flags, link steps); it was written to run from our private app repo, so treat it as the recipe - its patch-copy step is not needed with this archive.

## FCEUmm (NES) - `fceumm` v1

- Licence: GPL-2.0-or-later (full text: `cores/fceumm/LICENSE`)
- Upstream: https://github.com/libretro/libretro-fceumm, commit `afe65ef1b4328c0bf2df05fb47a6b31a45bccf47`
- Our changes: `fceumm-patches` - 1 file(s), already applied inside the source archive
- Complete source: release `fceumm-v1` asset `fceumm-v1-source.tar.gz` (SHA-256 `89d7fe2f800adcf433342eba307c542ee297a6eb47d01d7698b41e495ff8b868`)
- Binary: release `fceumm-v1` asset `libfceumm_libretro_android.so` (SHA-256 `baa3c329d0354c785495188541121969c3af8c8f2b01b85882ed0149b7053681`)
- Rebuild: the unpacked source archive is the complete, already-patched tree. Toolchain: Windows host, Android NDK r27, CMake 3.22 + Ninja, target arm64-v8a / android-29. `cores/fceumm/build-script.ps1` is the exact script we run (configure flags, link steps); it was written to run from our private app repo, so treat it as the recipe - its patch-copy step is not needed with this archive.

## Mesen-S (SNES) - `mesen-s` v1

- Licence: GPL-3.0-or-later (full text: `cores/mesen-s/LICENSE`)
- Upstream: https://github.com/libretro/Mesen-S, commit `1d475abd174d16ecb1fb030961ff26076ab51ee6`
- Our changes: `mesen-patches` - 7 file(s), already applied inside the source archive
- Complete source: release `mesen-s-v1` asset `mesen-s-v1-source.tar.gz` (SHA-256 `802d5e73f9a230e632011f6799de54a23e90703a492ddf54769f5a6c11d7e8c3`)
- Binary: release `mesen-s-v1` asset `libmesen-s_libretro_android.so` (SHA-256 `fc0d03cee16880df87e24748ac689cfb39066e3cdc88c087e95f27533509e1a6`)
- Rebuild: the unpacked source archive is the complete, already-patched tree. Toolchain: Windows host, Android NDK r27, CMake 3.22 + Ninja, target arm64-v8a / android-29. `cores/mesen-s/build-script.ps1` is the exact script we run (configure flags, link steps); it was written to run from our private app repo, so treat it as the recipe - its patch-copy step is not needed with this archive.

## Core runner - `core-runner` (ships inside the app, not downloaded)

- Licence: GPL-3.0-or-later (full text: `core-runner/LICENSE`). Written by Project Light Gun; the one third-party
  file is `src/libretro.h` (MIT, RetroArch team).
- Complete source: the `core-runner/` folder of this repository (source files: dump.c, dump.h, gpu_test.c, gpu_test.h, hw_render.c, hw_render.h, input.c, input.h, libretro.h, protocol.h, runner.c).
  It is the exact source of the binary in the app: app-repo commit `7a818d6ea7d68e1170330232aaf168ae988a9dd2`.
- Binary in the app: `libcorerunner.so` (Android arm64 executable), SHA-256 `6456e297c5548a7da2ba486d992d14f85678475e1e52d2252023da5acd1dd442`
- Rebuild: `core-runner/build-android.ps1` (Android NDK r27 clang, arm64-v8a / android-32).
- What it does: loads one core, runs one game, talks to the app over one local connection using only the
  messages in `src/protocol.h`.
