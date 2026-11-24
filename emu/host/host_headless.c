/*
 * host_headless.c - headless WP5 backend: no window system needed.
 *
 * present() optionally writes each frame as a PPM snapshot (P6); poll()
 * serves a scripted event queue filled via host_headless_push_key(); used
 * by CI tests and by tools/sophie when no display is available.
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hl_close(HostWindow *w)
{
    if (w == NULL)
        return;
    free(w->ppm);
    free(w);
}

static int hl_present(HostWindow *w, const uint32_t *fb, int ww, int hh)
{
    (void)ww; (void)hh;
    w->frames++;
    if (w->ppm != NULL && fb != NULL) {
        FILE *f = fopen(w->ppm, "wb");
        if (f != NULL) {
            fprintf(f, "P6\n%d %d\n255\n", w->width, w->height);
            for (int i = 0; i < w->width * w->height; i++) {
                uint32_t p = fb[i];
                fputc((int)((p >> 16) & 0xFF), f);
                fputc((int)((p >> 8) & 0xFF), f);
                fputc((int)(p & 0xFF), f);
            }
            fclose(f);
        }
    }
    return 0;
}

static int hl_poll(HostWindow *w, HostKeyEvent *ev)
{
    if (w->q_head == w->q_tail)
        return 0;
    *ev = w->queue[w->q_head];
    w->q_head = (w->q_head + 1) % HOST_QUEUE;
    return 1;
}

static int hl_should_quit(HostWindow *w)
{
    return w->quit;
}

static const HostBackend hl_backend = {
    hl_close, hl_present, hl_poll, hl_should_quit
};

HostWindow *host_headless_open(const char *title, int w, int h)
{
    (void)title;
    HostWindow *win = calloc(1, sizeof *win);
    if (win == NULL)
        return NULL;
    win->be = &hl_backend;
    win->width = w;
    win->height = h;
    return win;
}

void host_headless_set_ppm(HostWindow *w, const char *path)
{
    if (w == NULL)
        return;
    free(w->ppm);
    w->ppm = path != NULL ? strdup(path) : NULL;
}

void host_headless_push_key(HostWindow *w, int key, int press)
{
    if (w == NULL)
        return;
    int next = (w->q_tail + 1) % HOST_QUEUE;
    if (next == w->q_head)
        return; /* queue full: drop */
    w->queue[w->q_tail].key = key;
    w->queue[w->q_tail].press = press;
    w->q_tail = next;
}

void host_headless_set_quit(HostWindow *w, int quit)
{
    if (w != NULL)
        w->quit = quit;
}
