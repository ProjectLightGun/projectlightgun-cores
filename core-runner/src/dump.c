/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
/*
 * Project Light Gun core runner - test-mode output: pictures as PNG, sound as WAV.
 * Only used by --test (running a game on its own from the command line). Tiny on purpose:
 * the PNG is written uncompressed ("stored" deflate blocks), which every viewer reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dump.h"
#include "protocol.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
    static int done;
    if (done) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    done = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    if (len) crc = crc_update(crc, data, len);
    uint8_t tail[4];
    put_be32(tail, crc ^ 0xFFFFFFFFu);
    fwrite(tail, 1, 4, f);
}

int dump_png(const char *path, const uint8_t *pixels, int w, int h, int format)
{
    crc_init();
    /* Raw image: each row = filter byte 0 + RGB. */
    size_t row = (size_t)w * 3 + 1, raw_len = row * h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    int bottom_up = (format & PIX_BOTTOM_UP) != 0;
    format &= 0x7F;
    for (int oy = 0; oy < h; oy++) {
        uint8_t *o = raw + oy * row;
        int y = bottom_up ? h - 1 - oy : oy;
        *o++ = 0;
        for (int x = 0; x < w; x++) {
            if (format == PIX_RGB565) {
                uint16_t v = (uint16_t)(pixels[(y * w + x) * 2] | (pixels[(y * w + x) * 2 + 1] << 8));
                uint8_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                *o++ = (uint8_t)((r << 3) | (r >> 2));
                *o++ = (uint8_t)((g << 2) | (g >> 4));
                *o++ = (uint8_t)((b << 3) | (b >> 2));
            } else {
                const uint8_t *p = pixels + (y * w + x) * 4;
                *o++ = p[0]; *o++ = p[1]; *o++ = p[2];
            }
        }
    }
    /* zlib stream of stored blocks (max 65535 bytes each) + adler32. */
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *o = z;
    if (!z) { free(raw); return 0; }
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *o++ = off + n >= raw_len ? 1 : 0;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n); o += n;
    }
    put_be32(o, (b << 16) | a); o += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w); put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit RGB */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 1;
}

static void put_le32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put_le16(FILE *f, uint16_t v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

int dump_wav(const char *path, const int16_t *samples, size_t frames, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t bytes = (uint32_t)(frames * 4);
    fwrite("RIFF", 1, 4, f); put_le32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_le32(f, 16); put_le16(f, 1); put_le16(f, 2);
    put_le32(f, (uint32_t)rate); put_le32(f, (uint32_t)rate * 4); put_le16(f, 4); put_le16(f, 16);
    fwrite("data", 1, 4, f); put_le32(f, bytes);
    fwrite(samples, 4, frames, f);   /* little-endian host assumed (x86_64, arm64) */
    fclose(f);
    return 1;
}
