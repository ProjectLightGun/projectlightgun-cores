/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
/*
 * Project Light Gun core runner.
 *
 * A small program that loads ONE libretro emulator core, runs ONE game and nothing else. The app
 * starts a new runner for every game and talks to it over one local connection, using only the
 * messages in protocol.h (= docs/core-runner-contract.md). Licensed GPL-3.0-or-later (see README).
 *
 * Two ways to run it:
 *   core-runner --connect 127.0.0.1:<port> --token <token>
 *       The app's way. Connects, says HELLO, waits for START, plays until STOP or the
 *       connection drops.
 *   core-runner --test --core <file> --game <file> [options]
 *       On its own from a command line, no app: runs N frames as fast as it can and writes
 *       pictures (PNG) and the sound (WAV) to a folder. See usage() below.
 *
 * Everything runs on one thread: read messages, run one frame, send its picture and sound,
 * wait for the next frame. The game runs on the runner's own clock.
 */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define close_sock closesocket
#else
#  include <dlfcn.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#  include <dirent.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define close_sock close
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libretro.h"
#include "protocol.h"
#include "input.h"
#include "dump.h"
#include "gpu_test.h"
#include "hw_render.h"

#define RUNNER_VERSION   "0.1"
#define MAX_OPTIONS      64
#define SAVE_CHECK_SECS  2.0

/* ============================== small platform layer ============================== */

/* CLOCK_MONOTONIC in microseconds: the same clock the app reads, so stamps can be compared across
 * the two processes (Android). 0 on Windows (no measuring there). */
static uint64_t now_us(void)
{
#ifdef _WIN32
    return 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static double now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void *lib_open(const char *path)
{
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *lib_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static int replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0775);
#endif
}

/* ============================== state ============================== */

typedef struct { char key[96]; char value[160]; } core_option;

static struct {
    /* START */
    char core[1024], game[1024], system_dir[1024], save_dir[1024], cart_save[1024];
    core_option options[MAX_OPTIONS];
    int option_count;

    /* the core */
    void *lib;
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_get_system_info)(struct retro_system_info *);
    void (*retro_get_system_av_info)(struct retro_system_av_info *);
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_unload_game)(void);
    void (*retro_run)(void);
    void *(*retro_get_memory_data)(unsigned);
    size_t (*retro_get_memory_size)(unsigned);
    int loaded;

    enum retro_pixel_format core_format;
    int options_dirty;          /* report "options changed" once after load (see env GET_VARIABLE_UPDATE) */
    double fps, sample_rate;
    unsigned base_w, base_h;
    float aspect;
    int shape_changed;

    /* picture of the latest frame, top row first (bottom first if pic_format has PIX_BOTTOM_UP) */
    uint8_t *picture;
    size_t picture_cap;
    int pic_w, pic_h, pic_format, pic_new;
    uint32_t frame_number;

    /* GPU-drawn core (SET_HW_RENDER: Dreamcast) */
    int hw_core, hw_depth, hw_stencil, readback_async;
    /* Bind our framebuffer before each frame (the app's GLHwContext.BindForRender). Off for Saturn:
     * YabaSanshiro binds and sets up its own framebuffer and viewport, and a frontend doing it behind
     * its back desyncs its GL state cache (LibretroCore.cs, "coreManagesOwnBinding"). */
    int bind_fbo;
    retro_hw_context_reset_t hw_reset, hw_destroy_cb;
    unsigned max_w, max_h;
    uint32_t read_us_last;

    /* delay measurement, stamped on every PICTURE (protocol.h) */
    uint16_t run_input_number;      /* the INPUT number the core had for this frame */
    uint64_t run_made_us;           /* when this frame's retro_run started */
    uint32_t run_us_last;
    uint16_t prev_input_number;     /* async readback hands back the PREVIOUS frame: its stamps */
    uint64_t prev_made_us;
    uint32_t prev_run_us;
    uint16_t pic_input_number;      /* stamps of the picture about to be sent */
    uint64_t pic_made_us;
    uint32_t pic_run_us;

    /* once-a-second stats line */
    double stats_t0;
    int stats_frames, stats_late, stats_pictures, stats_early;

    /* Start a frame early (by up to early_s) when fresh controller input has just arrived, so the core
     * gets the newest aim and trigger instead of an INPUT up to an app frame old. Long-run speed is
     * unchanged: the schedule (next += time advanced) stays where it was. */
    double early_s;
    int input_fresh;
    double stats_run_sum, stats_run_max, stats_read_sum, stats_read_max, stats_send_sum, stats_send_max;
    double stats_sound_frames;
    uint64_t stats_cpu_ns;

    /* sound made during the current frame */
    int16_t *sound;
    size_t sound_frames, sound_cap;

    input_state input;

    /* cartridge save */
    uint8_t *save_on_disk;
    size_t save_size;
    double save_timer;

    /* connection (NULL socket in --test) */
    sock_t sock;
    int connected;
    uint8_t *rx;
    size_t rx_len, rx_cap;
    int paused, stopping;

    /* GPU test (plan S4): START gpu_test=on runs gpu_test.c instead of a core */
    int gpu_test, gpu_width, gpu_height, gpu_load;

    /* --test */
    int test_mode;
    int16_t *test_sound;
    size_t test_sound_frames, test_sound_cap;
} R;

/* ============================== messages ============================== */

