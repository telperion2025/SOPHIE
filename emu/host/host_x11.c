/*
 * host_x11.c - WP5 X11 window backend, loaded via dlopen("libX11.so") so the
 * default build has no X11 dependency (build with `make host-x11`; used
 * automatically when DISPLAY is set). Only compiled with SOPHIE_HOST_X11.
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#ifdef SOPHIE_HOST_X11

#include "host.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

/* minimal X11 declarations (ABI-stable structs / prototypes) */
typedef struct {
    int width, height, xoffset, format;
    char *data;
    int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth;
    int bytes_per_line, bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
    void *obdata;
    void *f_create_image, *f_destroy_image, *f_get_pixel, *f_put_pixel,
         *f_sub_image, *f_add_pixel;
} XImage;
typedef void Display;
typedef unsigned long Window, Atom;
typedef unsigned long KeySym;
typedef union { int type; char pad[192]; } XEvent;
typedef struct {
    int type; unsigned long serial; int send_event;
    Display *display; Window window; Atom message_type; int format;
    union { char b[20]; short s[10]; long l[5]; } data;
} XClientMessageEvent;

#define KeyPress 2
#define KeyRelease 3
#define ClientMessage 33
#define KeyPressMask (1L << 0)
#define KeyReleaseMask (1L << 1)
#define StructureNotifyMask (1L << 17)

