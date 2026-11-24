/*
 * keymap.c - host key code -> RISC OS low-level key number (WP5).
 *
 * Key numbers follow the Archimedes (A3000) keyboard matrix layout as
 * modelled in MAME's archimedes_keyb device / the A3000 TRM: the matrix is
 * scanned by column (mux), 16 rows per column, key number = (col << 4) | row.
 * The KART byte carries this number (make); break = number | 0x80.
 * Shifted printable characters map to the same physical key as their
 * unshifted form (RISC OS applies shift internally).
 *
 * Licence: Apache-2.0 (see LICENSE). Layout facts only from the Acorn
 * keyboard matrix documentation (see NOTICE).
 */
#include "host.h"

/* key number = (matrix column << 4) | row bit index */
#define K(col, row) (uint8_t)(((col) << 4) | (row))

static int ascii_to_riscos(int c)
{
    switch (c) {
      case ' ': return K(3, 0);
      case '!': case '1': return K(1, 5);
      case '"': case '\'': return K(10, 2);
      case '#': case '3': return K(3, 6);
      case '$': case '4': return K(4, 4);
      case '%': case '5': return K(4, 5);
      case '&': case '7': return K(5, 4);
      case '(': case ')': case '0': return K(12, 3);
      case '*': return K(9, 3);
      case '+': case '=': return K(9, 5);
      case ',': case '<': return K(6, 1);
      case '-': case '_': return K(11, 4);
      case '.': case '>': return K(10, 1);
      case '/': case '?': return K(10, 0);
      case '2': case '@': return K(2, 6);
      case '6': case '^': return K(5, 5);
      case '8': return K(6, 4);
      case '9': return K(10, 4);
      case ':': case ';': return K(12, 0);
      case '[': case '{': return K(12, 2);
      case '\\': return K(9, 4);
      case ']': case '}': return K(10, 7);
      case '`': case '~': return K(1, 1);
      case 'a': return K(1, 7);
      case 'b': return K(4, 0);
      case 'c': return K(3, 1);
      case 'd': return K(3, 2);
      case 'e': return K(3, 7);
      case 'f': return K(4, 3);
      case 'g': return K(4, 2);
      case 'h': return K(5, 2);
      case 'i': return K(6, 6);
      case 'j': return K(5, 3);
      case 'k': return K(6, 2);
      case 'l': return K(10, 3);
      case 'm': return K(5, 1);
      case 'n': return K(5, 0);
      case 'o': return K(10, 6);
      case 'p': return K(12, 1);
      case 'q': return K(1, 6);
      case 'r': return K(4, 6);
      case 's': return K(2, 2);
      case 't': return K(4, 7);
      case 'u': return K(5, 6);
      case 'v': return K(4, 1);
      case 'w': return K(2, 7);
      case 'x': return K(2, 1);
      case 'y': return K(5, 7);
      case 'z': return K(1, 2);
      default: return -1;
    }
}

int host_key_to_riscos(int host_key)
{
    if (host_key >= 'a' && host_key <= 'z')
        return ascii_to_riscos(host_key);
    if (host_key >= 'A' && host_key <= 'Z')
        return ascii_to_riscos(host_key - 'A' + 'a'); /* same physical key */
    if (host_key >= 0x20 && host_key < 0x7F)
        return ascii_to_riscos(host_key);
    switch (host_key) {
      case HOST_KEY_LEFT:      return K(6, 0);
      case HOST_KEY_RIGHT:     return K(7, 2);
      case HOST_KEY_UP:        return K(7, 7);
      case HOST_KEY_DOWN:      return K(7, 1);
      case HOST_KEY_RETURN:    return K(6, 7);
      case HOST_KEY_ESC:       return K(1, 0);
      case HOST_KEY_BACKSPACE: return K(9, 6);
      case HOST_KEY_DELETE:    return K(8, 4);
      case HOST_KEY_TAB:       return K(1, 3);
      case HOST_KEY_HOME:      return K(13, 3);
      case HOST_KEY_END:       return K(8, 6);
      case HOST_KEY_PGUP:      return K(0, 4);
      case HOST_KEY_PGDN:      return K(8, 7);
      case HOST_KEY_INSERT:    return K(9, 7);
      case HOST_KEY_CAPS:      return K(2, 0);
      case HOST_KEY_F1:        return K(2, 4);
      case HOST_KEY_F2:        return K(2, 3);
      case HOST_KEY_F3:        return K(3, 3);
      case HOST_KEY_F4:        return K(3, 4);
      case HOST_KEY_F5:        return K(6, 5);
      case HOST_KEY_F6:        return K(10, 5);
      case HOST_KEY_F7:        return K(12, 6);
      case HOST_KEY_F8:        return K(12, 7);
      case HOST_KEY_F9:        return K(12, 5);
      case HOST_KEY_F10:       return K(11, 5);
      case HOST_KEY_F11:       return K(11, 6);
      case HOST_KEY_F12:       return K(11, 7);
      case HOST_KEY_LSHIFT:    return K(13, 2);
      case HOST_KEY_RSHIFT:    return K(13, 5);
      case HOST_KEY_LCTRL:     return K(13, 1);
      case HOST_KEY_RCTRL:     return K(13, 6);
      case HOST_KEY_LALT:      return K(13, 0);
      case HOST_KEY_RALT:      return K(13, 7);
      default:                 return -1;
    }
}