static int send_all(const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int n = send(R.sock, p, (int)(len > 1 << 20 ? 1 << 20 : len), 0);
        if (n <= 0) { R.connected = 0; return 0; }
        p += n; len -= (size_t)n;
    }
    return 1;
}

static int send_msg2(uint8_t type, const void *a, size_t alen, const void *b, size_t blen)
{
    if (!R.connected) return 0;
    uint8_t hdr[5];
    uint32_t len = (uint32_t)(1 + alen + blen);
    hdr[0] = (uint8_t)len; hdr[1] = (uint8_t)(len >> 8); hdr[2] = (uint8_t)(len >> 16); hdr[3] = (uint8_t)(len >> 24);
    hdr[4] = type;
    return send_all(hdr, 5) && (!alen || send_all(a, alen)) && (!blen || send_all(b, blen));
}

static int send_msg(uint8_t type, const void *data, size_t len) { return send_msg2(type, data, len, NULL, 0); }

static int send_text(uint8_t type, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    return send_msg(type, buf, (size_t)n);
}

/* Text for a person: to stderr always, and to the app as LOG when connected. */
static void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    fprintf(stderr, "[runner] %s\n", buf);
    if (R.connected) send_msg(MSG_LOG, buf, n);
}

/* ============================== core callbacks ============================== */

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_WARN && !R.test_mode) return;   /* keep only what's worth keeping */
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say("core: %s", buf);
}

static const char *option_value(const char *key)
{
    for (int i = 0; i < R.option_count; i++)
        if (!strcmp(R.options[i].key, key)) return R.options[i].value;
    return NULL;
}

static void set_geometry(const struct retro_game_geometry *g)
{
    float a = g->aspect_ratio > 0.1f ? g->aspect_ratio
            : (g->base_height ? (float)g->base_width / (float)g->base_height : R.aspect);
    if (a != R.aspect) { R.aspect = a; R.shape_changed = 1; }
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data)
{
    switch (cmd & 0xFFFF) {   /* strip the EXPERIMENTAL/PRIVATE flags */
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data) *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (data) set_geometry((const struct retro_game_geometry *)data);
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (data) {
            const struct retro_system_av_info *av = data;
            set_geometry(&av->geometry);
            if (av->timing.fps > 1) R.fps = av->timing.fps;
            if (av->timing.sample_rate > 1) R.sample_rate = av->timing.sample_rate;
        }
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        if (data) R.core_format = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        if (!data) return false;
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (data) *(const char **)data = R.system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        if (data) *(const char **)data = R.save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        /* Every option is set before load, so there is never a live change. But some cores run
         * real setup only when told "options changed", so say yes exactly once after a load
         * (same as the app did, see LibretroCore.cs). */
        if (data) *(bool *)data = R.options_dirty ? true : false;
        R.options_dirty = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!v || !v->key) return false;
        const char *val = option_value(v->key);
        if (!val) return false;   /* unset = the core's own default */
        v->value = val;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        /* Same answer as the app: OpenGL ES is the only GPU API we give a core. */
        if (data) *(unsigned *)data = RETRO_HW_CONTEXT_OPENGLES_VERSION;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT & 0xFFFF:
        return false;   /* as the app (LibretroCore.cs: saying yes hung Dolphin) */
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        struct retro_hw_render_callback *hw = data;
        if (!hw) return false;
        if (hw->context_type != RETRO_HW_CONTEXT_OPENGL && hw->context_type != RETRO_HW_CONTEXT_OPENGLES2 &&
            hw->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && hw->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
            hw->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) {
            say("core asked for GPU API %d; only OpenGL ES is offered", (int)hw->context_type);
            return false;
        }
        char err[256];
        if (!R.hw_core && !hw_create(err, sizeof err)) { say("%s", err); return false; }
        hw->get_current_framebuffer = (retro_hw_get_current_framebuffer_t)hw_framebuffer;
        hw->get_proc_address = (retro_hw_get_proc_address_t)hw_get_proc_address;
        R.hw_reset = hw->context_reset;
        R.hw_destroy_cb = hw->context_destroy;
        R.hw_depth = hw->depth; R.hw_stencil = hw->stencil;
        R.hw_core = 1;
        say("GPU context: type %d v%u.%u depth %d stencil %d bottom-left %d", (int)hw->context_type,
            hw->version_major, hw->version_minor, hw->depth, hw->stencil, hw->bottom_left_origin);
        return true;
    }
    default:
        return false;
    }
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return;   /* duplicated frame: keep the last picture */
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        /* GPU-drawn: the picture is in our framebuffer. Copy it out in GL's own row order (bottom
         * first) and say so in the format - the app used it in exactly that form in-process. */
        size_t need = (size_t)w * h * 4;
        if (need > R.picture_cap) {
            uint8_t *p = realloc(R.picture, need);
            if (!p) return;
            R.picture = p; R.picture_cap = need;
        }
        uint64_t t0 = now_us();
        int got = hw_read(R.picture, (int)w, (int)h, R.readback_async);
        R.read_us_last = (uint32_t)(now_us() - t0);
        if (!got) return;
        R.pic_w = (int)w; R.pic_h = (int)h; R.pic_format = PIX_RGBA8 | PIX_BOTTOM_UP;
        R.pic_new = 1;
        R.frame_number++;
        /* async: this is the previous frame's picture, so it carries the previous frame's stamps */
        R.pic_input_number = R.readback_async ? R.prev_input_number : R.run_input_number;
        R.pic_made_us = R.readback_async ? R.prev_made_us : R.run_made_us;
        R.pic_run_us = R.readback_async ? R.prev_run_us : 0;   /* sync: filled in after retro_run */
        return;
    }
    int out_format = R.core_format == RETRO_PIXEL_FORMAT_XRGB8888 ? PIX_RGBA8 : PIX_RGB565;
    size_t bpp = out_format == PIX_RGBA8 ? 4 : 2, need = (size_t)w * h * bpp;
    if (need > R.picture_cap) {
        uint8_t *p = realloc(R.picture, need);
        if (!p) return;
        R.picture = p; R.picture_cap = need;
    }
    /* Rows go out BOTTOM first (PIX_BOTTOM_UP), the order the app's screen texture uses for every
     * core - so the app takes CPU- and GPU-drawn pictures the same way. */
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)data + y * pitch;
        uint8_t *dst = R.picture + (size_t)(h - 1 - y) * w * bpp;
        if (R.core_format == RETRO_PIXEL_FORMAT_RGB565) {
            memcpy(dst, src, (size_t)w * 2);
        } else if (R.core_format == RETRO_PIXEL_FORMAT_0RGB1555) {
            for (unsigned x = 0; x < w; x++) {
                uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
                uint16_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                uint16_t o = (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
                dst[x * 2] = (uint8_t)o; dst[x * 2 + 1] = (uint8_t)(o >> 8);
            }
        } else {   /* XRGB8888 (in memory: B, G, R, X) -> R, G, B, A */
            for (unsigned x = 0; x < w; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
    }
    R.pic_w = (int)w; R.pic_h = (int)h; R.pic_format = out_format | PIX_BOTTOM_UP;
    R.pic_new = 1;
    R.frame_number++;
    R.pic_input_number = R.run_input_number;
    R.pic_made_us = R.run_made_us;
    R.pic_run_us = 0;
}

static void add_sound(int16_t **buf, size_t *frames, size_t *cap, const int16_t *data, size_t n)
{
    if (*frames + n > *cap) {
        size_t c = (*cap ? *cap * 2 : 4096);
        while (c < *frames + n) c *= 2;
        int16_t *p = realloc(*buf, c * 4);
        if (!p) return;
        *buf = p; *cap = c;
    }
    memcpy(*buf + *frames * 2, data, n * 4);
    *frames += n;
}

static size_t RETRO_CALLCONV audio_batch(const int16_t *data, size_t frames)
{
    add_sound(&R.sound, &R.sound_frames, &R.sound_cap, data, frames);
    return frames;
}

static void RETRO_CALLCONV audio_sample(int16_t left, int16_t right)
{
    int16_t s[2] = { left, right };
    add_sound(&R.sound, &R.sound_frames, &R.sound_cap, s, 1);
}

static void RETRO_CALLCONV input_poll(void) {}

static int16_t RETRO_CALLCONV input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return input_read(&R.input, port, device, index, id);
}

