/*
 * host.h - WP5 host window layer: presents VIDC frames and pumps keyboard
 * input into the machine's IOC KART.
 *
 * The layer is a small backend interface so the emulator core stays
 * host-OS-agnostic (spec section 3): the default build uses the headless
 * backend (PPM snapshots + scripted input, used by CI/tests); build with
 * `make host-x11` to get a real X11 window backend (libX11 via dlopen, so
 * the default build has no X11 dependency).
 *
 * Key events carry host key codes: ASCII for printable keys, HOST_KEY_*
 * (>= 0x100) for specials; they are translated to RISC OS low-level key
 * numbers (KART make = key, break = key|0x80) by host_key_to_riscos().
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#ifndef SOPHIE_HOST_H
#define SOPHIE_HOST_H

#include <stdint.h>

#include "machine.h"

/* host key codes for non-printable keys */
#define HOST_KEY_BASE 0x100
enum {
    HOST_KEY_LEFT = HOST_KEY_BASE, HOST_KEY_RIGHT, HOST_KEY_UP, HOST_KEY_DOWN,
    HOST_KEY_RETURN, HOST_KEY_ESC, HOST_KEY_BACKSPACE, HOST_KEY_DELETE,
    HOST_KEY_TAB, HOST_KEY_HOME, HOST_KEY_END, HOST_KEY_PGUP, HOST_KEY_PGDN,
    HOST_KEY_INSERT, HOST_KEY_CAPS,
    HOST_KEY_F1, HOST_KEY_F2, HOST_KEY_F3, HOST_KEY_F4, HOST_KEY_F5,
    HOST_KEY_F6, HOST_KEY_F7, HOST_KEY_F8, HOST_KEY_F9, HOST_KEY_F10,
    HOST_KEY_F11, HOST_KEY_F12,
    HOST_KEY_LSHIFT, HOST_KEY_RSHIFT, HOST_KEY_LCTRL, HOST_KEY_RCTRL,
    HOST_KEY_LALT, HOST_KEY_RALT
};

typedef struct HostKeyEvent {
    int key;    /* host key code */
    int press;  /* 1 = make, 0 = break */
} HostKeyEvent;

typedef struct HostWindow HostWindow;
typedef struct HostBackend {
    void (*close)(HostWindow *w);
    int  (*present)(HostWindow *w, const uint32_t *fb, int ww, int hh);
    int  (*poll)(HostWindow *w, HostKeyEvent *ev);
    int  (*should_quit)(HostWindow *w);
} HostBackend;

#define HOST_QUEUE 64
struct HostWindow {
    const HostBackend *be;
    int width, height;
    /* headless backend state */
    char *ppm;
    uint64_t frames;
    HostKeyEvent queue[HOST_QUEUE];
    int q_head, q_tail;
    int quit;
    /* x11 backend state (opaque) */
    void *x11;
};

/* Open a window (or headless surface) of w x h. Returns NULL on failure. */
HostWindow *host_open(const char *title, int w, int h);
void host_close(HostWindow *w);

/* Present a 32bpp 0xRRGGBB framebuffer (w x h). Returns 0 on success. */
int host_present(HostWindow *w, const uint32_t *fb, int ww, int hh);

/* Poll one input event. Returns 1 if an event was written to *ev. */
int host_poll(HostWindow *w, HostKeyEvent *ev);

int host_should_quit(HostWindow *w);

/* Translate a host key code to a RISC OS low-level key number (0-0x7F),
 * or -1 if unmapped. */
int host_key_to_riscos(int host_key);

/* Poll all pending host events and inject them into the machine's KART
 * (make -> ioc_key_press, break -> ioc_key_release). */
void host_pump_input(HostWindow *w, Machine *m);

/* Headless-backend controls (no-ops on a real window backend). */
void host_headless_set_ppm(HostWindow *w, const char *path);  /* snapshot each presented frame */
void host_headless_push_key(HostWindow *w, int key, int press); /* scripted input event */
void host_headless_set_quit(HostWindow *w, int quit);

/* Backend entries (host_headless.c / host_x11.c). */
HostWindow *host_headless_open(const char *title, int w, int h);
#ifdef SOPHIE_HOST_X11
HostWindow *host_x11_open(const char *title, int w, int h);
#endif

#endif /* SOPHIE_HOST_H */
