/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
#ifndef PLG_DUMP_H
#define PLG_DUMP_H
#include <stddef.h>
#include <stdint.h>
int dump_png(const char *path, const uint8_t *pixels, int w, int h, int format);   /* format = PIX_* */
int dump_wav(const char *path, const int16_t *samples, size_t frames, int rate);    /* stereo */
#endif
