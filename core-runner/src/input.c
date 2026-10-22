/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
/*
 * Project Light Gun core runner - what each Quest button means in the game. See input.h.
 *
 * A straight copy of the app's mapping, so a game plays exactly the same in either setup:
 *   input_read          = LibretroCore.InputState (which port answers, which mapper, which pad role)
 *   read_gun            = GunInputMapper.Read
 *   read_pointer/mouse  = GunInputMapper.ReadPointer / ReadMouse (Mesen-S Super Scope + SNES Justifier)
 *   read_pad            = PadInputMapper.Read, with the NES grip tap/hold counted in emulated frames
 * The reasons behind each choice are in the comments of those C# files.
 */
#include <string.h>
#include "input.h"
#include "protocol.h"
#include "libretro.h"

/* Device ids (Libretro.cs) */
#define DEVID_PS_GUNCON       ((1u << 8) | RETRO_DEVICE_LIGHTGUN)   /* 0x104 */
#define DEVID_PS_JUSTIFIER    ((2u << 8) | RETRO_DEVICE_LIGHTGUN)   /* 0x204 */
#define DEVID_SS_GUN_US       ((2u << 8) | RETRO_DEVICE_LIGHTGUN)   /* 0x204 */
#define DEVID_MESEN_SCOPE     ((1u << 8) | RETRO_DEVICE_POINTER)    /* 0x106 */
#define DEVID_MESEN_JUSTIFIER ((2u << 8) | RETRO_DEVICE_POINTER)    /* 0x206 */
#define DEVID_NES_ZAPPER      ((1u << 8) | RETRO_DEVICE_MOUSE)      /* 0x102 */

/* GunInputMapper ids (RETRO_DEVICE_ID_LIGHTGUN_*) */
#define LG_SCREEN_X 13
#define LG_SCREEN_Y 14
#define LG_OFFSCREEN 15
#define LG_TRIGGER 2
#define LG_RELOAD 16
#define LG_AUX_A 3
#define LG_AUX_B 4
#define LG_START 6
#define LG_SELECT 7
#define LG_DPAD_UP 9
#define LG_DPAD_DOWN 10
#define LG_DPAD_LEFT 11
#define LG_DPAD_RIGHT 12

/* Half deflection = a d-pad press (GunInputMapper.StickDeadzone / PadInputMapper.StickThreshold 0.5). */
#define STICK_THRESHOLD 16384

typedef enum { ROLE_GENERIC, ROLE_NES_COMPANION, ROLE_SCOPE_COMPANION, ROLE_SATURN_COMPANION } pad_role;

static const struct { const char *name; port_device dev; } NAMES[] = {
    { "none", DEV_NONE }, { "pad", DEV_PAD }, { "zapper", DEV_ZAPPER }, { "dcgun", DEV_DC_GUN },
    { "guncon", DEV_GUNCON }, { "justifier", DEV_JUSTIFIER }, { "superscope", DEV_SUPERSCOPE },
    { "saturngun", DEV_SATURN_GUN },
};