/* ============================== cartridge save ============================== */
/* The runner owns the battery save (NES/SNES carts): read it in after load, write it out a
 * couple of seconds after the game changes it, and on PAUSE and STOP. Same file and format as
 * the app's old CartSaveRam.cs, so existing saves carry over. */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

static void save_load(void)
{
    if (!R.cart_save[0]) return;
    size_t size = R.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    uint8_t *mem = R.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!size || !mem) return;
    R.save_size = size;
    R.save_on_disk = malloc(size);
    if (!R.save_on_disk) { R.save_size = 0; return; }

    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", R.cart_save);
    size_t len = 0;
    uint8_t *data = read_file(R.cart_save, &len);
    if (!data) data = read_file(tmp, &len);   /* a swap that was interrupted */
    if (data) {
        memcpy(mem, data, len < size ? len : size);
        free(data);
        say("save: loaded %zu bytes from %s", len, R.cart_save);
    } else {
        say("save: none yet for %s (%zu bytes)", R.cart_save, size);
    }
    memcpy(R.save_on_disk, mem, size);
}

static void save_flush(void)
{
    if (!R.save_size) return;
    uint8_t *mem = R.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !memcmp(mem, R.save_on_disk, R.save_size)) return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", R.cart_save);
    /* Make the folder (and any missing parents) the first time. */
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/' || *s == '\\') { char c = *s; *s = 0; make_dir(tmp); *s = c; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { say("save: can't write %s", tmp); return; }
    size_t n = fwrite(mem, 1, R.save_size, f);
    if (fclose(f) != 0 || n != R.save_size || replace_file(tmp, R.cart_save) != 0) {
        say("save: failed writing %s", R.cart_save);
        return;
    }
    memcpy(R.save_on_disk, mem, R.save_size);
    say("save: wrote %zu bytes to %s", R.save_size, R.cart_save);
}

/* ============================== loading the core and game ============================== */