static void *x11;
static Display *(*p_XOpenDisplay)(const char *);
static int (*p_XCloseDisplay)(Display *);
static int (*p_XDefaultScreen)(Display *);
static Window (*p_XRootWindow)(Display *, int);
static Window (*p_XCreateSimpleWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
static int (*p_XStoreName)(Display *, Window, const char *);
static int (*p_XSelectInput)(Display *, Window, long);
static int (*p_XMapWindow)(Display *, Window);
static int (*p_XPending)(Display *);
static int (*p_XNextEvent)(Display *, XEvent *);
static XImage *(*p_XCreateImage)(Display *, void *, unsigned, int, int, char *, unsigned, unsigned, int, int);
static int (*p_XPutImage)(Display *, Window, void *, XImage *, int, int, int, int, unsigned, unsigned);
static int (*p_XDestroyImage)(XImage *);
static KeySym (*p_XLookupKeysym)(XEvent *, int);
static Atom (*p_XInternAtom)(Display *, const char *, int);
static int (*p_XSetWMProtocols)(Display *, Window, Atom *, int);
static int (*p_XSync)(Display *, int);

typedef struct X11Win {
    Display *dpy;
    Window win;
    void *gc;
    XImage *img;
    Atom wm_delete;
    HostWindow *host;
    int quit;
} X11Win;

static KeySym keysym_to_host_key(KeySym ks, int *ok)
{
    *ok = 1;
    if (ks >= 0x20 && ks <= 0x7E)
        return (KeySym)(ks & 0x7F); /* ASCII */
    switch (ks) {
      case 0xFF08: return HOST_KEY_BACKSPACE;
      case 0xFF09: return HOST_KEY_TAB;
      case 0xFF0D: return HOST_KEY_RETURN;
      case 0xFF1B: return HOST_KEY_ESC;
      case 0xFFFF: return HOST_KEY_DELETE;
      case 0xFF50: return HOST_KEY_HOME;
      case 0xFF51: return HOST_KEY_LEFT;
      case 0xFF52: return HOST_KEY_UP;
      case 0xFF53: return HOST_KEY_RIGHT;
      case 0xFF54: return HOST_KEY_DOWN;
      case 0xFF55: return HOST_KEY_PGUP;
      case 0xFF56: return HOST_KEY_PGDN;
      case 0xFF57: return HOST_KEY_END;
      case 0xFF63: return HOST_KEY_INSERT;
      case 0xFFE5: return HOST_KEY_CAPS;
      case 0xFFBE: return HOST_KEY_F1;
      case 0xFFBF: return HOST_KEY_F2;
      case 0xFFC0: return HOST_KEY_F3;
      case 0xFFC1: return HOST_KEY_F4;
      case 0xFFC2: return HOST_KEY_F5;
      case 0xFFC3: return HOST_KEY_F6;
      case 0xFFC4: return HOST_KEY_F7;
      case 0xFFC5: return HOST_KEY_F8;
      case 0xFFC6: return HOST_KEY_F9;
      case 0xFFC7: return HOST_KEY_F10;
      case 0xFFC8: return HOST_KEY_F11;
      case 0xFFC9: return HOST_KEY_F12;
      case 0xFFE1: return HOST_KEY_LSHIFT;
      case 0xFFE2: return HOST_KEY_RSHIFT;
      case 0xFFE3: return HOST_KEY_LCTRL;
      case 0xFFE4: return HOST_KEY_RCTRL;
      case 0xFFE9: return HOST_KEY_LALT;
      case 0xFFEA: return HOST_KEY_RALT;
      default: *ok = 0; return 0;
    }
}

static void x11_close(HostWindow *w)
{
    X11Win *x = w->x11;
    if (x != NULL) {
        if (x->img != NULL && p_XDestroyImage != NULL)
            p_XDestroyImage(x->img);
        if (x->dpy != NULL)
            p_XCloseDisplay(x->dpy);
        free(x);
    }
    free(w->ppm);
    free(w);
}

static int x11_present(HostWindow *w, const uint32_t *fb, int ww, int hh)
{
    X11Win *x = w->x11;
    if (x == NULL || x->dpy == NULL || fb == NULL)
        return -1;
    if (x->img == NULL || x->img->width != ww || x->img->height != hh) {
        if (x->img != NULL)
            p_XDestroyImage(x->img);
        x->img = p_XCreateImage(x->dpy, NULL, 24, 2 /*ZPixmap*/, 0, NULL,
                                (unsigned)ww, (unsigned)hh, 32, 0);
        if (x->img == NULL)
            return -1;
        x->img->data = malloc((size_t)ww * hh * 4);
        if (x->img->data == NULL)
            return -1;
    }
    uint32_t *dst = (uint32_t *)x->img->data;
    for (int i = 0; i < ww * hh; i++)
        dst[i] = fb[i] & 0x00FFFFFFu; /* XImage 24depth ZPixmap: BGRX on little-endian */
    p_XPutImage(x->dpy, x->win, x->gc, x->img, 0, 0, 0, 0, (unsigned)ww, (unsigned)hh);
    p_XSync(x->dpy, 0);
    w->frames++;
    return 0;
}

static int x11_poll(HostWindow *w, HostKeyEvent *ev)
{
    X11Win *x = w->x11;
    if (x == NULL || x->dpy == NULL)
        return 0;
    while (p_XPending(x->dpy) > 0) {
        XEvent e;
        p_XNextEvent(x->dpy, &e);
        if (e.type == ClientMessage) {
            XClientMessageEvent *cm = (XClientMessageEvent *)&e;
            if (cm->message_type == x->wm_delete || (cm->format == 32 && cm->data.l[0] == (long)x->wm_delete))
                x->quit = 1;
            continue;
        }
        if (e.type == KeyPress || e.type == KeyRelease) {
            KeySym ks = p_XLookupKeysym(&e, 0);
            int ok;
            KeySym hk = keysym_to_host_key(ks, &ok);
            if (ok) {
                ev->key = (int)hk;
                ev->press = (e.type == KeyPress);
                return 1;
            }
        }
    }
    return 0;
}

static int x11_should_quit(HostWindow *w)
{
    X11Win *x = w->x11;
    return x != NULL && x->quit;
}

static const HostBackend x11_backend = {
    x11_close, x11_present, x11_poll, x11_should_quit
};

HostWindow *host_x11_open(const char *title, int w, int h)
{
    if (x11 == NULL) {
        x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
        if (x11 == NULL)
            x11 = dlopen("libX11.so", RTLD_NOW | RTLD_GLOBAL);
        if (x11 == NULL)
            return NULL;
#define SYM(name) p_##name = (void *)dlsym(x11, #name); if (p_##name == NULL) return NULL
        SYM(XOpenDisplay); SYM(XCloseDisplay); SYM(XDefaultScreen);
        SYM(XRootWindow); SYM(XCreateSimpleWindow); SYM(XStoreName);
        SYM(XSelectInput); SYM(XMapWindow); SYM(XPending); SYM(XNextEvent);
        SYM(XCreateImage); SYM(XPutImage); SYM(XDestroyImage);
        SYM(XLookupKeysym); SYM(XInternAtom); SYM(XSetWMProtocols); SYM(XSync);
#undef SYM
    }
    Display *dpy = p_XOpenDisplay(NULL);
    if (dpy == NULL)
        return NULL;
    int scr = p_XDefaultScreen(dpy);
    Window root = p_XRootWindow(dpy, scr);
    Window win = p_XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)w, (unsigned)h, 0, 0, 0);
    p_XStoreName(dpy, win, title != NULL ? title : "SOPHIE");
    p_XSelectInput(dpy, win, KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    Atom wm_delete = p_XInternAtom(dpy, "WM_DELETE_WINDOW", 0);
    p_XSetWMProtocols(dpy, win, &wm_delete, 1);
    p_XMapWindow(dpy, win);
    p_XSync(dpy, 0);

    HostWindow *hw = calloc(1, sizeof *hw);
    X11Win *x = calloc(1, sizeof *x);
    if (hw == NULL || x == NULL) {
        free(hw);
        free(x);
        p_XCloseDisplay(dpy);
        return NULL;
    }
    hw->be = &x11_backend;
    hw->width = w;
    hw->height = h;
    x->dpy = dpy;
    x->win = win;
    x->gc = NULL; /* default GC is fine for XPutImage */
    x->wm_delete = wm_delete;
    x->host = hw;
    hw->x11 = x;
    return hw;
}

#endif /* SOPHIE_HOST_X11 */
