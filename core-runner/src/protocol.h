/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
/*
 * Project Light Gun core runner - the wire format of the app <-> runner connection.
 *
 * This file is the byte-level version of docs/core-runner-contract.md. If a message isn't
 * listed here it does not cross. Keep the two in step; the app's C# reader mirrors this file.
 *
 * Every message is:   u32 length (little-endian, counts the type byte + payload)
 *                     u8  type
 *                     payload (length - 1 bytes)
 *
 * Text payloads are UTF-8, one "key=value" per line, '\n' separated. No terminating zero.
 */
#ifndef PLG_PROTOCOL_H
#define PLG_PROTOCOL_H

#define PLG_PROTOCOL_VERSION 2

/* Largest message either side will accept (a 1024x1024 RGBA picture plus header). */
#define PLG_MAX_MESSAGE (4u * 1024u * 1024u + 64u)

/* ---- app -> runner ---- */
enum {
    MSG_START  = 1,   /* text: core= game= system_dir= save_dir= cart_save= port0= port1=
                         option=<core option name>=<value> (repeatable)
                         <runner setting>=<value> (e.g. offscreen_signal=on) */
    MSG_INPUT  = 2,   /* binary, 12 bytes, see below */
    MSG_PAUSE  = 3,   /* empty */
    MSG_RESUME = 4,   /* empty */
    MSG_STOP   = 5    /* empty */
};

/* ---- runner -> app ---- */
enum {
    MSG_HELLO   = 64, /* text: runner=<version> protocol=<n> token=<token from the command line> */
    MSG_READY   = 65, /* text: width= height= aspect= fps= sample_rate= */
    MSG_FAILED  = 66, /* text: one line, why */
    MSG_PICTURE = 67, /* binary: PLG_PICTURE_HEADER bytes (see below), then the pixels,
                         rows packed with no padding, top row first unless PIX_BOTTOM_UP */
    MSG_SHAPE   = 68, /* text: aspect= */
    MSG_SOUND   = 69, /* binary: interleaved stereo int16 samples at READY's sample_rate */
    MSG_LOG     = 70, /* text: one line */
    MSG_STOPPED = 71  /* empty */
};

/* PICTURE pixel formats */
enum {
    PIX_RGB565 = 1,   /* 2 bytes a pixel, little-endian, red in the top 5 bits */
    PIX_RGBA8  = 2    /* 4 bytes a pixel: R, G, B, A (A always 255) */
};
/* Added to a format: rows are BOTTOM row first (a GPU-drawn core's picture, copied straight out of
 * the GPU in its own order; flipping it here and back again in the app would be two wasted copies). */
#define PIX_BOTTOM_UP 0x80

/*
 * PICTURE header (27 bytes, little-endian):
 *   u32 frame number
 *   u16 width, u16 height
 *   u8  format (PIX_* | PIX_BOTTOM_UP)
 *   u16 input number  the INPUT number (see below) the core had when it made this picture
 *   u64 made_us       when the runner started emulating this frame, CLOCK_MONOTONIC microseconds
 *                     (the same clock in every process on the headset, so the app can compare)
 *   u32 run_us        how long emulating it took
 *   u32 read_us       how long copying it back from the GPU took (0 for a CPU-drawn core)
 * The last four are for measuring delay (plan: Dreamcast runner test); they cost 18 bytes.
 */
#define PLG_PICTURE_HEADER 27

/*
 * INPUT payload (12 bytes, little-endian). The Quest controller after the app's own aiming and
 * calibration. Says nothing about what a button means in the game - the runner decides that.
 *
 *   u16 aim_x     0..65535 = left..right edge of the picture
 *   u16 aim_y     0..65535 = top..bottom edge of the picture
 *   u8  flags     bit 0 = aim is on the picture
 *   u8  buttons   see BTN_* below
 *   i16 stick_x   -32767..32767, right is positive
 *   i16 stick_y   -32767..32767, up is positive
 *   u16 number    counts up by one every INPUT (wraps); PICTURE says which one the core had
 */
#define PLG_INPUT_SIZE 12
#define AIM_ON_SCREEN  0x01
enum {
    BTN_TRIGGER     = 0x01,
    BTN_A           = 0x02,
    BTN_B           = 0x04,
    BTN_GRIP        = 0x08,
    BTN_STICK_CLICK = 0x10,
    BTN_MENU        = 0x20
};

#endif