#define LOAD(name) do { *(void **)&R.name = lib_sym(R.lib, #name); \
    if (!R.name) { snprintf(err, errlen, "core is missing " #name); return 0; } } while (0)

static int load_core_and_game(char *err, size_t errlen)
{
    R.lib = lib_open(R.core);
    if (!R.lib) { snprintf(err, errlen, "could not load core %s", R.core); return 0; }
    LOAD(retro_init); LOAD(retro_deinit); LOAD(retro_set_environment); LOAD(retro_set_video_refresh);
    LOAD(retro_set_audio_sample); LOAD(retro_set_audio_sample_batch); LOAD(retro_set_input_poll);
    LOAD(retro_set_input_state); LOAD(retro_get_system_info); LOAD(retro_get_system_av_info);
    LOAD(retro_set_controller_port_device); LOAD(retro_load_game); LOAD(retro_unload_game);
    LOAD(retro_run); LOAD(retro_get_memory_data); LOAD(retro_get_memory_size);

    R.core_format = RETRO_PIXEL_FORMAT_0RGB1555;   /* libretro's default until the core says otherwise */
    R.retro_set_environment(environment);
    R.retro_init();
    R.retro_set_video_refresh(video_refresh);
    R.retro_set_audio_sample(audio_sample);
    R.retro_set_audio_sample_batch(audio_batch);
    R.retro_set_input_poll(input_poll);
    R.retro_set_input_state(input_state_cb);

    struct retro_system_info si;
    memset(&si, 0, sizeof si);
    R.retro_get_system_info(&si);
    say("core: %s %s", si.library_name ? si.library_name : "?", si.library_version ? si.library_version : "");

    struct retro_game_info gi;
    memset(&gi, 0, sizeof gi);
    gi.path = R.game;   /* path only, as the app did: the core reads the file itself */
    R.options_dirty = 1;
    if (!R.retro_load_game(&gi)) {
        snprintf(err, errlen, "the core could not load %s", R.game);
        R.retro_deinit();
        return 0;
    }
    R.loaded = 1;

    struct retro_system_av_info av;
    memset(&av, 0, sizeof av);
    R.retro_get_system_av_info(&av);
    R.fps = av.timing.fps > 1 ? av.timing.fps : 60.0;
    R.sample_rate = av.timing.sample_rate > 1 ? av.timing.sample_rate : 44100.0;
    R.base_w = av.geometry.base_width; R.base_h = av.geometry.base_height;
    R.max_w = av.geometry.max_width; R.max_h = av.geometry.max_height;
    R.aspect = 0;
    set_geometry(&av.geometry);
    R.shape_changed = 0;

    /* GPU core: now the largest picture is known, make the framebuffer and let the core build its
     * GPU resources (the app did this at the same point, LibretroCore.LoadCore). */
    if (R.hw_core) {
        hw_make_current();
        if (!hw_alloc_target((int)(R.max_w ? R.max_w : R.base_w), (int)(R.max_h ? R.max_h : R.base_h),
                             R.hw_depth, R.hw_stencil, err, errlen)) return 0;
        if (R.hw_reset) R.hw_reset();
        say("GPU framebuffer %ux%u, readback %s", R.max_w, R.max_h,
            R.readback_async ? "async (one frame late, no waiting)" : "sync (waits for the GPU)");
    }

    for (unsigned p = 0; p < 2; p++)
        R.retro_set_controller_port_device(p, input_libretro_device(&R.input, R.input.port[p]));
    R.input.hold_frames = (int)(0.4 * R.fps + 0.5);    /* grip held 0.4 s = Start */
    R.input.pulse_frames = (int)(0.1 * R.fps + 0.5);   /* a tap pulses Select for 0.1 s */
    if (R.input.pulse_frames < 2) R.input.pulse_frames = 2;

    save_load();
    say("loaded %s: %ux%u, %.3f fps, %.0f Hz, ports %s/%s", R.game, av.geometry.base_width,
        av.geometry.base_height, R.fps, R.sample_rate, input_device_name(R.input.port[0]),
        input_device_name(R.input.port[1]));
    return 1;
}

static void unload(void)
{
    if (!R.loaded) return;
    save_flush();
    /* GPU core: context_destroy with the context current, BEFORE unload (libretro order; the app
     * learned this the hard way with SwanStation). */
    double t0 = now_seconds();
    if (R.hw_core) { hw_make_current(); if (R.hw_destroy_cb) R.hw_destroy_cb(); }
    double t1 = now_seconds();
    R.retro_unload_game();
    double t2 = now_seconds();
    R.retro_deinit();
    double t3 = now_seconds();
    if (R.hw_core) hw_destroy();
    say("stop: context_destroy %.0f ms, unload %.0f ms, deinit %.0f ms, GPU %.0f ms",
        (t1 - t0) * 1000, (t2 - t1) * 1000, (t3 - t2) * 1000, (now_seconds() - t3) * 1000);
    R.loaded = 0;
}

/* ============================== START / settings ============================== */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void add_option(const char *key, const char *value)
{
    for (int i = 0; i < R.option_count; i++)
        if (!strcmp(R.options[i].key, key)) { copy_str(R.options[i].value, sizeof R.options[i].value, value); return; }
    if (R.option_count >= MAX_OPTIONS) { say("too many core options, ignoring %s", key); return; }
    core_option *o = &R.options[R.option_count++];
    copy_str(o->key, sizeof o->key, key);
    copy_str(o->value, sizeof o->value, value);
}

/* One "key=value" setting, from START or from the command line. Returns 0 if unknown. */
static int apply_setting(const char *key, const char *value)
{
    if (!strcmp(key, "core")) copy_str(R.core, sizeof R.core, value);
    else if (!strcmp(key, "game")) copy_str(R.game, sizeof R.game, value);
    else if (!strcmp(key, "system_dir")) copy_str(R.system_dir, sizeof R.system_dir, value);
    else if (!strcmp(key, "save_dir")) copy_str(R.save_dir, sizeof R.save_dir, value);
    else if (!strcmp(key, "cart_save")) copy_str(R.cart_save, sizeof R.cart_save, value);
    else if (!strcmp(key, "port0")) R.input.port[0] = input_device_from_name(value);
    else if (!strcmp(key, "port1")) R.input.port[1] = input_device_from_name(value);
    else if (!strcmp(key, "offscreen_signal")) R.input.offscreen_signal = !strcmp(value, "on");
    else if (!strcmp(key, "grip_reload")) R.input.grip_reload = !strcmp(value, "on");
    else if (!strcmp(key, "system")) R.input.snes = !strcmp(value, "snes");
    else if (!strcmp(key, "bind_fbo")) R.bind_fbo = strcmp(value, "off") != 0;
    else if (!strcmp(key, "readback")) R.readback_async = !strcmp(value, "async");
    else if (!strcmp(key, "early_ms")) R.early_s = atof(value) / 1000.0;
    else if (!strcmp(key, "gpu_test")) R.gpu_test = !strcmp(value, "on");
    else if (!strcmp(key, "gpu_width")) R.gpu_width = atoi(value);
    else if (!strcmp(key, "gpu_height")) R.gpu_height = atoi(value);
    else if (!strcmp(key, "gpu_load")) R.gpu_load = atoi(value);
    else if (!strcmp(key, "option")) {
        const char *eq = strchr(value, '=');
        if (!eq) return 0;
        char k[96];
        size_t n = (size_t)(eq - value);
        if (n >= sizeof k) n = sizeof k - 1;
        memcpy(k, value, n); k[n] = 0;
        add_option(k, eq + 1);
    }
    else return 0;
    return 1;
}

static void apply_start(char *text)
{
    char *line = text;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            if (!apply_setting(line, eq + 1)) say("START: ignoring unknown setting '%s'", line);
        }
        line = next;
    }
}

