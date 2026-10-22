/*
 * Project Light Gun core runner - the early GPU test (plan S4).
 *
 * Instead of running a core, the runner opens its OWN graphics context (off-screen, EGL + GLES 3),
 * draws a fake picture every frame, copies it back from the GPU and sends it to the app, exactly as
 * a GPU-drawn core (PS1, Saturn, Dreamcast) would have to after the split. It answers two questions
 * before Part 4 depends on them:
 *   1. can a second program (not the app) use the GPU on the headset at all?
 *   2. does that program sharing the GPU with the app make either of them stutter?
 * Once a second it sends a LOG line with its timings. Headset (Android) only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gpu_test.h"
#include "protocol.h"

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <GLES3/gl3.h>

static const char *VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* A moving pattern with some arithmetic per pixel; `load` sets how much (a stand-in for a game's
 * shading work). */
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform float t; uniform int load;\n"
    "void main() {\n"
    "  vec3 c = vec3(0.0);\n"
    "  for (int i = 0; i < load; i++) {\n"
    "    float f = float(i) * 0.37;\n"
    "    c += 0.5 + 0.5 * sin(vec3(uv.x * 9.0 + t + f, uv.y * 7.0 - t * 1.3 + f, (uv.x + uv.y) * 5.0 + f));\n"
    "  }\n"
    "  o = vec4(c / float(max(load, 1)), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src, char *err, size_t errlen)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(s, (GLsizei)errlen, NULL, err); return 0; }
    return s;
}

int gpu_test_run(const gpu_test_io *io, int width, int height, int load)
{
    char err[512];
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        io->failed("gpu-test: no EGL display (eglGetDisplay/eglInitialize failed, error 0x%x)", eglGetError());
        return 0;
    }
    const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        io->failed("gpu-test: no GLES3 pbuffer config (error 0x%x)", eglGetError());
        return 0;
    }
    const EGLint surf_attr[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surf_attr);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        io->failed("gpu-test: could not create/make current a GLES3 context (error 0x%x)", eglGetError());
        return 0;
    }

    GLuint vs = compile(GL_VERTEX_SHADER, VS, err, sizeof err);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, FS, err, sizeof err) : 0;
    if (!fs) { io->failed("gpu-test: shader: %s", err); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);
    GLint u_t = glGetUniformLocation(prog, "t"), u_load = glGetUniformLocation(prog, "load");
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, width, height);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    io->ready("gpu-test: renderer=%s | vendor=%s | version=%s | %dx%d load %d",
              (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VENDOR),
              (const char *)glGetString(GL_VERSION), width, height, load);

    size_t bytes = (size_t)width * height * 4;
    uint8_t *pixels = malloc(bytes), *flipped = malloc(bytes);
    if (!pixels || !flipped) { io->failed("gpu-test: out of memory"); return 0; }

    double period = 1.0 / 60.0, next = io->now(), second = next;
    double sum_draw = 0, max_draw = 0, sum_send = 0, max_send = 0;
    int frames = 0, late = 0;
    uint32_t number = 0;
    while (io->keep_going()) {
        double t = io->now();
        if (io->paused()) { next = t; io->wait(0.05); continue; }
        if (t < next) { io->wait(next - t); continue; }

        double t0 = io->now();
        glUniform1f(u_t, (float)t);
        glUniform1i(u_load, load);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);   /* waits for the GPU */
        double t1 = io->now();
        for (int y = 0; y < height; y++)   /* GL is bottom row first; PICTURE is top row first */
            memcpy(flipped + (size_t)y * width * 4, pixels + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);
        io->send_picture(++number, width, height, PIX_RGBA8, flipped, bytes);
        double t2 = io->now();

        double draw = (t1 - t0) * 1000, send = (t2 - t1) * 1000;
        sum_draw += draw; sum_send += send;
        if (draw > max_draw) max_draw = draw;
        if (send > max_send) max_send = send;
        frames++;
        next += period;
        if (t2 > next) late++;
        if (t - next > 0.25) next = t;

        if (t2 - second >= 1.0) {
            io->log("gpu-test: %d pictures/s | GPU draw+readback avg %.2f ms max %.2f | send avg %.2f ms max %.2f | %.1f MB/s | late %d",
                    frames, sum_draw / frames, max_draw, sum_send / frames, max_send,
                    frames * bytes / 1048576.0 / (t2 - second), late);
            second = t2;
            frames = late = 0;
            sum_draw = max_draw = sum_send = max_send = 0;
        }
    }

    free(pixels); free(flipped);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    return 1;
}

#else

int gpu_test_run(const gpu_test_io *io, int width, int height, int load)
{
    (void)width; (void)height; (void)load;
    io->failed("gpu-test: only built for the headset (Android)");
    return 0;
}

#endif
