/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
/*
 * Project Light Gun core runner - what each Quest button means in the game.
 *
 * The app only ever says "trigger pressed, aiming here" (protocol.h, MSG_INPUT). This file turns
 * that into the presses each emulated light gun and pad reports to the core. It is the runner-side
 * copy of the app's GunInputMapper.cs + PadInputMapper.cs + LibretroCore.InputState routing: when
 * those change, change this file the same way (and the other way round).
 */
#ifndef PLG_INPUT_H
#define PLG_INPUT_H

#include <stdint.h>

/* What is plugged into a port. Plain words in START. */
typedef enum {
    DEV_NONE = 0,
    DEV_PAD,          /* "pad"        - a standard pad */
    DEV_ZAPPER,       /* "zapper"     - NES Zapper (FCEUmm) */
    DEV_DC_GUN,       /* "dcgun"      - Dreamcast light gun (Flycast) */
    DEV_GUNCON,       /* "guncon"     - PS1 Namco GunCon (SwanStation) */
    DEV_JUSTIFIER,    /* "justifier"  - Konami Justifier: PS1 (SwanStation) or SNES (our Mesen-S) */
    DEV_SUPERSCOPE,   /* "superscope" - SNES Super Scope (Mesen-S, pointer-based) */
    DEV_SATURN_GUN    /* "saturngun"  - Saturn Virtua Gun / Stunner (YabaSanshiro) */
} port_device;

/* The controller as last sent by the app. */
typedef struct {
    uint16_t aim_x, aim_y;   /* 0..65535 across the picture (the app's clamped aim + 0x8000) */
    int      on_screen;
    uint8_t  buttons;        /* BTN_* from protocol.h */
    int16_t  stick_x, stick_y;
    uint16_t number;         /* the INPUT's number (protocol.h), for the delay measurement */
} controller_state;

/* NES pad grip: a tap = Select, a hold = Start. Counted in emulated frames, on the one thread
 * that runs the core, so a press can never be missed (the app's old wall-clock version could). */
typedef struct {
    int held_frames;      /* frames the grip has been down, 0 = up */
    int select_frames;    /* frames of Select pulse still to deliver */
    int start;            /* Start is being held */
} grip_state;

typedef struct {
    port_device port[2];
    int snes;               /* system is SNES: a Justifier is Mesen's pointer Justifier, not the PS1 one */
    int offscreen_signal;   /* our own NES game: trigger while pointing away = pad Up+Down */
    int grip_reload;        /* PS1 GunCon: grip also presses the gun's A (Time Crisis cover) */
    int hold_frames;        /* grip held this many frames = Start */
    int pulse_frames;       /* a tap pulses Select for this many frames */
    controller_state ctl;   /* latest INPUT */
    grip_state grip;
} input_state;

port_device input_device_from_name(const char *name);
const char *input_device_name(port_device d);
/* The id handed to retro_set_controller_port_device. */
unsigned input_libretro_device(const input_state *in, port_device d);

/* Call once per emulated frame, before retro_run. */
void input_advance_frame(input_state *in);

/* The libretro input_state callback's answer. */
int16_t input_read(const input_state *in, unsigned port, unsigned device, unsigned index, unsigned id);

#endif
