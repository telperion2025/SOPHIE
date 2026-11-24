/*
 * disc.h - raw floppy disc image backend (WP7).
 *
 * A disc image is a flat file of sector data only (the common ADFS .adf /
 * .adl / DOS .img "data only" layout used by Arculator/RPCEmu images).
 * Geometry is inferred from the file size:
 *
 *   163840  ADFS S   40 trk x 1 side x 16 sect x 256B
 *   327680  ADFS M   80 x 1 x 16 x 256
 *   655360  ADFS L   80 x 2 x 16 x 256      (old map, 256-byte sectors)
 *   819200  ADFS D/E 80 x 2 x  5 x 1024     (new map, 1024-byte sectors)
 *  1638400  ADFS F  80 x 2 x 10 x 1024
 *  3276800  ADFS G  80 x 2 x 20 x 1024
 *   409600  DOS 400K 80 x 2 x  5 x 512
 *   737280  DOS 720K 80 x 2 x  9 x 512
 *   368640  DOS 360K 40 x 2 x  9 x 512
 *
 * Sector numbering on the FDC: 1-based for 512-byte sectors (DOS/ADFS-E
 * 512B), 0-based otherwise (256B and 1024B ADFS formats) - matching the
 * studied emulators. Linear layout: offset = ((track * sides + side) *
 * sectors_per_track + (sector - base)) * sector_size, i.e. sides are
 * interleaved within a track on ADFS floppies.
 *
 * Licence: Apache-2.0 (see LICENSE). Format facts per Acorn FileCore/ADFS
 * documentation; studied emulators are reference-and-credit only.
 */
#ifndef SOPHIE_DISC_H
#define SOPHIE_DISC_H

#include <stdint.h>

#define DISC_MAX_DRIVES 4
#define DISC_MAX_SECTOR 1024u
#define DISC_MAX_TRACK_BYTES (20u * 1024u) /* largest track: ADFS G 20x1024 */

typedef struct Disc {
    uint8_t *data;      /* owned image buffer, NULL = no disc */
    uint32_t size;      /* image bytes */
    int      present;
    int      wprot;     /* write protect */
    int      tracks, sides, spt, sectsize, sectbase;
} Disc;

/* Load an image file into disc (caller-owned struct). Returns 0 on success;
 * on failure the disc is left empty (present=0). wprot sets write protect. */
int  disc_load(Disc *d, const char *path, int wprot);
void disc_unload(Disc *d);

/* Look up geometry for an image byte size; returns 0 and fills the fields,
 * or -1 if the size matches no known format. */
int  disc_geometry_for_size(uint32_t size, int *tracks, int *sides, int *spt,
                            int *sectsize, int *sectbase);

/* Byte offset of a sector, or -1 if out of range / no disc. */
int64_t disc_sector_offset(const Disc *d, int track, int side, int sector);

/* Read/write one sector into/out of buf (sectsize bytes). 0 ok, -1 fail. */
int disc_read_sector(Disc *d, int track, int side, int sector, uint8_t *buf);
int disc_write_sector(Disc *d, int track, int side, int sector, const uint8_t *buf);

/* Read-address record: 6 bytes {track, head, sector, sizecode, crc1, crc2}.
 * sizecode: 0=128,1=256,2=512,3=1024 bytes. 0 ok, -1 fail. */
int disc_read_address(const Disc *d, int track, int side, uint8_t *buf6);

/* Read a whole track (spt*sectsize bytes) into buf (>= DISC_MAX_TRACK_BYTES*2).
 * 0 ok, -1 fail. */
int disc_read_track(const Disc *d, int track, int side, uint8_t *buf);

#endif /* SOPHIE_DISC_H */
