/*
 * host.c - WP5 host layer dispatch + input glue.
 *
 * host_open() uses the X11 backend when built with SOPHIE_HOST_X11 and a
 * DISPLAY is present, otherwise the headless backend. host_pump_input()
 * translates host key events into KART make/break bytes on the machine.
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "host.h"

#include <stdlib.h>

HostWindow *host_open(const char *title, int w, int h)
{
#ifdef SOPHIE_HOST_X11
    if (getenv("DISPLAY") != NULL) {
        HostWindow *win = host_x11_open(title, w, h);
        if (win != NULL)
            return win;
    }
#endif
    return host_headless_open(title, w, h);
}

void host_close(HostWindow *w)
{
    if (w != NULL && w->be != NULL && w->be->close != NULL)
        w->be->close(w);
}

int host_present(HostWindow *w, const uint32_t *fb, int ww, int hh)
{
    return (w != NULL && w->be != NULL && w->be->present != NULL)
         ? w->be->present(w, fb, ww, hh) : -1;
}

int host_poll(HostWindow *w, HostKeyEvent *ev)
{
    return (w != NULL && w->be != NULL && w->be->poll != NULL)
         ? w->be->poll(w, ev) : 0;
}

int host_should_quit(HostWindow *w)
{
    return (w != NULL && w->be != NULL && w->be->should_quit != NULL)
         ? w->be->should_quit(w) : 1;
}

void host_pump_input(HostWindow *w, Machine *m)
{
    HostKeyEvent ev;
    while (w != NULL && m != NULL && host_poll(w, &ev)) {
        int rk = host_key_to_riscos(ev.key);
        if (rk < 0)
            continue;
        if (ev.press)
            ioc_key_press(&m->ioc, (uint8_t)rk);
        else
            ioc_key_release(&m->ioc, (uint8_t)rk);
    }
}
