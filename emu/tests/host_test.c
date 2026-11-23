/*
 * host_test.c - WP5 host window layer tests: headless backend present/poll/
 * quit cycle, PPM snapshot contents, keymap, and input injection into the
 * machine's IOC KART via host_pump_input().
 *
 * Run: ./host_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "test.h"
#include "machine.h"
#include "host.h"

static uint8_t rom[0x800];
static uint8_t ram[0x40000];

int main(void)
{
    char det[160];

    /* 1: open/present/poll/quit cycle on the headless backend */
    {
        HostWindow *w = host_open("test", 4, 2);
        check("host-open", w != NULL, "");
        uint32_t fb[8] = { 0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu,
                           0x000000u, 0x123456u, 0xAABBCCu, 0xDDEEFFu };
        snprintf(det, sizeof det, "rc=%d", host_present(w, fb, 4, 2));
        check("host-present", host_present(w, fb, 4, 2) == 0 && w->frames == 2, det);
        HostKeyEvent ev;
        check("host-poll-empty", host_poll(w, &ev) == 0, "");
        host_headless_push_key(w, 'a', 1);
        host_headless_push_key(w, 'a', 0);
        int got = host_poll(w, &ev) && ev.key == 'a' && ev.press == 1
               && host_poll(w, &ev) && ev.key == 'a' && ev.press == 0
               && host_poll(w, &ev) == 0;
        check("host-poll-queue-order", got, "");
        check("host-quit-flag", host_should_quit(w) == 0, "");
        host_headless_set_quit(w, 1);
        check("host-quit-set", host_should_quit(w) == 1, "");
        host_close(w);
    }

    /* 2: PPM snapshot contents (P6 header + RGB bytes) */
    {
        const char *path = "/tmp/sophie_host_test.ppm";
        HostWindow *w = host_open("test", 2, 2);
        host_headless_set_ppm(w, path);
        uint32_t fb[4] = { 0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu };
        host_present(w, fb, 2, 2);
        host_close(w);
        FILE *f = fopen(path, "rb");
        check("host-ppm-exists", f != NULL, path);
        if (f != NULL) {
            char hdr[12];
            size_t n = fread(hdr, 1, 11, f); /* "P6\n2 2\n255\n" */
            hdr[n] = 0;
            uint8_t px[12];
            size_t got = fread(px, 1, 12, f);
            fclose(f);
            int ok = strncmp(hdr, "P6\n2 2\n255\n", 11) == 0 && got == 12
                  && px[0] == 0xFF && px[1] == 0x00 && px[2] == 0x00
                  && px[3] == 0x00 && px[4] == 0xFF && px[5] == 0x00
                  && px[6] == 0x00 && px[7] == 0x00 && px[8] == 0xFF
                  && px[9] == 0xFF && px[10] == 0xFF && px[11] == 0xFF;
            snprintf(det, sizeof det, "hdr=%.11s got=%zu", hdr, got);
            check("host-ppm-pixels", ok, det);
        }
    }

    /* 3: keymap sanity (A3000 matrix: key = (col<<4)|row) */
    {
        snprintf(det, sizeof det, "a=%02x", host_key_to_riscos('a'));
        check("keymap-letter", host_key_to_riscos('a') == 0x17 && host_key_to_riscos('A') == 0x17, det);
        snprintf(det, sizeof det, "ret=%02x esc=%02x left=%02x",
                 host_key_to_riscos(HOST_KEY_RETURN), host_key_to_riscos(HOST_KEY_ESC),
                 host_key_to_riscos(HOST_KEY_LEFT));
        check("keymap-specials",
              host_key_to_riscos(HOST_KEY_RETURN) == 0x67
           && host_key_to_riscos(HOST_KEY_ESC) == 0x10
           && host_key_to_riscos(HOST_KEY_LEFT) == 0x60
           && host_key_to_riscos(0x7F) == -1, det);
    }

    /* 4: host_pump_input injects KART make/break into the machine's IOC */
    {
        Machine m;
        machine_init(&m, ram, sizeof ram, rom, sizeof rom);
        HostWindow *w = host_open("test", 8, 8);
        host_headless_push_key(w, 'a', 1);   /* make */
        host_headless_push_key(w, 'a', 0);   /* break */
        host_pump_input(w, &m);
        uint8_t stb = mem_rd8(&m.bus, 0x03200000u + IOC_IRQ_STATUS_B);
        uint8_t k1 = mem_rd8(&m.bus, 0x03200000u + IOC_KART); /* read pops the make */
        uint8_t k2 = mem_rd8(&m.bus, 0x03200000u + IOC_KART); /* ... and the break */
        snprintf(det, sizeof det, "IRB=%02x k1=%02x k2=%02x", stb, k1, k2);
        check("host-input-to-kart",
              (stb & IOC_IRB_KART_RX_FULL) != 0 && k1 == 0x17 && k2 == (0x17 | 0x80), det);
        host_close(w);
    }

    return test_summary("host") ? 1 : 0;
}