port_device input_device_from_name(const char *name)
{
    for (unsigned i = 0; name && i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (!strcmp(name, NAMES[i].name)) return NAMES[i].dev;
    return DEV_NONE;
}

const char *input_device_name(port_device d)
{
    for (unsigned i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (NAMES[i].dev == d) return NAMES[i].name;
    return "none";
}

unsigned input_libretro_device(const input_state *in, port_device d)
{
    switch (d) {
    case DEV_PAD:        return RETRO_DEVICE_JOYPAD;
    case DEV_ZAPPER:     return DEVID_NES_ZAPPER;
    case DEV_DC_GUN:     return RETRO_DEVICE_LIGHTGUN;
    case DEV_GUNCON:     return DEVID_PS_GUNCON;
    case DEV_JUSTIFIER:  return in->snes ? DEVID_MESEN_JUSTIFIER : DEVID_PS_JUSTIFIER;
    case DEV_SUPERSCOPE: return DEVID_MESEN_SCOPE;
    case DEV_SATURN_GUN: return DEVID_SS_GUN_US;
    default:             return RETRO_DEVICE_NONE;
    }
}

static int is_gun(port_device d) { return d != DEV_NONE && d != DEV_PAD; }
static int has(const input_state *in, uint8_t b) { return (in->ctl.buttons & b) != 0; }
static int16_t aim_x(const input_state *in) { return (int16_t)((int)in->ctl.aim_x - 0x8000); }
static int16_t aim_y(const input_state *in) { return (int16_t)((int)in->ctl.aim_y - 0x8000); }

void input_advance_frame(input_state *in)
{
    grip_state *g = &in->grip;
    int down = has(in, BTN_GRIP);
    if (g->select_frames > 0) g->select_frames--;
    if (down) {
        g->held_frames++;
        if (g->held_frames >= in->hold_frames) g->start = 1;
    } else {
        /* Released before it became a hold = a tap: pulse Select long enough that a
         * once-a-frame poll can't miss it. A release after Start fired is not a tap. */
        if (g->held_frames > 0 && !g->start) g->select_frames = in->pulse_frames;
        g->held_frames = 0;
        g->start = 0;
    }
}

/* GunInputMapper.Read */
static int16_t read_gun(const input_state *in, port_device dev, unsigned id)
{
    const controller_state *c = &in->ctl;
    int dc = dev == DEV_DC_GUN;
    switch (id) {
    case LG_SCREEN_X:  return aim_x(in);
    case LG_SCREEN_Y:  return aim_y(in);
    case LG_OFFSCREEN: return (int16_t)(c->on_screen ? 0 : 1);
    case LG_RELOAD:    return 0;
    case LG_TRIGGER:   return (int16_t)has(in, BTN_TRIGGER);
    case LG_AUX_A:
        if (dev == DEV_ZAPPER || dev == DEV_SATURN_GUN) return 0;
        if (dc) return (int16_t)has(in, BTN_B);
        return (int16_t)has(in, BTN_A);
    case LG_AUX_B:
        if (dev == DEV_SUPERSCOPE) return (int16_t)has(in, BTN_GRIP);
        if (dev == DEV_ZAPPER || dev == DEV_SATURN_GUN) return 0;
        return (int16_t)has(in, BTN_B);
    case LG_START:
        if (dev == DEV_ZAPPER) return 0;
        if (dc) return (int16_t)has(in, BTN_A);
        if (dev == DEV_GUNCON) return (int16_t)(has(in, BTN_B) || (in->grip_reload && has(in, BTN_GRIP)));
        return (int16_t)has(in, BTN_B);
    case LG_SELECT:
        if (dev == DEV_SUPERSCOPE || dev == DEV_ZAPPER) return 0;
        return (int16_t)has(in, BTN_A);
    case LG_DPAD_UP:    return (int16_t)(dc && c->stick_y >  STICK_THRESHOLD);
    case LG_DPAD_DOWN:  return (int16_t)(dc && c->stick_y < -STICK_THRESHOLD);
    case LG_DPAD_LEFT:  return (int16_t)(dc && c->stick_x < -STICK_THRESHOLD);
    case LG_DPAD_RIGHT: return (int16_t)(dc && c->stick_x >  STICK_THRESHOLD);
    default:            return 0;
    }
}

/* GunInputMapper.ReadPointer / ReadMouse */
static int16_t read_pointer(const input_state *in, unsigned id)
{
    switch (id) {
    case RETRO_DEVICE_ID_POINTER_X:       return aim_x(in);
    case RETRO_DEVICE_ID_POINTER_Y:       return aim_y(in);
    case RETRO_DEVICE_ID_POINTER_PRESSED: return (int16_t)(in->ctl.on_screen ? 1 : 0);
    default:                              return 0;
    }
}

static int16_t read_mouse(const input_state *in, unsigned id)
{
    switch (id) {
    case RETRO_DEVICE_ID_MOUSE_LEFT:   return (int16_t)has(in, BTN_TRIGGER);   /* fire */
    case RETRO_DEVICE_ID_MOUSE_RIGHT:  return (int16_t)has(in, BTN_A);         /* cursor */
    case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (int16_t)has(in, BTN_B);         /* turbo */
    default:                           return 0;
    }
}

/* PadInputMapper.Read */
static int16_t read_pad(const input_state *in, unsigned id, int gun_port, pad_role role)
{
    const controller_state *c = &in->ctl;
    if (role == ROLE_SCOPE_COMPANION) return 0;
    if (role == ROLE_SATURN_COMPANION)
        return (int16_t)(id == RETRO_DEVICE_ID_JOYPAD_START && (has(in, BTN_MENU) || has(in, BTN_GRIP)));
    int nes = role == ROLE_NES_COMPANION;
    if (nes && in->offscreen_signal && (id == RETRO_DEVICE_ID_JOYPAD_UP || id == RETRO_DEVICE_ID_JOYPAD_DOWN)
        && has(in, BTN_TRIGGER) && !c->on_screen)
        return 1;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:    return (int16_t)(c->stick_y >  STICK_THRESHOLD);
    case RETRO_DEVICE_ID_JOYPAD_DOWN:  return (int16_t)(c->stick_y < -STICK_THRESHOLD);
    case RETRO_DEVICE_ID_JOYPAD_LEFT:  return (int16_t)(c->stick_x < -STICK_THRESHOLD);
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (int16_t)(c->stick_x >  STICK_THRESHOLD);
    case RETRO_DEVICE_ID_JOYPAD_B:
        if (nes) return (int16_t)has(in, BTN_B);
        return (int16_t)(has(in, BTN_A) || (has(in, BTN_TRIGGER) && gun_port));
    case RETRO_DEVICE_ID_JOYPAD_A:
        if (nes) return (int16_t)has(in, BTN_A);
        return (int16_t)has(in, BTN_B);
    case RETRO_DEVICE_ID_JOYPAD_SELECT:
        return (int16_t)(nes && in->grip.select_frames > 0);
    case RETRO_DEVICE_ID_JOYPAD_START:
        if (nes) return (int16_t)(in->grip.start || has(in, BTN_MENU));
        return (int16_t)(has(in, BTN_MENU) || has(in, BTN_GRIP));
    default:
        return 0;
    }
}

/* LibretroCore.InputState */
int16_t input_read(const input_state *in, unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port >= 2 || in->port[port] == DEV_NONE) return 0;   /* no phantom second player */
    port_device p0 = in->port[0], p1 = in->port[1];
    port_device this_dev = in->port[port], other_dev = in->port[port ^ 1];

    if ((device & 0xFF) == RETRO_DEVICE_LIGHTGUN)   /* base id or any subclass */
        return read_gun(in, this_dev, id);

    if (device == RETRO_DEVICE_JOYPAD) {
        int gun_port = port == 0 && is_gun(p0);
        pad_role role = (p0 == DEV_ZAPPER || p1 == DEV_ZAPPER) ? ROLE_NES_COMPANION
                      : (this_dev == DEV_PAD && other_dev == DEV_SUPERSCOPE) ? ROLE_SCOPE_COMPANION
                      : (this_dev == DEV_PAD && other_dev == DEV_SATURN_GUN) ? ROLE_SATURN_COMPANION
                      : ROLE_GENERIC;
        return read_pad(in, id, gun_port, role);
    }

    /* Mesen-S reads the Super Scope / SNES Justifier through POINTER (aim) and MOUSE (buttons) on
     * port 0, whichever port the gun is on - so answer for the active gun, ignoring the port. */
    if (device == RETRO_DEVICE_POINTER || device == RETRO_DEVICE_MOUSE) {
        port_device scope = is_gun(p0) ? p0 : p1;
        if (scope == DEV_SUPERSCOPE || (in->snes && scope == DEV_JUSTIFIER))
            return device == RETRO_DEVICE_POINTER ? read_pointer(in, id) : read_mouse(in, id);
    }
    return 0;
}