/* ============================== one frame ============================== */

static void put_u16(uint8_t *b, unsigned v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *b, uint32_t v) { put_u16(b, v & 0xFFFF); put_u16(b + 2, v >> 16); }
static void put_u64(uint8_t *b, uint64_t v) { put_u32(b, (uint32_t)v); put_u32(b + 4, (uint32_t)(v >> 32)); }

static void send_picture(uint32_t number, int w, int h, int format, uint16_t input_number, uint64_t made_us,
                         uint32_t run_us, uint32_t read_us, const uint8_t *px)
{
    uint8_t hdr[PLG_PICTURE_HEADER];
    put_u32(hdr, number);
    put_u16(hdr + 4, (unsigned)w); put_u16(hdr + 6, (unsigned)h);
    hdr[8] = (uint8_t)format;
    put_u16(hdr + 9, input_number);
    put_u64(hdr + 11, made_us);
    put_u32(hdr + 19, run_us);
    put_u32(hdr + 23, read_us);
    size_t bytes = (size_t)w * h * ((format & 0x7F) == PIX_RGBA8 ? 4 : 2);
    send_msg2(MSG_PICTURE, hdr, sizeof hdr, px, bytes);
}

/* CPU time used by the whole runner process so far, in nanoseconds - every thread, because a GPU
 * core like Flycast runs its emulation on threads of its own. Android only; 0 elsewhere.
 * schedstat's first field is nanoseconds on the CPU (the same source the app's AppProfiler uses). */
static uint64_t process_cpu_ns(void)
{
#ifdef __ANDROID__
    uint64_t total = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    struct dirent *e;
    char path[96];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long long ns = 0;
        if (fscanf(f, "%llu", &ns) == 1) total += ns;
        fclose(f);
    }
    closedir(d);
    return total;
#else
    return 0;
#endif
}

/* Run one frame and send what it made. Returns how much GAME time it advanced, in seconds. */
static double run_frame(void)
{
    input_advance_frame(&R.input);
    R.sound_frames = 0;
    if (R.hw_core) { hw_make_current(); if (R.bind_fbo) hw_bind_for_render(); }
    R.prev_input_number = R.run_input_number;
    R.prev_made_us = R.run_made_us;
    R.prev_run_us = R.run_us_last;
    R.run_input_number = R.input.ctl.number;
    R.run_made_us = now_us();
    R.read_us_last = 0;
    double t_run = now_seconds();
    R.retro_run();
    double run_ms = (now_seconds() - t_run) * 1000.0;
    R.run_us_last = (uint32_t)(run_ms * 1000.0);
    if (R.pic_new && !(R.hw_core && R.readback_async)) R.pic_run_us = R.run_us_last;

    if (R.shape_changed) { R.shape_changed = 0; send_text(MSG_SHAPE, "aspect=%.6f", R.aspect); }
    double send_ms = 0;
    if (R.pic_new && R.connected) {
        double t0 = now_seconds();
        send_picture(R.frame_number, R.pic_w, R.pic_h, R.pic_format, R.pic_input_number, R.pic_made_us,
                     R.pic_run_us, R.read_us_last, R.picture);
        send_ms = (now_seconds() - t0) * 1000.0;
        R.stats_pictures++;
    }
    R.pic_new = 0;
    if (R.sound_frames) {
        if (R.connected) send_msg(MSG_SOUND, R.sound, R.sound_frames * 4);
        if (R.test_mode) add_sound(&R.test_sound, &R.test_sound_frames, &R.test_sound_cap, R.sound, R.sound_frames);
    }

    double read_ms = R.read_us_last / 1000.0;
    run_ms -= read_ms;   /* report emulating and copying-back separately */
    R.stats_frames++;
    R.stats_run_sum += run_ms;   if (run_ms > R.stats_run_max) R.stats_run_max = run_ms;
    R.stats_read_sum += read_ms; if (read_ms > R.stats_read_max) R.stats_read_max = read_ms;
    R.stats_send_sum += send_ms; if (send_ms > R.stats_send_max) R.stats_send_max = send_ms;
    R.stats_sound_frames += (double)R.sound_frames;

    R.save_timer += 1.0 / R.fps;
    if (R.save_timer >= SAVE_CHECK_SECS) { R.save_timer = 0; save_flush(); fflush(NULL); }

    /* Sound is generated from EMULATED time, so frames of sound made / sample rate is exactly how far
     * the game advanced. Flycast does not always advance one frame per call (the app's pump in
     * LibretroCore.cs has the full story), so pace on this, not on the number of calls. Falls back to
     * one frame when the core made no sound. */
    double period = 1.0 / R.fps, advanced = period;
    if (R.sound_frames && R.sample_rate > 0) advanced = R.sound_frames / R.sample_rate;
    return advanced < period * 3 ? advanced : period * 3;
}

