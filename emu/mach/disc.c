/*
 * disc.c - raw disc image backend (WP7). See disc.h for the format contract.
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include "disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t size;
    int tracks, sides, spt, sectsize, sectbase;
} DiscGeom;

static const DiscGeom geom_table[] = {
    {  163840u, 40, 1, 16,  256, 0 }, /* ADFS S */
    {  327680u, 80, 1, 16,  256, 0 }, /* ADFS M */
    {  655360u, 80, 2, 16,  256, 0 }, /* ADFS L (old map) */
    {  819200u, 80, 2,  5, 1024, 0 }, /* ADFS D/E (new map) */
    { 1638400u, 80, 2, 10, 1024, 0 }, /* ADFS F */
    { 3276800u, 80, 2, 20, 1024, 0 }, /* ADFS G */
    {  409600u, 80, 2,  5,  512, 1 }, /* DOS 400K */
    {  737280u, 80, 2,  9,  512, 1 }, /* DOS 720K */
    {  368640u, 40, 2,  9,  512, 1 }, /* DOS 360K */
};

int disc_geometry_for_size(uint32_t size, int *tracks, int *sides, int *spt,
                           int *sectsize, int *sectbase)
{
    for (size_t i = 0; i < sizeof geom_table / sizeof geom_table[0]; i++) {
        if (geom_table[i].size == size) {
            *tracks = geom_table[i].tracks;
            *sides = geom_table[i].sides;
            *spt = geom_table[i].spt;
            *sectsize = geom_table[i].sectsize;
            *sectbase = geom_table[i].sectbase;
            return 0;
        }
    }
    return -1;
}

int disc_load(Disc *d, const char *path, int wprot)
{
    memset(d, 0, sizeof *d);
    d->wprot = wprot;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || disc_geometry_for_size((uint32_t)sz, &d->tracks, &d->sides,
                                          &d->spt, &d->sectsize, &d->sectbase) != 0) {
        fclose(f);
        return -1;
    }
    d->data = malloc((size_t)sz);
    if (d->data == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(d->data, 1, (size_t)sz, f) != (size_t)sz) {
        free(d->data);
        memset(d, 0, sizeof *d);
        fclose(f);
        return -1;
    }
    fclose(f);
    d->size = (uint32_t)sz;
    d->present = 1;
    return 0;
}

void disc_unload(Disc *d)
{
    free(d->data);
    memset(d, 0, sizeof *d);
}

int64_t disc_sector_offset(const Disc *d, int track, int side, int sector)
{
    if (!d->present || track < 0 || track >= d->tracks || side < 0 || side >= d->sides)
        return -1;
    int s = sector - d->sectbase;
    if (s < 0 || s >= d->spt)
        return -1;
    int64_t off = ((int64_t)track * d->sides + side) * d->spt + s;
    off *= d->sectsize;
    if (off + d->sectsize > d->size)
        return -1;
    return off;
}

int disc_read_sector(Disc *d, int track, int side, int sector, uint8_t *buf)
{
    int64_t off = disc_sector_offset(d, track, side, sector);
    if (off < 0)
        return -1;
    memcpy(buf, d->data + off, (size_t)d->sectsize);
    return 0;
}

int disc_write_sector(Disc *d, int track, int side, int sector, const uint8_t *buf)
{
    if (d->wprot)
        return -1;
    int64_t off = disc_sector_offset(d, track, side, sector);
    if (off < 0)
        return -1;
    memcpy(d->data + off, buf, (size_t)d->sectsize);
    return 0;
}

int disc_read_address(const Disc *d, int track, int side, uint8_t *buf6)
{
    if (!d->present || track < 0 || track >= d->tracks || side < 0 || side >= d->sides)
        return -1;
    int sizecode = 0;
    for (int s = d->sectsize; s > 128; s >>= 1)
        sizecode++;
    buf6[0] = (uint8_t)track;
    buf6[1] = (uint8_t)side;
    buf6[2] = (uint8_t)d->sectbase; /* first sector on the track */
    buf6[3] = (uint8_t)sizecode;
    buf6[4] = 0xFF; /* fake CRC */
    buf6[5] = 0xFF;
    return 0;
}

int disc_read_track(const Disc *d, int track, int side, uint8_t *buf)
{
    if (!d->present || track < 0 || track >= d->tracks || side < 0 || side >= d->sides)
        return -1;
    int64_t base = ((int64_t)track * d->sides + side) * d->spt * d->sectsize;
    if (base + (int64_t)d->spt * d->sectsize > d->size)
        return -1;
    memcpy(buf, d->data + base, (size_t)d->spt * d->sectsize);
    return 0;
}
