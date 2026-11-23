/*
 * mkadfs.c - WP7 tool: create a blank, formatted ADFS L (large, old map)
 * floppy disc image: 80 tracks x 2 sides x 16 sectors x 256 bytes = 640KB,
 * raw sector data (the common .adl layout).
 *
 * On-disc structures written (per Acorn FileCore "disc map old" /
 * directories documentation):
 *   sector 0: free-space start list (one fragment: sectors 7..2559),
 *             interleaved disc name, disc size, checksum
 *   sector 1: matching free-space lengths, disc id, boot option,
 *             free-list end pointer, checksum
 *   sectors 2-6: empty root directory "$" ("Hugo", old directory format)
 *
 * Usage: tools/mkadfs <out.adf> [disc-name]     (name: up to 10 chars)
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ADFS_L_SIZE   655360u  /* 80*2*16*256 */
#define ADFS_L_SECT   2560u    /* sectors of 256 bytes */
#define ROOT_SECT     2u       /* root directory starts at sector 2 */
#define ROOT_BYTES    1280u    /* 5 x 256 */

/* Old-map checksum: start at 255, add bytes 254 down to 0, folding carries. */
static uint8_t oldmap_checksum(const uint8_t *sec)
{
    uint32_t acc = 255;
    for (int i = 254; i >= 0; i--) {
        acc += sec[i];
        acc = (acc & 0xFFu) + (acc >> 8);
    }
    acc = (acc & 0xFFu) + (acc >> 8);
    return (uint8_t)(acc & 0xFFu);
}

static void put24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out.adf> [disc-name]\n", argv[0]);
        return 2;
    }
    const char *out = argv[1];
    char name[11] = "SOPHIE";
    if (argc > 2) {
        memset(name, ' ', 10);
        name[10] = 0;
        size_t n = strlen(argv[2]);
        if (n > 10)
            n = 10;
        memcpy(name, argv[2], n);
    }

    uint8_t *img = calloc(1, ADFS_L_SIZE);
    if (img == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* sector 0: free-space starts */
    uint8_t *s0 = img;
    put24(s0 + 0, 7);                 /* one free fragment: sectors 7..2559 */
    s0[246] = 0;                      /* reserved */
    for (int i = 0; i < 5; i++)
        s0[247 + i] = (uint8_t)name[2 * i];       /* even chars of disc name */
    put24(s0 + 252, ADFS_L_SECT);     /* disc size in 256-byte sectors */
    s0[255] = oldmap_checksum(s0);

    /* sector 1: free-space lengths */
    uint8_t *s1 = img + 256;
    put24(s1 + 0, ADFS_L_SECT - 7);   /* fragment length */
    for (int i = 0; i < 5; i++)
        s1[246 + i] = (uint8_t)name[2 * i + 1];   /* odd chars of disc name */
    put24(s1 + 251, 0x1234);          /* disc id (2 bytes used) */
    s1[253] = 0;                      /* boot option: off */
    s1[254] = 3;                      /* free-list end pointer: 1 entry x 3 */
    s1[255] = oldmap_checksum(s1);

    /* root directory: 5 sectors at 0x200, old "Hugo" format */
    uint8_t *root = img + ROOT_SECT * 256;
    root[0] = 0;                      /* master sequence number */
    memcpy(root + 1, "Hugo", 4);
    /* entries: bytes 5..1226 stay zero (empty) */
    root[1227] = 0;                   /* end-of-entries marker */
    memcpy(root + 1228, "$         ", 10);   /* directory name */
    put24(root + 1238, ROOT_SECT);    /* parent directory start sector (self) */
    memcpy(root + 1241, "$                   ", 19); /* directory title */
    /* 1260..1273 reserved zero */
    root[1274] = 0;                   /* master sequence number (BCD) */
    memcpy(root + 1275, "Hugo", 4);   /* directory identifier */
    root[1279] = 0;                   /* 32-bit checksum: zero = ignored */

    FILE *f = fopen(out, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot create %s\n", out);
        free(img);
        return 1;
    }
    if (fwrite(img, 1, ADFS_L_SIZE, f) != ADFS_L_SIZE) {
        fprintf(stderr, "short write on %s\n", out);
        fclose(f);
        free(img);
        return 1;
    }
    fclose(f);
    free(img);
    printf("mkadfs: wrote %s (%u bytes, ADFS L 80x2x16x256, name \"%s\", blank)\n",
           out, ADFS_L_SIZE, name);
    return 0;
}