/* Once a second: what the runner did, as a LOG line the app picks out ("stats: ..."). */
static void stats_tick(double t)
{
    if (R.stats_t0 == 0) { R.stats_t0 = t; R.stats_cpu_ns = process_cpu_ns(); return; }
    double dt = t - R.stats_t0;
    if (dt < 1.0) return;
    uint64_t cpu = process_cpu_ns();
    double cpu_pct = cpu && R.stats_cpu_ns ? (double)(cpu - R.stats_cpu_ns) / (dt * 1e9) * 100.0 : -1;
    int n = R.stats_frames ? R.stats_frames : 1;
    say("stats: frames=%.1f/s pictures=%.1f/s speed=%.1f%% run=%.2f/%.2f read=%.2f/%.2f send=%.2f/%.2f late=%d early=%d cpu=%.0f%%",
        R.stats_frames / dt, R.stats_pictures / dt,
        R.sample_rate > 0 ? R.stats_sound_frames / dt / R.sample_rate * 100.0 : 0.0,
        R.stats_run_sum / n, R.stats_run_max, R.stats_read_sum / n, R.stats_read_max,
        R.stats_send_sum / n, R.stats_send_max, R.stats_late, R.stats_early, cpu_pct);
    R.stats_t0 = t; R.stats_cpu_ns = cpu;
    R.stats_frames = R.stats_late = R.stats_pictures = R.stats_early = 0;
    R.stats_run_sum = R.stats_run_max = R.stats_read_sum = R.stats_read_max = R.stats_send_sum = R.stats_send_max = 0;
    R.stats_sound_frames = 0;
}

/* ============================== connection ============================== */

static int connect_to_app(const char *hostport)
{
    char host[128];
    const char *colon = strrchr(hostport, ':');
    if (!colon) return 0;
    size_t n = (size_t)(colon - hostport);
    if (n >= sizeof host) return 0;
    memcpy(host, hostport, n); host[n] = 0;
    int port = atoi(colon + 1);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return 0;
    R.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (R.sock == BAD_SOCK) return 0;
    if (connect(R.sock, (struct sockaddr *)&addr, sizeof addr) != 0) { close_sock(R.sock); return 0; }
    int one = 1;   /* send each message at once, don't batch them up (latency) */
    setsockopt(R.sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    R.connected = 1;
    return 1;
}

/* Wait up to `timeout` seconds for incoming data and append it to R.rx. 0 = connection closed. */
static int receive(double timeout)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(R.sock, &fds);
    struct timeval tv;
    if (timeout < 0) timeout = 0;
    tv.tv_sec = (long)timeout;
    tv.tv_usec = (long)((timeout - (double)tv.tv_sec) * 1e6);
    int r = select((int)R.sock + 1, &fds, NULL, NULL, &tv);
    if (r < 0) return 0;
    if (r == 0) return 1;
    if (R.rx_len + 65536 > R.rx_cap) {
        size_t c = R.rx_cap ? R.rx_cap * 2 : 131072;
        uint8_t *p = realloc(R.rx, c);
        if (!p) return 0;
        R.rx = p; R.rx_cap = c;
    }
    int n = recv(R.sock, (char *)R.rx + R.rx_len, 65536, 0);
    if (n <= 0) return 0;
    R.rx_len += (size_t)n;
    return 1;
}

/* Take one complete message from R.rx. Returns 1 and fills type/payload/len, 0 if none yet,
 * -1 if the stream is broken. The payload stays valid until the next call. */
