# Project Light Gun - emulator cores

The emulator cores used by **Project Light Gun**, a light-gun game app for Meta Quest. The app does not
include any emulator. When you first open a system in the app, it reads `manifest.json` from this
repository and downloads that system's core, checking its size and SHA-256 before using it. Each core is
versioned on its own, and an app build always downloads the exact core version it was tested with.

## Cores

| Core | Console | Licence | Version | Upstream | Licence text |
|---|---|---|---|---|---|
| SwanStation | PlayStation | GPL-3.0 | v1 | [libretro/swanstation](https://github.com/libretro/swanstation) | [LICENSE-swanstation.txt](LICENSE-swanstation.txt) |
| YabaSanshiro | Sega Saturn | GPL-2.0-or-later | v1 | [libretro/yabause/tree/yabasanshiro](https://github.com/libretro/yabause/tree/yabasanshiro) | [LICENSE-yabasanshiro.txt](LICENSE-yabasanshiro.txt) |
| Flycast | Dreamcast | GPL-2.0-or-later (GPL-3.0 as built) | v1 | [flyinghead/flycast](https://github.com/flyinghead/flycast) | [LICENSE-flycast.txt](LICENSE-flycast.txt) |
| FCEUmm | NES | GPL-2.0-or-later | v1 | [libretro/libretro-fceumm](https://github.com/libretro/libretro-fceumm) | [LICENSE-fceumm.txt](LICENSE-fceumm.txt) |
| Mesen-S | SNES | GPL-3.0-or-later | v1 | [libretro/Mesen-S](https://github.com/libretro/Mesen-S) | [LICENSE-mesen-s.txt](LICENSE-mesen-s.txt) |

All cores are built for Android arm64 (Meta Quest 3 / 3S) only. They will not run on other platforms.

## Licences and source

Each core is free software under the GNU General Public License. This repository holds, for every core:

- the compiled Android (arm64) library the app downloads (as a GitHub Release asset),
- the **complete corresponding source code** (also a Release asset), including our modifications,
- the licence text (`cores/<core>/LICENSE`, copied to `LICENSE-<core>.txt` in this folder).

See [SOURCES.md](SOURCES.md) for versions, upstream commits and checksums, and `manifest.json` for the
list the app reads. To ask for anything else, open an issue on this repository.

## Repository layout

- `manifest.json` - the list the app reads: each core's version, download address, size and SHA-256,
  plus where its source is.
- `SOURCES.md` - per core: upstream project and exact commit, which files we changed, checksums of the
  binary and the source archive, and how to rebuild it.
- `cores/<core>/` - the licence text and the build script we use for that core.
- `LICENSE-<core>.txt` - copies of the licence texts.
- **Releases** - one release per core version (tag `<core>-v<n>`), holding the library and its complete
  source archive. Older versions are kept, because older app builds still download them.

## Rebuilding a core

Download the core's source archive from its release and unpack it: it is the complete source tree with our
changes already applied. The build script in `cores/<core>/build-script.ps1` shows the exact toolchain,
settings and link steps we use (see [SOURCES.md](SOURCES.md) for details).

## Reporting problems

- A problem with an emulator itself: report it to that core's upstream project (linked above).
- A problem with how Project Light Gun uses a core, or a question about these files: open an issue on
  this repository.

Project Light Gun is not affiliated with Sony, Sega or Nintendo; all trademarks belong to their owners.