/*
 * Project Light Gun core runner - a GPU context for cores that draw with OpenGL ES (Dreamcast).
 *
 * The runner side of what the app's GLHwContext.cs did when the core ran inside the app: an
 * off-screen EGL + GLES 3 context, a framebuffer the core draws into, and the copy of each finished
 * picture back from the GPU. Headset (Android) only; on Windows every call fails cleanly.
 */
#ifndef PLG_HW_RENDER_H
#define PLG_HW_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* Create the context and make it current on the calling thread. 0 = failed (err says why). */
int hw_create(char *err, size_t errlen);

/* The framebuffer the core draws into, sized for the core's largest picture. */
int hw_alloc_target(int width, int height, int depth, int stencil, char *err, size_t errlen);

void hw_make_current(void);

/* Bind the framebuffer + viewport before retro_run. Flycast expects it pre-bound (the app did the
 * same, GLHwContext.BindForRender). */
void hw_bind_for_render(void);

uintptr_t hw_framebuffer(void);
void *hw_get_proc_address(const char *sym);

/* Copy the finished w x h picture into dst (RGBA, 4 bytes a pixel, BOTTOM row first - GL's order).
 *   async = 0: waits for the GPU, dst holds THIS frame. Returns 1.
 *   async = 1: starts this frame's copy and hands back the PREVIOUS frame's (one frame later,
 *              no waiting). Returns 0 when there's nothing to hand back yet. */
int hw_read(uint8_t *dst, int w, int h, int async);

void hw_destroy(void);

#endif