static uint8_t *msg_buf;
static size_t msg_cap;
static int next_message(uint8_t *type, uint8_t **payload, size_t *len)
{
    if (R.rx_len < 5) return 0;
    uint32_t n = (uint32_t)R.rx[0] | ((uint32_t)R.rx[1] << 8) | ((uint32_t)R.rx[2] << 16) | ((uint32_t)R.rx[3] << 24);
    if (n < 1 || n > PLG_MAX_MESSAGE) return -1;
    if (R.rx_len < 4 + (size_t)n) return 0;
    if (n > msg_cap) {
        uint8_t *p = realloc(msg_buf, n + 1);
        if (!p) return -1;
        msg_buf = p; msg_cap = n;
    }
    *type = R.rx[4];
    *len = n - 1;
    memcpy(msg_buf, R.rx + 5, *len);
    msg_buf[*len] = 0;   /* so text payloads can be used as C strings */
    *payload = msg_buf;
    memmove(R.rx, R.rx + 4 + n, R.rx_len - 4 - n);
    R.rx_len -= 4 + n;
    return 1;
}

static void handle_message(uint8_t type, const uint8_t *p, size_t len)
{
    switch (type) {
    case MSG_INPUT:
        if (len < PLG_INPUT_SIZE) break;
        R.input.ctl.aim_x = (uint16_t)(p[0] | (p[1] << 8));
        R.input.ctl.aim_y = (uint16_t)(p[2] | (p[3] << 8));
        R.input.ctl.on_screen = (p[4] & AIM_ON_SCREEN) != 0;
        R.input.ctl.buttons = p[5];
        R.input.ctl.stick_x = (int16_t)(p[6] | (p[7] << 8));
        R.input.ctl.stick_y = (int16_t)(p[8] | (p[9] << 8));
        R.input.ctl.number = (uint16_t)(p[10] | (p[11] << 8));
        R.input_fresh = 1;
        break;
    case MSG_PAUSE:
        /* The headset may be about to sleep. fflush(NULL) pushes out every file the core has open
         * but not flushed - Flycast writes memory-card blocks with fwrite and never flushes them, so
         * without this a forced stop could lose the last save. */
        if (!R.paused) { R.paused = 1; save_flush(); fflush(NULL); }
        break;
    case MSG_RESUME:
        R.paused = 0;
        break;
    case MSG_STOP:
        R.stopping = 1;
        break;
    default:
        say("ignoring message type %u", type);
        break;
    }
}

/* ---- the GPU test's view of the runner (see gpu_test.h) ---- */

static int pump(double timeout)   /* wait for and handle messages; 0 = the app went away */
{
    uint8_t type, *p;
    size_t len;
    int r;
    if (!receive(timeout)) { R.connected = 0; return 0; }
    while ((r = next_message(&type, &p, &len)) > 0) handle_message(type, p, len);
    if (r < 0) { R.connected = 0; return 0; }
    return 1;
}
static void gt_wait(double s) { pump(s); }
static int gt_keep_going(void) { return R.connected && !R.stopping; }
static int gt_paused(void) { return R.paused; }
static void gt_send_picture(uint32_t number, int w, int h, int format, const uint8_t *px, size_t bytes)
{
    (void)bytes;
    send_picture(number, w, h, format, 0, now_us(), 0, 0, px);
}
static void gt_log(const char *fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    say("%s", buf);
}
static void gt_ready(const char *fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    say("%s", buf);
    send_text(MSG_READY, "width=%d\nheight=%d\naspect=%.6f\nfps=60\nsample_rate=48000\ngpu_test=on",
              R.gpu_width, R.gpu_height, (double)R.gpu_width / R.gpu_height);
}
static void gt_failed(const char *fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    say("%s", buf);
    send_text(MSG_FAILED, "%s", buf);
}

static int run_gpu_test(void)
{
    gpu_test_io io = { now_seconds, gt_wait, gt_keep_going, gt_paused, gt_send_picture, gt_log, gt_ready, gt_failed };
    if (R.gpu_width <= 0) R.gpu_width = 640;
    if (R.gpu_height <= 0) R.gpu_height = 480;
    if (R.gpu_load <= 0) R.gpu_load = 8;
    int ok = gpu_test_run(&io, R.gpu_width, R.gpu_height, R.gpu_load);
    if (R.connected) send_msg(MSG_STOPPED, NULL, 0);
    close_sock(R.sock);
    return ok ? 0 : 5;
}

static int run_connected(const char *hostport, const char *token)
{
    if (!connect_to_app(hostport)) { fprintf(stderr, "[runner] could not connect to %s\n", hostport); return 2; }
    send_text(MSG_HELLO, "runner=%s\nprotocol=%d\ntoken=%s", RUNNER_VERSION, PLG_PROTOCOL_VERSION, token);

    /* Wait for START. */
    uint8_t type, *p;
    size_t len;
    for (;;) {
        int r = next_message(&type, &p, &len);
        if (r < 0) { say("bad message from the app"); return 3; }
        if (r == 0) { if (!receive(1.0)) return 0; continue; }
        if (type == MSG_START) break;
        if (type == MSG_STOP) return 0;
    }
    apply_start((char *)p);
    if (R.gpu_test) return run_gpu_test();

    char err[1200];
    if (!load_core_and_game(err, sizeof err)) {
        send_text(MSG_FAILED, "%s", err);
        say("%s", err);
        return 4;
    }
    send_text(MSG_READY, "width=%u\nheight=%u\naspect=%.6f\nfps=%.6f\nsample_rate=%.1f",
              R.base_w, R.base_h, R.aspect, R.fps, R.sample_rate);

    /* Play. The runner's own clock sets the pace; waiting for the next frame is done by
     * waiting for messages, so input arriving in between is picked up straight away. */
    double period = 1.0 / R.fps, next = now_seconds();
    R.stats_t0 = 0;
    while (R.connected && !R.stopping) {
        int r;
        /* Pick up whatever has arrived WITHOUT waiting. The waits below also read, but a runner that
         * is behind schedule never waits - and then never saw PAUSE, STOP or new INPUT at all. */
        if (!receive(0)) break;
        while ((r = next_message(&type, &p, &len)) > 0) handle_message(type, p, len);
        if (r < 0) { say("bad message from the app"); break; }
        if (R.stopping) break;

        double t = now_seconds();
        if (R.paused) { next = t; R.stats_t0 = 0; if (!receive(0.05)) break; continue; }
        if (t < next) {
            if (R.input_fresh && next - t <= R.early_s) R.stats_early++;   /* new input, frame nearly due: go */
            else { if (!receive(next - t)) break; continue; }
        }
        R.input_fresh = 0;

        if (t - next > period) R.stats_late++;   /* more than a frame behind its due time */
        next += run_frame();
        if (t - next > 0.25) next = t;   /* fell far behind (debugger, system hiccup): don't race to catch up */
        stats_tick(now_seconds());
    }

    unload();
    if (R.connected) send_msg(MSG_STOPPED, NULL, 0);
    close_sock(R.sock);
    return 0;
}

