/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Project Light Gun
 */
/*
 * Project Light Gun core runner - a GPU context for cores that draw with OpenGL ES. See hw_render.h.
 *
 * Mirrors the app's GLHwContext.cs (Android branch) so the core sees the same thing in both setups:
 * same EGL config (RGBA8, depth 24, stencil 8, pbuffer), same GLES 3 context, same RGBA8 colour
 * texture + packed depth/stencil renderbuffer, same eglGetProcAddress-then-dlsym lookup. The runner
 * owns the GL context outright (no Unity sharing the thread), so the copy-back needs none of the
 * app's save/restore-the-core's-GL-state dance; it only resets what glReadPixels depends on.
 */
#include <stdio.h>
#include <string.h>
#include "hw_render.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static void *s_gles;
static GLuint s_fbo, s_tex, s_rb;
static int s_w, s_h;
static GLuint s_pbo[2];
static int s_pbo_w[2], s_pbo_h[2], s_pbo_pending[2], s_pbo_cur;

int hw_create(char *err, size_t errlen)
{
    s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s_dpy == EGL_NO_DISPLAY || !eglInitialize(s_dpy, NULL, NULL)) {
        snprintf(err, errlen, "hw: no EGL display (error 0x%x)", eglGetError());
        return 0;
    }
    const EGLint cfg_attr[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        snprintf(err, errlen, "hw: no GLES3 pbuffer config (error 0x%x)", eglGetError());
        return 0;
    }
    const EGLint surf_attr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    s_surf = eglCreatePbufferSurface(s_dpy, cfg, surf_attr);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (s_surf == EGL_NO_SURFACE || s_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
        snprintf(err, errlen, "hw: could not create/make current a GLES3 context (error 0x%x)", eglGetError());
        return 0;
    }
    s_gles = dlopen("libGLESv2.so", RTLD_NOW);
    return 1;
}

int hw_alloc_target(int width, int height, int depth, int stencil, char *err, size_t errlen)
{
    s_w = width > 0 ? width : 1;
    s_h = height > 0 ? height : 1;
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_w, s_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
    if (depth || stencil) {
        glGenRenderbuffers(1, &s_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, s_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_w, s_h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_rb);
    }
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(err, errlen, "hw: framebuffer incomplete 0x%04x (%dx%d)", status, s_w, s_h);
        return 0;
    }
    return 1;
}

void hw_make_current(void)
{
    if (s_ctx != EGL_NO_CONTEXT) eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
}

void hw_bind_for_render(void)
{
    if (!s_fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_w, s_h);
}

uintptr_t hw_framebuffer(void) { return s_fbo; }

void *hw_get_proc_address(const char *sym)
{
    void *p = (void *)eglGetProcAddress(sym);
    if (!p && s_gles) p = dlsym(s_gles, sym);
    return p;
}

/* Put the read-side state glReadPixels depends on back to plain values. The core may have left a
 * pixel-pack buffer bound (glReadPixels would then write into it, not into our memory) or pack
 * row-length/skip settings (GLHwContext.cs found exactly this on the Wii). */
static void read_state(void)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
}

int hw_read(uint8_t *dst, int w, int h, int async)
{
    if (!s_fbo || !dst) return 0;
    GLint prev_read = 0, prev_pbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
    read_state();
    int got = 0;
    if (!async) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);   /* waits for the GPU */
        got = 1;
    } else {
        if (!s_pbo[0]) glGenBuffers(2, s_pbo);
        int cur = s_pbo_cur, prev = 1 - cur;
        GLsizeiptr size = (GLsizeiptr)w * h * 4;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[cur]);
        if (s_pbo_w[cur] != w || s_pbo_h[cur] != h) {
            glBufferData(GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);
            s_pbo_w[cur] = w; s_pbo_h[cur] = h;
        }
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 0);   /* starts the copy, no waiting */
        s_pbo_pending[cur] = 1;
        if (s_pbo_pending[prev] && s_pbo_w[prev] == w && s_pbo_h[prev] == h) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[prev]);
            void *p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT);
            if (p) { memcpy(dst, p, (size_t)size); got = 1; }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            s_pbo_pending[prev] = 0;
        }
        s_pbo_cur = prev;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    return got;
}

void hw_destroy(void)
{
    if (s_ctx == EGL_NO_CONTEXT) return;
    hw_make_current();
    if (s_pbo[0]) { glDeleteBuffers(2, s_pbo); s_pbo[0] = s_pbo[1] = 0; }
    if (s_rb) { glDeleteRenderbuffers(1, &s_rb); s_rb = 0; }
    if (s_fbo) { glDeleteFramebuffers(1, &s_fbo); s_fbo = 0; }
    if (s_tex) { glDeleteTextures(1, &s_tex); s_tex = 0; }
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    eglDestroySurface(s_dpy, s_surf);
    eglTerminate(s_dpy);   /* our own process: unlike the app, nobody else is using this display */
    s_ctx = EGL_NO_CONTEXT;
    if (s_gles) { dlclose(s_gles); s_gles = NULL; }
}

#else   /* Windows build: no GPU cores in the runner there */

int hw_create(char *err, size_t errlen) { snprintf(err, errlen, "hw: GPU cores only run in the headset build"); return 0; }
int hw_alloc_target(int width, int height, int depth, int stencil, char *err, size_t errlen)
{ (void)width; (void)height; (void)depth; (void)stencil; snprintf(err, errlen, "hw: not available"); return 0; }
void hw_make_current(void) {}
void hw_bind_for_render(void) {}
uintptr_t hw_framebuffer(void) { return 0; }
void *hw_get_proc_address(const char *sym) { (void)sym; return NULL; }
int hw_read(uint8_t *dst, int w, int h, int async) { (void)dst; (void)w; (void)h; (void)async; return 0; }
void hw_destroy(void) {}

#endif
