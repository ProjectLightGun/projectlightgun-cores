# The connection between the app and the core runner

Project Light Gun is two separate programs:

- **The app**: menus, the virtual screen, aiming and calibration, hand occlusion, settings, importing games.
- **The core runner** (this folder, GPL-3.0-or-later): a small program that loads ONE emulator core, runs ONE
  game and nothing else. The app starts a new runner for every game and stops it when the game ends.

**This page lists everything that passes between them. If it is not here, it does not cross.** The exact
byte format of every message is [src/protocol.h](src/protocol.h); that file is the authority if the two differ.

## Rules

1. **Plain data only.** Messages carry numbers, short text, and pictures and sound in standard formats.
   No shared code, no shared objects, no function calls across the line.
2. **One connection, nothing else.** The two programs talk over one local (loopback, `127.0.0.1`) network
   connection on the headset. The runner never reads or writes the app's files, and the app never reads
   the core's memory.
3. **The app does not speak libretro.** It talks about "a gun on this port", "a picture", "a block of
   sound". Libretro devices, callbacks and structures stay inside the runner.
4. **Either side can go away.** If the connection drops, the other side cleans up by itself. A core crash
   closes the connection and the app returns to the game library.

## How it starts and stops

1. The app starts the runner and gives it the port to connect to plus a one-off random code (token).
2. The runner connects and sends **HELLO** with that code, so the app knows it is talking to the runner it
   started. The app then sends **START**.
3. The runner loads the game and sends **READY**, or **FAILED** with a reason.
4. While playing: the app sends **INPUT**; the runner sends **PICTURE** and **SOUND**.
5. The app sends **STOP**. The runner writes the save, sends **STOPPED** and exits.

## From the app to the runner

| Message | What it carries | When |
|---|---|---|
| **START** | Core file path, game file path, BIOS folder, save folder, cartridge-save file, what is plugged into each port, the system name, and a list of settings as `name = value` text | Once, after HELLO |
| **INPUT** | The player's controller right now, plus a running number | Each app frame, when it changes |
| **PAUSE** / **RESUME** | Nothing | Menu opened or closed, headset taken off |
| **STOP** | Nothing | Leaving the game, app closing |

**INPUT** is the Quest controller after the app's own aiming and calibration. It does not say what any
button means in the game; the runner decides that.

- **Aim:** where the gun points on the picture, two numbers from 0 to 1 (left to right, top to bottom), plus
  "on screen" yes/no.
- **Buttons:** trigger, A, B, grip, thumbstick click, menu (each pressed or not).
- **Thumbstick:** two numbers from -1 to 1.

**START** names the system (`ps1`, `saturn`, `dreamcast`, `nes`, `snes`) and the device on each port
(`none`, `pad`, `zapper`, `dcgun`, `guncon`, `justifier`, `superscope`, `saturngun`). Other per-game settings
travel as text: core options (`option=key=value`), `grip_reload=on|off`, `bind_fbo=on|off`,
`readback=sync|async`, `cart_save=` (the save file for NES, SNES and the PS1 memory card).

## From the runner to the app

| Message | What it carries | When |
|---|---|---|
| **HELLO** | Runner version | On connecting |
| **READY** | Picture size, picture shape, frames per second, sound sample rate | Game loaded |
| **FAILED** | One line of text saying why | Game did not load |
| **PICTURE** | Width, height, pixel format (`RGB565` or `RGBA8`), the pixels (bottom row first), a frame number, and delay-measurement stamps | Each new frame |
| **SHAPE** | New picture shape | The game changed video mode |
| **SOUND** | A block of 16-bit stereo samples | As the game makes sound |
| **LOG** | One line of text | When the core logs something, and a once-a-second `stats:` line |
| **STOPPED** | Nothing | Save written, about to exit |

## What does not cross

- **Save files.** The runner reads and writes them itself, in the folder it was given. The app never sees
  the save data.
- **What each button does in each game.** The runner decides this (`src/input.c`).
- **Timing.** The runner runs the game on its own clock; the app shows the newest picture and plays the sound.
- **Aiming and calibration.** They stay in the app; the runner only receives the final 0-to-1 position.
- **The game's memory, the core itself and the core's callbacks.** They never leave the runner.