/* ============================== --test ============================== */

static void usage(void)
{
    fprintf(stderr,
        "Project Light Gun core runner " RUNNER_VERSION "\n"
        "  core-runner --connect 127.0.0.1:<port> --token <token>\n"
        "  core-runner --test --core <file> --game <file> [--system <dir>] [--save <dir>]\n"
        "              [--cart-save <file>] [--port0 none|pad|zapper] [--port1 none|pad|zapper]\n"
        "              [--option <name>=<value>]... [--set <setting>=<value>]...\n"
        "              [--frames <n>] [--out <dir>] [--dump-every <n>]\n");
}

static int run_test(int frames, const char *out, int dump_every)
{
    char err[1200];
    if (!load_core_and_game(err, sizeof err)) { fprintf(stderr, "[runner] %s\n", err); return 4; }
    make_dir(out);
    char path[1200];
    double t0 = now_seconds();
    int pictures = 0;
    for (int i = 1; i <= frames; i++) {
        run_frame();
        if ((dump_every > 0 && i % dump_every == 0) || i == frames) {
            if (R.pic_w > 0) {
                snprintf(path, sizeof path, "%s/frame_%05d.png", out, i);
                if (dump_png(path, R.picture, R.pic_w, R.pic_h, R.pic_format)) pictures++;
            }
        }
    }
    double secs = now_seconds() - t0;
    snprintf(path, sizeof path, "%s/sound.wav", out);
    dump_wav(path, R.test_sound, R.test_sound_frames, (int)(R.sample_rate + 0.5));
    say("ran %d frames in %.2f s (%.0f frames a second); %d pictures, %zu sound frames (%.2f s) -> %s",
        frames, secs, frames / (secs > 0 ? secs : 1), pictures, R.test_sound_frames,
        R.test_sound_frames / R.sample_rate, out);
    unload();
    return 0;
}

/* ============================== main ============================== */

int main(int argc, char **argv)
{
    memset(&R, 0, sizeof R);
    R.sock = BAD_SOCK;
    R.input.port[0] = DEV_PAD;
    R.input.port[1] = DEV_ZAPPER;
    R.bind_fbo = 1;

    const char *connect_to = NULL, *token = "", *out = "runner-out";
    int test = 0, frames = 600, dump_every = 60;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKE (v ? (i++, v) : (usage(), exit(1), ""))
        if (!strcmp(a, "--connect")) connect_to = TAKE;
        else if (!strcmp(a, "--token")) token = TAKE;
        else if (!strcmp(a, "--test")) test = 1;
        else if (!strcmp(a, "--core")) apply_setting("core", TAKE);
        else if (!strcmp(a, "--game")) apply_setting("game", TAKE);
        else if (!strcmp(a, "--system")) apply_setting("system_dir", TAKE);
        else if (!strcmp(a, "--save")) apply_setting("save_dir", TAKE);
        else if (!strcmp(a, "--cart-save")) apply_setting("cart_save", TAKE);
        else if (!strcmp(a, "--port0")) apply_setting("port0", TAKE);
        else if (!strcmp(a, "--port1")) apply_setting("port1", TAKE);
        else if (!strcmp(a, "--option")) apply_setting("option", TAKE);
        else if (!strcmp(a, "--set")) {
            char kv[512];
            copy_str(kv, sizeof kv, TAKE);
            char *eq = strchr(kv, '=');
            if (!eq) { usage(); return 1; }
            *eq = 0;
            if (!apply_setting(kv, eq + 1)) { fprintf(stderr, "[runner] unknown setting %s\n", kv); return 1; }
        }
        else if (!strcmp(a, "--frames")) frames = atoi(TAKE);
        else if (!strcmp(a, "--out")) out = TAKE;
        else if (!strcmp(a, "--dump-every")) dump_every = atoi(TAKE);
        else { usage(); return 1; }
#undef TAKE
    }

    if (connect_to) return run_connected(connect_to, token);
    if (test && R.core[0] && R.game[0]) { R.test_mode = 1; return run_test(frames, out, dump_every); }
    usage();
    return 1;
}
