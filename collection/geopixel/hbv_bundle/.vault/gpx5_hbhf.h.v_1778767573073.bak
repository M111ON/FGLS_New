#pragma once
/*
 * gpx5_hbhf.h — append / read HBHF trailer in a .gpx5 file
 *
 * Strategy (Option C):
 *   - HBHF 64B block appended at end of cycle-0 .gpx5 file
 *   - flags byte (offset 5) gets GPX5_FLAG_HBHF bit set
 *   - reader: if flag set → fseek(-64, SEEK_END) → hbhf_read()
 *
 * No re-layout. Backward-compatible (old readers ignore tail bytes).
 */

#include <stdio.h>
#include <stdint.h>
#include "hb_header_frame.h"

#define GPX5_FLAG_HBHF  0x04u   /* bit2 — HBHF trailer present at EOF-64 */
#define GPX5_HDR_FLAGS_OFF  5u  /* byte offset of flags in file           */

/* ── append 64B HBHF + patch flags byte ── */
static inline int gpx5_append_hbhf(const char *path, const HbHeaderFrame *hf)
{
    if (!path || !hf) return -1;

    /* serialise */
    uint8_t buf[HBHF_SZ];
    hbhf_write(buf, hf);

    /* append */
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    if (fwrite(buf, 1, HBHF_SZ, f) != HBHF_SZ) { fclose(f); return -1; }

    /* patch flags byte at offset 5 */
    if (fseek(f, (long)GPX5_HDR_FLAGS_OFF, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t flags = 0;
    if (fread(&flags, 1, 1, f) != 1) { fclose(f); return -1; }
    flags |= GPX5_FLAG_HBHF;
    if (fseek(f, (long)GPX5_HDR_FLAGS_OFF, SEEK_SET) != 0) { fclose(f); return -1; }
    if (fwrite(&flags, 1, 1, f) != 1) { fclose(f); return -1; }

    fclose(f);
    return 0;
}

/* ── read HBHF trailer (returns -1 if flag not set or corrupt) ── */
static inline int gpx5_read_hbhf(const char *path, HbHeaderFrame *out)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* check flag */
    if (fseek(f, (long)GPX5_HDR_FLAGS_OFF, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t flags = 0;
    if (fread(&flags, 1, 1, f) != 1) { fclose(f); return -1; }
    if (!(flags & GPX5_FLAG_HBHF)) { fclose(f); return -2; }  /* not present */

    /* seek to trailer */
    if (fseek(f, -(long)HBHF_SZ, SEEK_END) != 0) { fclose(f); return -1; }
    uint8_t buf[HBHF_SZ];
    if (fread(buf, 1, HBHF_SZ, f) != HBHF_SZ) { fclose(f); return -1; }
    fclose(f);

    return hbhf_read(buf, out);  /* -1=bad magic, -2=ver, -3=crc */
}
