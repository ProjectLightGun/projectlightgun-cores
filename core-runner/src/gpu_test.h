#ifndef PLG_GPU_TEST_H
#define PLG_GPU_TEST_H
#include <stddef.h>
#include <stdint.h>

/* What the GPU test needs from the runner (its connection and clock), so gpu_test.c stays
 * separate from runner.c. */
typedef struct {
    double (*now)(void);
    void (*wait)(double seconds);          /* waits, reading messages from the app meanwhile */
    int (*keep_going)(void);               /* 0 once STOP arrived or the app went away */
    int (*paused)(void);
    void (*send_picture)(uint32_t number, int w, int h, int format, const uint8_t *pixels, size_t bytes);
    void (*log)(const char *fmt, ...);
    void (*ready)(const char *fmt, ...);   /* sends READY (with this line also as a LOG) */
    void (*failed)(const char *fmt, ...);  /* sends FAILED */
} gpu_test_io;

/* Runs until keep_going() says stop. Returns 0 if the GPU could not be used. */
int gpu_test_run(const gpu_test_io *io, int width, int height, int load);

#endif
